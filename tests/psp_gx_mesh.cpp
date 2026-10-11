// Native GU command recorder: verify the backend against the old per-vertex
// transform, including the retail title lists when extracted data is present.
#include "../src/port/psp/PspGxBackend.cpp"
namespace std { using ::sqrtf; }
#include "../src/plugPikiYamashita/P2DPicture.cpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>
#include <chrono>

namespace PikminPsp {
void performanceAdd(PerformanceMetric, u32) {}
}

namespace {
alignas(16) u8 memory[8 * 1024 * 1024];
u32 used;
u32 drawCalls;
u32 modelMatrixUploads;
u32 deferredDrawCalls;
u32 rasterCalls;
bool guEnabled[32];
int guDepthMask, guDepthFunction, guFrontFace;
u32 guBlend[5], guAlpha[3], guPixelMask;
bool recordGeometry = true;
ScePspFMatrix4 modelMatrix;
ScePspFMatrix4 bones[8];
std::vector<PspGxVertex> triangles;
struct MovieVertex { u16 u, v; s16 x, y, z; };
std::vector<MovieVertex> sprites;
const void* moviePixels;
int movieFormat, movieStride, movieWrapU, movieWrapV;
u32 pictureResidentCalls;
u32 meshClockReads;

void transform(PspGxVertex& vertex, const ScePspFMatrix4& matrix)
{
	const f32* m = reinterpret_cast<const f32*>(&matrix);
	f32 x = vertex.x, y = vertex.y, z = vertex.z;
	vertex.x = m[0] * x + m[4] * y + m[8] * z + m[12];
	vertex.y = m[1] * x + m[5] * y + m[9] * z + m[13];
	vertex.z = m[2] * x + m[6] * y + m[10] * z + m[14];
}

void resetDraws()
{
	used = drawCalls = deferredDrawCalls = 0;
	sCachedDrawBatch = 0;
	sState.guPositionMatrixValid = false;
	triangles.clear();
	sprites.clear();
	modelMatrix = positionMatrix(sState.positionMatrices[sState.currentPositionMatrix]);
}

void resetState()
{
	memset(&sState, 0, sizeof(sState));
	sState.guInitialised = true;
	sState.transform3D = true;
	sState.currentColour = GU_RGBA(51, 102, 204, 170);
	for (u32 i = 0; i < 10; ++i) {
		Mtx& m = sState.positionMatrices[i];
		m[0][0] = 1.0f + i * 0.1f; m[0][1] = 0.2f; m[0][3] = i * 3.0f;
		m[1][0] = -0.3f; m[1][1] = 0.7f; m[1][3] = 4.0f;
		m[2][1] = 0.5f; m[2][2] = 0.8f; m[2][3] = -100.0f;
	}
}

void referenceList(const u8* input, const u8* end)
{
	while (input < end) {
		u8 command = *input++;
		if (!command) continue;
		u32 count = readBig16(input);
		GXBegin((GXPrimitive)(command & GX_OPCODE_MASK), GX_VTXFMT0, count);
		sState.verticesPretransformed = true;
		for (u32 i = 0; i < count; ++i) decodeMeshVertex(input);
		PspGXEnd();
	}
	assert(input == end);
}

void checkMeshCacheTags()
{
	for (u32 set = 0; set < 256; ++set) for (u32 way = 0; way < 8; ++way) {
		const auto& entry = sMeshCache[set][way];
		const auto& tag = sMeshCacheTags[set][way];
		if (!entry.storage) { assert(!tag.list && !tag.bytes); continue; }
		assert(tag.list == entry.list && tag.bytes == entry.listBytes);
		assert(meshCacheSet(tag.list) == set);
		assert(&meshCacheTag(sMeshCache[set][way]) == &tag);
	}
}

void equalVertex(const PspGxVertex& a, const PspGxVertex& b)
{
	assert(a.colour == b.colour);
	if (a.u != b.u || a.v != b.v)
		fprintf(stderr, "UV mismatch: got %.9g,%.9g expected %.9g,%.9g (source %u, matrix %u, refreshes %llu)\n",
		        a.u, a.v, b.u, b.v, (u32)sState.texSource, sState.texMatrix, (unsigned long long)sTimings.texCoordRefreshes);
	assert(a.u == b.u && a.v == b.v);
	assert(fabsf(a.x - b.x) < 0.005f);
	assert(fabsf(a.y - b.y) < 0.005f);
	assert(fabsf(a.z - b.z) < 0.005f);
}

u32 compareList(const u8* input, u32 size)
{
	resetDraws();
	referenceList(input, input + size);
	std::vector<PspGxVertex> expected = triangles;
	u32 oldDraws = drawCalls;
	resetDraws();
	assert(drawMeshList(input, input + size));
	assert(drawCalls == 1);
	assert(triangles.size() == expected.size());
	for (u32 i = 0; i < triangles.size(); ++i) equalVertex(triangles[i], expected[i]);
	// A second submission exercises the persistent cache against the same
	// reference geometry, rather than merely checking that a cache hit occurred.
	resetDraws();
	assert(drawMeshList(input, input + size));
	assert(drawCalls == 1 && triangles.size() == expected.size());
	for (u32 i = 0; i < triangles.size(); ++i) equalVertex(triangles[i], expected[i]);
	// Consecutive cache hits must preserve command order and geometry while
	// only one in sixteen needs a direct SDK draw/stall update.
	if (sTimings.meshCacheHits && !expected.empty()) {
		u64 hits = sTimings.meshCacheHits;
		u64 refreshes = sTimings.texCoordRefreshes;
		u64 compiled = sTimings.compiledMeshCalls;
		resetDraws();
		for (u32 draw = 0; draw < 33; ++draw) {
			triangles.clear();
			assert(drawMeshList(input, input + size));
			assert(triangles.size() == expected.size());
			for (u32 i = 0; i < triangles.size(); ++i) equalVertex(triangles[i], expected[i]);
		}
		if (sTimings.meshCacheHits - hits == 33 && sTimings.texCoordRefreshes == refreshes) {
			assert(deferredDrawCalls == 30);
			assert(sTimings.compiledMeshCalls - compiled == (sState.transform3D ? 30 : 0));
		}
	}
	checkMeshCacheTags();
	return oldDraws;
}

void syntheticLists()
{
	resetState();
	f32 positions[][3] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}, {-1, 0.5f, 1}};
	f32 uv[][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}, {0.5f, 0.5f}};
	GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
	GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
	GXSetVtxDesc(GX_VA_TEX0, GX_INDEX16);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	GXSetArray(GX_VA_TEX0, uv, sizeof(uv[0]));
	for (bool mixed : {false, true}) {
		std::vector<u8> list;
		for (GXPrimitive prim : {GX_TRIANGLESTRIP, GX_TRIANGLEFAN, GX_QUADS, GX_TRIANGLES}) {
			u8 count = prim == GX_TRIANGLES ? 3 : prim == GX_QUADS ? 4 : 5;
			list.insert(list.end(), {u8(prim), 0, count});
			for (u8 i = 0; i < count; ++i) {
				list.insert(list.end(), {u8(mixed ? (i % 3) * 3 : 6), 0, i, 0, i});
			}
			list.push_back(0);
		}
		assert(compareList(list.data(), list.size()) == 4);
		resetDraws();
		assert(!drawMeshList(list.data(), list.data() + list.size() - 2));
		assert(drawCalls == 0 && used == 0);
	}
	// Missing PNMTXIDX uses the selected GX matrix, including slots 8 and 9.
	GXSetVtxDesc(GX_VA_PNMTXIDX, GX_NONE);
	sState.currentPositionMatrix = 9;
	u8 list[] = {GX_TRIANGLES, 0, 3, 0, 0, 0, 0, 0, 1, 0, 1, 0, 2, 0, 2};
	compareList(list, sizeof(list));
}

void meshCacheInvalidation()
{
	clearMeshCache();
	resetState();
	f32 positions[][3] = {{0,0,0},{1,0,0},{0,1,0}};
	f32 uv[][2] = {{0,0},{1,0},{0,1}};
	f32 normals[][3] = {{0,0,1},{0,1,0},{1,0,0}};
	u32 colours[] = {0xff102030,0x80405060,0x40708090};
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxDesc(GX_VA_NRM, GX_INDEX8);
	GXSetVtxDesc(GX_VA_CLR0, GX_INDEX8);
	GXSetVtxDesc(GX_VA_TEX0, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	GXSetArray(GX_VA_NRM, normals, sizeof(normals[0]));
	GXSetArray(GX_VA_CLR0, colours, sizeof(colours[0]));
	GXSetArray(GX_VA_TEX0, uv, sizeof(uv[0]));
	u8 list[] = {GX_TRIANGLES,0,3, 0,0,0,0, 1,1,1,1, 2,2,2,2};
	compareList(list, sizeof(list));
	u64 decoded = sTimings.decodedVertices;
	sState.positionMatrices[0][0][3] += 20;
	compareList(list, sizeof(list));
	assert(sTimings.decodedVertices == decoded); // GE matrix animation needs no conversion.
	positions[1][0] = 7; compareList(list, sizeof(list));
	uv[1][1] = 0.75f; compareList(list, sizeof(list));
	colours[2] = 0xaabbccdd; compareList(list, sizeof(list));
	list[3] = 2; compareList(list, sizeof(list));
	sState.currentPositionMatrix = 9; compareList(list, sizeof(list));
	GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_C0, GX_CC_RASC, GX_CC_ZERO);
	GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
	GXSetTevColorS10(GX_TEVREG0, (GXColorS10){255,100,50,255}); compareList(list, sizeof(list));
	GXSetTevColorS10(GX_TEVREG0, (GXColorS10){50,100,255,255}); compareList(list, sizeof(list));
	Mtx reflection = {{0.5f,0,0,0.5f},{0,0.5f,0,0.5f},{0,0,0,1}};
	GXLoadTexMtxImm(reflection, GX_TEXMTX0, GX_MTX2x4);
	GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_NRM, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
	compareList(list, sizeof(list));
	normals[0][0] = 0.5f; compareList(list, sizeof(list));
	reflection[0][3] = 0.25f;
	GXLoadTexMtxImm(reflection, GX_TEXMTX0, GX_MTX2x4); compareList(list, sizeof(list));
	// Pointer reuse with changed bytes and changed array bindings must miss.
	f32 replacement[][3] = {{2,3,4},{5,6,7},{8,9,10}};
	GXSetArray(GX_VA_POS, replacement, sizeof(replacement[0])); compareList(list, sizeof(list));
	meshCacheGpuIdle();
	compareList(list, sizeof(list));
	MeshCacheEntry* pinned = findCachedMesh(list, sizeof(list));
	assert(pinned && pinned->gpuEpoch == sMeshCacheEpoch);
	const u8* oldStorage = pinned->storage;
	std::vector<u8> oldBytes(oldStorage, oldStorage + pinned->allocationBytes);
	// Multiple changes before any GPU sync fill the associative set. The
	// fallback may draw uncached, but must not overwrite the pinned allocation.
	for (u32 i = 0; i < 8; ++i) {
		replacement[1][0] += 1;
		compareList(list, sizeof(list));
		assert(pinned->storage == oldStorage);
		assert(!memcmp(oldStorage, oldBytes.data(), oldBytes.size()));
	}
	meshCacheGpuIdle();
	compareList(list, sizeof(list));
	assert(findCachedMesh(list, sizeof(list)));
	assert(sMeshCacheBytes > 0 && sMeshCacheBytes <= kMeshCacheLimit);
	clearMeshCache();
	assert(sMeshCacheBytes == 0);
	checkMeshCacheTags();
}

void sharedMeshArrays()
{
	clearMeshCache(); resetState();
	f32 positions[][3] = {{0,0,0},{1,0,0},{0,1,0}};
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	u8 list[] = {GX_TRIANGLES,0,3,0,1,2};
	u64 firstGeneration;
	{
		PikminPsp::MeshValidationScope scope;
		PikminPsp::validateMeshArray(GX_VA_POS, positions, sizeof(positions));
		compareList(list, sizeof(list));
		auto* entry = findCachedMesh(list, sizeof(list));
		assert(entry && entry->inputs[0].generation);
		firstGeneration = entry->inputs[0].generation;
		u64 decoded = sTimings.decodedVertices;
		sState.positionMatrices[0][0][3] += 4;
		compareList(list, sizeof(list));
		assert(sTimings.decodedVertices == decoded);
		// Nested scopes restore the outer bindings.
		{ PikminPsp::MeshValidationScope nested; assert(sMeshValidationScope == &nested); }
		assert(sMeshValidationScope == &scope);
	}
	assert(!sMeshValidationScope);
	positions[1][0] = 8;
	meshCacheGpuIdle();
	{
		PikminPsp::MeshValidationScope scope;
		PikminPsp::validateMeshArray(GX_VA_POS, positions, sizeof(positions));
		compareList(list, sizeof(list));
		auto* entry = findCachedMesh(list, sizeof(list));
		assert(entry && entry->inputs[0].generation != firstGeneration);
	}
	// Outside a validated model scope, direct writes still use byte snapshots.
	positions[2][1] = 5;
	meshCacheGpuIdle(); compareList(list, sizeof(list));
	positions[2][1] = 9;
	meshCacheGpuIdle(); compareList(list, sizeof(list));
	{
		PikminPsp::MeshValidationScope scope;
		// An undersized registration must not bypass checks for other vertices.
		PikminPsp::validateMeshArray(GX_VA_POS, positions, sizeof(positions[0]));
		meshCacheGpuIdle(); compareList(list, sizeof(list));
		positions[1][2] = 7;
		meshCacheGpuIdle(); compareList(list, sizeof(list));
	}
	clearMeshCache();
	assert(sMeshArrayBytes == 0);
}

void revisionedMeshes()
{
	clearMeshCache(); resetState();
	f32 positions[][3] = {{0,0,0},{1,0,0},{0,1,0}};
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	u8 list[] = {GX_TRIANGLES,0,3,0,1,2};
	u64 revision = PikminPsp::nextMeshRevision();
	{
		PikminPsp::MeshValidationScope scope;
		PikminPsp::bindMeshRevision(revision);
		compareList(list, sizeof(list));
		auto* entry = findCachedMesh(list, sizeof(list));
		assert(entry && entry->allocationBytes == kMeshCacheDataOffset + meshCacheAlignedSize(entry->nativeBytes) + kMeshCacheCommandBytes);
		assert(sMeshArrayBytes == 0); // No source snapshots allocated.
		u64 decoded = sTimings.decodedVertices;
		sState.positionMatrices[0][0][3] += 7;
		compareList(list, sizeof(list));
		assert(sTimings.decodedVertices == decoded);
		const u8* pinned = entry->storage;
		std::vector<u8> before(pinned, pinned + entry->allocationBytes);
		positions[1][0] = 8;
		PikminPsp::bindMeshRevision(PikminPsp::nextMeshRevision());
		compareList(list, sizeof(list));
		assert(sTimings.decodedVertices > decoded);
		assert(entry->storage == pinned && !memcmp(pinned, before.data(), before.size()));
		list[3] = 2;
		PikminPsp::bindMeshRevision(PikminPsp::nextMeshRevision());
		compareList(list, sizeof(list));
		// Same addresses reused by a new model must not reuse old geometry.
		positions[2][1] = 9;
		{
			PikminPsp::MeshValidationScope nested;
			PikminPsp::bindMeshRevision(PikminPsp::nextMeshRevision());
			compareList(list, sizeof(list));
		}
		assert(sMeshValidationScope == &scope);
	}
	// An unscoped draw cannot accept the revision-only cache entry.
	positions[1][1] = 4;
	compareList(list, sizeof(list));
	clearMeshCache();
	assert(PikminPsp::nextMeshRevision() > revision); // Lifetime IDs survive cache resets.
}

void animatedCachedMeshes()
{
	clearMeshCache(); resetState();
	f32 positions[][3] = {{0,0,0},{1,0,0},{0,1,0}};
	GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	// Nonconsecutive slots, including the highest GX slot, must preserve
	// their mapping onto GE bones on both direct and deferred cache hits.
	u8 list[] = {GX_TRIANGLES,0,3, 0,0, 12,1, 27,2};
	sState.texSource = GX_TG_TEX0;
	compareList(list, sizeof(list));
	u64 decoded = sTimings.decodedVertices;
	for (u32 frame = 0; frame < 3; ++frame) {
		meshCacheGpuIdle();
		sState.positionMatrices[4][0][3] += 3;
		sState.positionMatrices[9][1][3] -= 2;
		compareList(list, sizeof(list));
		assert(sTimings.decodedVertices == decoded);
	}
	// Unversioned edits must still invalidate the complete weighted buffer.
	positions[1][0] += 2;
	compareList(list, sizeof(list));
	assert(sTimings.decodedVertices == decoded + 3);
	list[5] = 3;
	compareList(list, sizeof(list));
	assert(sTimings.decodedVertices == decoded + 6);
	clearMeshCache();
}

void meshMaterialVariants()
{
	clearMeshCache(); resetState();
	f32 positions[][3] = {{0,0,0},{1,0,0},{0,1,0}};
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	u8 list[] = {GX_TRIANGLES,0,3,0,1,2};
	PikminPsp::MeshValidationScope scope;
	PikminPsp::bindMeshRevision(PikminPsp::nextMeshRevision());
	Mtx projection = {{0.5f,0,0,0.5f},{0,0.5f,0,0.5f},{0,0,0,1}};
	u64 decoded = sTimings.decodedVertices;
	for (u32 frame = 0; frame < 20; ++frame) {
		// Animated projection precedes the ordinary material pass. It must
		// not replace that pass solely because both use the same list address.
		meshCacheGpuIdle();
		projection[0][3] += 0.125f;
		GXLoadTexMtxImm(projection, GX_TEXMTX0, GX_MTX3x4);
		GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_POS, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
		compareList(list, sizeof(list));
		u64 afterProjection = sTimings.decodedVertices;
		// Updating an unrelated texture matrix must not invalidate this pass.
		GXLoadTexMtxImm(projection, GX_TEXMTX1, GX_MTX3x4);
		compareList(list, sizeof(list));
		assert(sTimings.decodedVertices == afterProjection);
		GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
		compareList(list, sizeof(list));
		assert(sTimings.decodedVertices == afterProjection + (frame == 0 ? 3 : 0));
	}
	assert(sTimings.decodedVertices == decoded + 6); // Two geometry variants, regardless of matrix animation.
	// Nonzero uniform selectors use the packed single-matrix key too.
	GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_POS, GX_TEXMTX1, GX_FALSE, GX_PTIDENTITY);
	compareList(list, sizeof(list));
	decoded = sTimings.decodedVertices;
	projection[1][3] += 0.5f;
	GXLoadTexMtxImm(projection, GX_TEXMTX0, GX_MTX3x4);
	compareList(list, sizeof(list));
	assert(sTimings.decodedVertices == decoded);
	GXLoadTexMtxImm(projection, GX_TEXMTX1, GX_MTX3x4);
	compareList(list, sizeof(list));
	assert(sTimings.decodedVertices == decoded);
	// Per-vertex selectors still require every selectable texture matrix.
	GXSetVtxDesc(GX_VA_TEX0MTXIDX, GX_DIRECT);
	u8 selected[] = {GX_TRIANGLES,0,3, GX_TEXMTX0,0, GX_TEXMTX1,1, GX_TEXMTX0,2};
	compareList(selected, sizeof(selected));
	decoded = sTimings.decodedVertices;
	projection[0][3] += 0.5f;
	GXLoadTexMtxImm(projection, GX_TEXMTX1, GX_MTX3x4);
	compareList(selected, sizeof(selected));
	assert(sTimings.decodedVertices == decoded);
	clearMeshCache();
}

void animatedTexCoordMeshes()
{
	u64 conversions = 0, refreshes = sTimings.texCoordRefreshes;
	for (bool skinned : {false, true}) for (bool selected : {false, true})
	for (GXTexGenSrc source : {GX_TG_POS, GX_TG_NRM}) for (bool projected : {false, true}) {
		clearMeshCache(); resetState();
		f32 positions[][3] = {{1,2,3},{4,5,6},{7,8,9}};
		f32 normals[][3] = {{0,0,1},{0,1,0},{1,0,0}};
		GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
		GXSetVtxDesc(GX_VA_TEX0MTXIDX, selected ? GX_DIRECT : GX_NONE);
		GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
		GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
		GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
		GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
		GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
		GXSetArray(GX_VA_NRM, normals, sizeof(normals[0]));
		std::vector<u8> list = {GX_TRIANGLES,0,3};
		for (u8 i = 0; i < 3; ++i) {
			list.push_back(skinned ? (i == 2 ? 27 : i * 12) : 6);
			if (selected) list.push_back(i == 2 ? GX_IDENTITY : GX_TEXMTX0 + i * 3);
			list.insert(list.end(), {0,i,0,i});
		}
		Mtx matrix = {{0.5f,0.25f,0,0.5f},{0,0.5f,0.25f,0.5f},{0,0,0,1}};
		GXLoadTexMtxImm(matrix, GX_TEXMTX0, GX_MTX3x4);
		GXLoadTexMtxImm(matrix, GX_TEXMTX1, GX_MTX3x4);
		GXSetTexCoordGen2(GX_TEXCOORD0, projected ? GX_TG_MTX3X4 : GX_TG_MTX2X4,
		                 source, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
		u64 decoded = sTimings.decodedVertices;
		compareList(list.data(), list.size());
		conversions += sTimings.decodedVertices - decoded;
		assert(sTimings.decodedVertices == decoded + 3);
		auto* entry = findCachedMesh(list.data(), list.size());
		assert(entry && !findCachedMesh(list.data(), list.size() - 1));
		const u8* storage = entry->storage;
		std::vector<u8> original(storage, storage + entry->allocationBytes);
		for (u32 frame = 0; frame < 20; ++frame) {
			meshCacheGpuIdle();
			matrix[0][3] += 0.0625f;
			matrix[1][0] -= 0.03125f;
			matrix[2][3] = frame % 3 == 0 ? 0.0f : 0.5f; // Projective division and zero-q handling.
			GXLoadTexMtxImm(matrix, GX_TEXMTX0, GX_MTX3x4);
			matrix[1][3] += 0.125f;
			GXLoadTexMtxImm(matrix, GX_TEXMTX1, GX_MTX3x4);
			sState.positionMatrices[9][0][3] += 1;
			u64 compiled = sTimings.compiledMeshCalls;
			compareList(list.data(), list.size());
			assert(sTimings.compiledMeshCalls == compiled); // Refreshed vertices are transient.
			assert(sTimings.decodedVertices == decoded + 3);
			assert(entry->storage == storage && !memcmp(storage, original.data(), original.size()));
			assert(entry->gpuEpoch == sMeshCacheEpoch); // Persistent indices remain GPU-pinned.
		}
		// Real source edits still require conversion, even after a matrix-only hit.
		if (source == GX_TG_NRM) normals[1][0] += 0.5f;
		else positions[1][0] += 0.5f;
		compareList(list.data(), list.size());
		assert(sTimings.decodedVertices == decoded + 6);
		assert(!memcmp(storage, original.data(), original.size()));
		clearMeshCache(); checkMeshCacheTags();
	}
	printf("Animated projection/reflection: %llu initial conversions across 16 variants; %llu UV refresh draws without geometry rebuilds\n",
	       (unsigned long long)conversions, (unsigned long long)(sTimings.texCoordRefreshes - refreshes));
	// Auxiliary UV inputs must not turn a previously supported large mesh
	// into a per-primitive fallback merely because they exceed the GU list.
	clearMeshCache(); resetState(); resetDraws();
	f32 positions[][3] = {{1,2,3},{4,5,6},{7,8,9}};
	GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	std::vector<u8> large = {GX_TRIANGLES, u8(30000 >> 8), u8(30000)};
	for (u32 i = 0; i < 30000; ++i) large.insert(large.end(), {u8((i & 1) * 3), u8(i % 3)});
	Mtx matrix = {{0.5f,0,0,0.5f},{0,0.5f,0,0.5f},{0,0,0,1}};
	GXLoadTexMtxImm(matrix, GX_TEXMTX0, GX_MTX3x4);
	GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2X4, GX_TG_POS, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
	assert(drawMeshList(large.data(), large.data() + large.size()));
	auto* entry = findCachedMesh(large.data(), large.size());
	assert(entry && !entry->hasTexCoordInputs && entry->vertexCount == 30000);
	matrix[0][3] += 1;
	GXLoadTexMtxImm(matrix, GX_TEXMTX0, GX_MTX3x4);
	assert(!findCachedMesh(large.data(), large.size())); // Full validation remains for this path.
	checkMeshCacheTags(); clearMeshCache();
}

void movieFrame()
{
	resetState(); resetDraws();
	static u16 pixels[512 * 512];
	PikminPsp::drawMovieFrame(pixels, 360, 270, 512);
	assert(drawCalls == 0); // No commands outside an active frame.
	sState.listOpen = true;
	PikminPsp::drawMovieFrame(pixels, 360, 270, 512);
	assert(drawCalls == 6 && sprites.size() == 12);
	assert(moviePixels == pixels && movieFormat == GU_PSM_5650 && movieStride == 512);
	assert(movieWrapU == GU_CLAMP && movieWrapV == GU_CLAMP);
	for (u32 i = 0; i < sprites.size(); i += 2) {
		const auto& a = sprites[i]; const auto& b = sprites[i + 1];
		assert(a.u == i / 2 * 64 && b.u == MIN(a.u + 64, 360));
		assert(a.v == 0 && b.v == 270);
		assert(a.x == a.u + 60 && b.x == b.u + 60);
		assert(a.y == 1 && b.y == 271 && a.z == 0 && b.z == 0);
	}
	sState.listOpen = false;
}

struct Reader {
	std::vector<u8> data;
	u32 pos = 0;
	u32 read32() { const u8* p = data.data() + pos; pos += 4; assert(pos <= data.size()); return readBig32(p); }
	u16 read16() { const u8* p = data.data() + pos; pos += 2; assert(pos <= data.size()); return readBig16(p); }
	f32 readFloat() { u32 bits = read32(); f32 value; memcpy(&value, &bits, 4); return value; }
	void align() { pos = (pos + 31) & ~31U; }
};

struct ModelDraw { const u8* list; u32 bytes; PspGxState state; u32 arrayBytes[GX_VA_MAX_ATTR]; };
struct RecordedModel {
	std::vector<u8> data;
	std::vector<std::array<f32,3>> positions, normals;
	std::vector<std::array<f32,2>> uv[8];
	std::vector<u32> colours;
	std::vector<ModelDraw> draws;
};
std::vector<RecordedModel> recordedModels;

void benchmarkModels(const std::vector<ModelDraw>& modelDraws)
{
	// Replay entire model frames, so the benchmark exposes capacity misses
	// instead of repeatedly warming only one draw in an otherwise empty cache.
	recordGeometry = false;
	double milliseconds[2];
	for (bool cached : {false, true}) {
		clearMeshCache();
		const u64 revision = PikminPsp::nextMeshRevision();
		u64 beforeDecoded = 0, beforeHits = 0;
		std::chrono::steady_clock::time_point start;
		for (u32 frame = 0; frame < 13; ++frame) {
			meshCacheGpuIdle();
			if (frame == 1) {
				start = std::chrono::steady_clock::now();
				beforeDecoded = sTimings.decodedVertices; beforeHits = sTimings.meshCacheHits;
			}
			PikminPsp::MeshValidationScope validation;
			PikminPsp::bindMeshRevision(revision);
			for (const auto& draw : modelDraws) {
				sState = draw.state; resetDraws();
				assert(drawMeshList(draw.list, draw.list + draw.bytes, cached));
			}
		}
		milliseconds[cached] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 12;
		printf("  %s: native conversion %.3f ms/frame, decoded %.0f, cache hits %.0f, cache bytes %u\n",
		       cached ? "cached" : "uncached", milliseconds[cached],
		       (sTimings.decodedVertices - beforeDecoded) / 12.0, (sTimings.meshCacheHits - beforeHits) / 12.0, sMeshCacheBytes);
	}
	printf("  native conversion ratio: %.2fx (not PSP FPS)\n", milliseconds[0] / milliseconds[1]);
	recordGeometry = true;
	clearMeshCache();
}

void modelTest(const char* path)
{
	std::ifstream file(path, std::ios::binary);
	assert(file.good());
	Reader r;
	r.data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	std::vector<std::array<f32, 3>> positions;
	std::vector<std::array<f32, 3>> normals;
	std::vector<std::array<f32, 2>> uv[8];
	std::vector<u32> colours;
	u32 oldDraws = 0, newDraws = 0;
	std::vector<ModelDraw> modelDraws;
	resetState();
	while (r.pos + 8 <= r.data.size()) {
		u32 chunk = r.read32(), length = r.read32(), next = r.pos + length;
		if (chunk == 0xffff) break;
		if (chunk == 0x10) {
			positions.resize(r.read32()); r.align();
			for (auto& p : positions) for (auto& v : p) v = r.readFloat();
			GXSetArray(GX_VA_POS, positions.data(), 12);
			GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
		} else if (chunk == 0x11) {
			normals.resize(r.read32()); r.align();
			for (auto& n : normals) for (auto& v : n) v = r.readFloat();
			GXSetArray(GX_VA_NRM, normals.data(), 12);
			GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
		} else if (chunk == 0x13) {
			colours.resize(r.read32()); r.align();
			memcpy(colours.data(), r.data.data() + r.pos, colours.size() * 4);
			GXSetArray(GX_VA_CLR0, colours.data(), 4);
			GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
		} else if (chunk >= 0x18 && chunk <= 0x1f) {
			auto& coords = uv[chunk - 0x18];
			coords.resize(r.read32()); r.align();
			for (auto& pair : coords) for (auto& v : pair) v = r.readFloat();
			GXAttr attr = (GXAttr)(GX_VA_TEX0 + chunk - 0x18);
			GXSetArray(attr, coords.data(), 8);
			GXSetVtxAttrFmt(GX_VTXFMT0, attr, GX_TEX_ST, GX_F32, 0);
		} else if (chunk == 0x50) {
			u32 meshes = r.read32(); r.align();
			for (u32 m = 0; m < meshes; ++m) {
				r.read32();
				u32 flags = r.read32(), groups = r.read32();
				GXClearVtxDesc();
				GXSetVtxDesc(GX_VA_PNMTXIDX, flags & 1 ? GX_DIRECT : GX_NONE);
				GXSetVtxDesc(GX_VA_TEX1MTXIDX, flags & 2 ? GX_DIRECT : GX_NONE);
				GXSetVtxDesc(GX_VA_POS, GX_INDEX16);
				GXSetVtxDesc(GX_VA_NRM, GX_INDEX16);
				GXSetVtxDesc(GX_VA_CLR0, flags & 4 ? GX_INDEX16 : GX_NONE);
				for (u32 t = 0; t < 8; ++t) GXSetVtxDesc((GXAttr)(GX_VA_TEX0 + t), flags & (8 << t) ? GX_INDEX16 : GX_NONE);
				for (u32 g = 0; g < groups; ++g) {
					u32 deps = r.read32();
					for (u32 d = 0; d < deps; ++d) r.read16();
					u32 lists = r.read32();
					for (u32 l = 0; l < lists; ++l) {
						r.read32(); r.read32(); u32 size = r.read32(); r.align();
						oldDraws += compareList(r.data.data() + r.pos, size);
						ModelDraw draw = {r.data.data() + r.pos, size, sState, {}};
						draw.arrayBytes[GX_VA_POS] = positions.size() * sizeof(positions[0]);
						draw.arrayBytes[GX_VA_NRM] = normals.size() * sizeof(normals[0]);
						draw.arrayBytes[GX_VA_CLR0] = colours.size() * sizeof(colours[0]);
						for (u32 t = 0; t < 8; ++t) draw.arrayBytes[GX_VA_TEX0 + t] = uv[t].size() * sizeof(uv[t][0]);
						modelDraws.push_back(draw);
						if (!normals.empty()) {
							Mtx reflection = {{0.5f,0,0,0.5f},{0,0.5f,0,0.5f},{0,0,0,1}};
							GXLoadTexMtxImm(reflection, GX_TEXMTX0, GX_MTX2x4);
							GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_NRM, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
							compareList(r.data.data() + r.pos, size);
							GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
						}
						++newDraws; r.pos += size;
					}
				}
			}
		}
		r.pos = next;
	}
	printf("%s: identical triangles, GPU draws %u -> %u\n", path, oldDraws, newDraws);
	benchmarkModels(modelDraws);
	RecordedModel keep;
	keep.data = std::move(r.data); keep.positions = std::move(positions); keep.normals = std::move(normals);
	keep.colours = std::move(colours); keep.draws = std::move(modelDraws);
	for (u32 i = 0; i < 8; ++i) keep.uv[i] = std::move(uv[i]);
	recordedModels.push_back(std::move(keep));
}
} // namespace

extern "C" {
unsigned int sceKernelGetSystemTimeLow(void) { ++meshClockReads; return 0; }
void sceKernelDcacheWritebackRange(const void*, unsigned int) {}
void* sceGuGetMemory(int size) { u32 start = used; used += (size + 15) & ~15U; assert(used <= sizeof(memory)); return memory + start; }
int sceGuCheckList() { return used; }
int sceGuFinish() { return 0; }
int sceGuSync(int, int) { return 0; }
int sceGuStart(int, void*) { used = 0; return 0; }
void sceGuEnable(int state) { guEnabled[state] = true; ++rasterCalls; }
void sceGuDisable(int state) { guEnabled[state] = false; ++rasterCalls; }
void sceGuDepthMask(int mask) { guDepthMask = mask; ++rasterCalls; }
void sceGuDepthFunc(int function) { guDepthFunction = function; ++rasterCalls; }
void sceGuFrontFace(int face) { guFrontFace = face; ++rasterCalls; }
void sceGuBlendFunc(int op, int source, int destination, unsigned int sourceFix, unsigned int destinationFix)
{
	guBlend[0] = op; guBlend[1] = source; guBlend[2] = destination;
	guBlend[3] = sourceFix; guBlend[4] = destinationFix; ++rasterCalls;
}
void sceGuAlphaFunc(int function, int reference, int mask)
{
	guAlpha[0] = function; guAlpha[1] = reference; guAlpha[2] = mask; ++rasterCalls;
}
void sceGuPixelMask(unsigned int mask) { guPixelMask = mask; ++rasterCalls; }
void sceGuScissor(int, int, int, int) {}
void sceGuTexMode(int mode, int, int, int) { movieFormat = mode; }
void sceGuTexImage(int level, int width, int height, int stride, const void* pixels)
{
	assert(level == 0);
	if (movieFormat == GU_PSM_5650) assert(width == 512 && height == 512);
	else assert(movieFormat == GU_PSM_4444 && width <= 512 && height <= 512);
	movieStride = stride; moviePixels = pixels;
}
void sceGuTexFunc(int, int) {}
void sceGuTexFilter(int, int) {}
void sceGuTexWrap(int u, int v) { movieWrapU = u; movieWrapV = v; }
void sceGuTexScale(float, float) {}
void sceGuTexOffset(float, float) {}
void sceGuTexFlush() {}
void OSReport(const char*, ...) {}
void sceGuSetMatrix(int type, const ScePspFMatrix4* matrix) { if (type == GU_MODEL) { modelMatrix = *matrix; ++modelMatrixUploads; } }
void sceGuBoneMatrix(unsigned int index, const ScePspFMatrix4* matrix) { assert(index < 8); bones[index] = *matrix; }
void sceGuDrawArray(int primitive, int format, int count, const void* indexData, const void* vertexData)
{
	++drawCalls;
	if (!recordGeometry) return;
	if (primitive == GU_SPRITES) {
		assert(format == (GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D));
		assert(count == 2 && !indexData);
		const auto* pair = static_cast<const MovieVertex*>(vertexData);
		sprites.insert(sprites.end(), pair, pair + count);
		return;
	}
	std::vector<PspGxVertex> vertices;
	for (int i = 0; i < count; ++i) {
		u32 index = indexData ? static_cast<const u16*>(indexData)[i] : i;
		PspGxVertex v;
		if (format & GU_WEIGHT_BITS) {
			const auto& source = static_cast<const PspGxSkinnedVertex*>(vertexData)[index];
			v = source.vertex; v.x = v.y = v.z = 0;
			for (int b = 0; b < 8; ++b) if (source.weights[b]) {
				PspGxVertex part = source.vertex; transform(part, bones[b]);
				v.x += part.x * source.weights[b]; v.y += part.y * source.weights[b]; v.z += part.z * source.weights[b];
			}
		} else v = static_cast<const PspGxVertex*>(vertexData)[index];
		transform(v, modelMatrix);
		vertices.push_back(v);
	}
	if (primitive == GU_TRIANGLES) triangles.insert(triangles.end(), vertices.begin(), vertices.end());
	else {
		assert(primitive == GU_TRIANGLE_STRIP || primitive == GU_TRIANGLE_FAN);
		for (int i = 2; i < count; ++i) {
			triangles.push_back(vertices[primitive == GU_TRIANGLE_FAN ? 0 : i - 2 + (i & 1)]);
			triangles.push_back(vertices[primitive == GU_TRIANGLE_FAN ? i - 1 : i - 1 - (i & 1)]);
			triangles.push_back(vertices[i]);
		}
	}
}
}

extern "C" void sceGuSendCommandi(int command, int argument)
{
	static u32 base, vertexAddress, indexAddress;
	static int format;
	switch (command) {
	case 0x12: format = argument; break;
	case 0x10: base = (argument & 0xf0000) << 8; break;
	case 0x01: vertexAddress = base | (argument & 0xffffff); break;
	case 0x02: indexAddress = base | (argument & 0xffffff); break;
	case 0x0a: {
		const u32 address = base | (argument & 0xffffff);
		const u32* commands = nullptr;
		u64 latestUse = 0;
		for (auto& set : sMeshCache) for (auto& entry : set) {
			if (!entry.storage || (reinterpret_cast<uintptr_t>(meshCacheCommands(entry)) & 0xfffffff) != address
			    || entry.lastUse <= latestUse) continue;
			assert(entry.gpuEpoch == sMeshCacheEpoch);
			commands = meshCacheCommands(entry); latestUse = entry.lastUse;
		}
		assert(commands && !(reinterpret_cast<uintptr_t>(commands) & 63));
		assert(commands[6] == 0x0b000000); // RET, not END/FINISH: parent must resume.
		const u8 opcodes[] = {0x12,0x10,0x02,0x10,0x01,0x04};
		for (u32 i = 0; i < 6; ++i) {
			assert((commands[i] >> 24) == opcodes[i]);
			sceGuSendCommandi(commands[i] >> 24, commands[i] & 0xffffff);
		}
		break;
	}
	case 0x04: {
		// Host pointers are wider than GE addresses. Resolve through the live
		// cache, verifying that both encoded addresses select the same entry.
		const void* vertices = nullptr;
		const u16* indices = nullptr;
		u64 latestUse = 0;
		for (auto& set : sMeshCache) for (auto& entry : set) {
			if (!entry.storage) continue;
			auto* v = entry.storage + kMeshCacheDataOffset;
			auto* i = reinterpret_cast<u16*>(v + entry.vertexCount * entry.vertexSize);
			if ((reinterpret_cast<uintptr_t>(v) & 0xfffffff) == vertexAddress
			    && (reinterpret_cast<uintptr_t>(i) & 0xfffffff) == indexAddress
			    && entry.lastUse > latestUse) {
				// ASan puts different allocation sizes in separate 64-bit address
				// regions whose low GE address bits can alias. The current lookup
				// has the newest use serial; PSP addresses have no such ambiguity.
				latestUse = entry.lastUse;
				assert(entry.gpuEpoch == sMeshCacheEpoch);
				vertices = v; indices = i;
			}
		}
		assert(vertices && indices);
		++deferredDrawCalls;
		sceGuDrawArray((argument >> 16) & 7, format, argument & 0xffff, indices, vertices);
		break;
	}
	default: assert(false);
	}
}

// Minimal pane/resource scaffolding lets the real P2DPicture code render
// through the GU recorder without loading the rest of the game's UI system.
P2DPane::P2DPane() : mPaneTree(this) { mWorldMtx.makeIdentity(); }
P2DPane::~P2DPane() {}
void P2DPane::drawSelf(int, int, const Matrix4f*) {}
P2DPane* P2DPane::search(u32, bool) { return nullptr; }
void P2DPane::makeMatrix(int, int) {}
void Matrix4f::makeIdentity() {
	memset(mMtx, 0, sizeof(mMtx));
	for (int i = 0; i < 4; ++i) mMtx[i][i] = 1;
}
void Matrix4f::multiplyTo(const Matrix4f& other, Matrix4f& result) const {
	for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) {
		result.mMtx[i][j] = 0;
		for (int k = 0; k < 4; ++k) result.mMtx[i][j] += mMtx[i][k] * other.mMtx[k][j];
	}
}
Texture::Texture() { mTexObj = nullptr; }
void Texture::attach() {}
void Texture::detach() {}
namespace zen { Texture* loadTexExp(const char*, bool, bool) { return nullptr; } }

void titleMenuPictureAlpha()
{
	struct ResidentTexture : Texture {
		void makeResident() override { ++pictureResidentCalls; }
	};
	struct MenuPicture : P2DPicture {
		MenuPicture(Texture* texture) : P2DPicture(texture) {}
		void render() {
			Matrix4f identity; identity.makeIdentity();
			drawTexCoord(0, 0, 640, 480, 0, 0, 1, 0, 0, 1, 1, 1, &identity);
		}
	};
	resetState(); resetDraws();
	ResidentTexture texture;
	texture.mWidth = texture.mHeight = 8;
	u8 pixels[64]; memset(pixels, 255, sizeof(pixels));
	// The private layout contains a native pointer and grows on a 64-bit host.
	// Allocate that layout while exercising the public GX API.
	GXTexObjPriv object = {};
	texture.mTexObj = reinterpret_cast<GXTexObj*>(&object);
	GXInitTexObj(texture.mTexObj, pixels, 8, 8, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
	// GX's image address is 32 bits. Warm conversion with the valid host
	// pointer, then use its encoded address for the actual picture cache hit.
	PspGxTexture* converted = convertTexture(pixels, 8, 8, GX_TF_I8);
	assert(converted);
	converted->source = reinterpret_cast<void*>(uintptr_t(object.image3));
	MenuPicture background(&texture);
	background.setAlpha(0); // Same setup as ogScrTitleMgr's 'back' panes.
	pictureResidentCalls = 0;
	background.render();
	assert(pictureResidentCalls == 0 && drawCalls == 0 && triangles.empty());
	// Exercise the actual P2D TEV configuration and FIFO path at every alpha.
	for (u32 alpha = 1; alpha <= 255; ++alpha) {
		resetDraws(); background.setAlpha(alpha); background.render();
		assert(drawCalls == 1 && triangles.size() == 6);
		assert(guEnabled[GU_BLEND]);
		for (const auto& vertex : triangles) assert(vertex.colour == GU_RGBA(255,255,255,alpha));
		assert(triangles[0].x == 0 && triangles[0].y == 0);
		assert(triangles[2].x == 640 && triangles[2].y == 480);
		assert(triangles[2].u == 1 && triangles[2].v == 1);
	}
	assert(pictureResidentCalls == 255);
	resetDraws(); background.setAlpha(0); background.render();
	assert(pictureResidentCalls == 255 && drawCalls == 0);
	releaseTexture(*converted);
	puts("Title menu picture transparency, all fade levels and opaque restoration checks passed");
}

void particleColours()
{
	resetState();
	resetDraws();
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_TEXA, GX_CA_A0, GX_CA_ZERO);
	GXSetTevColor(GX_TEVREG0, (GXColor){255, 96, 32, 128});
	GXSetTevColor(GX_TEVREG1, (GXColor){0, 0, 0, 0});
	// Billboard, child (with irrelevant raster colour), and crossed quads.
	for (int kind = 0; kind < 3; ++kind) {
		GXBegin(GX_QUADS, GX_VTXFMT0, kind == 2 ? 8 : 4);
		for (int i = 0; i < (kind == 2 ? 8 : 4); ++i) {
			GXPosition3f32(i & 1 ? 25 : -25, i & 2 ? -25 : 25, i >= 4 ? 25 : 0);
			if (kind == 1) GXColor4u8(0, 0, 0, 0);
			GXTexCoord2f32(i & 1, (i >> 1) & 1);
		}
		GXEnd();
	}
	assert(drawCalls == 3 && triangles.size() == 24);
	for (const auto& vertex : triangles) assert(vertex.colour == GU_RGBA(255, 96, 32, 128));
	// Restoring ordinary blending must restore raster colours too.
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	for (int i = 0; i < 3; ++i) {
		GXPosition3f32(i, 0, 0);
		GXColor4u8(10, 20, 30, 255);
	}
	GXEnd();
	assert(triangles.back().colour == GU_RGBA(10, 20, 30, 255));
	GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_RASA, GX_CA_TEXA, GX_CA_ZERO);
	assert(!sState.particleColourEnabled);
}

// Until TEV lighting is implemented end-to-end, preserve the title's
// material tint and FIFO alpha rather than applying partial CPU lighting.
void titleColourCompatibility()
{
	resetState(); resetDraws();
	GXSetChanCtrl(GX_COLOR0, GX_TRUE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_SPOT);
	GXSetChanCtrl(GX_ALPHA0, GX_FALSE, GX_SRC_REG, GX_SRC_REG, GX_LIGHT0, GX_DF_CLAMP, GX_AF_NONE);
	GXSetChanMatColor(GX_COLOR0A0, (GXColor){255,255,229,255});
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	for (int i = 0; i < 3; ++i) GXPosition3f32(i, 0, 0);
	GXEnd();
	for (const auto& vertex : triangles) assert(vertex.colour == GU_RGBA(255,255,229,255));
	// A transparent overlay must retain the submitted vertex alpha.
	resetDraws();
	GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_VTX, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
	GXBegin(GX_QUADS, GX_VTXFMT0, 4);
	for (int i = 0; i < 4; ++i) {
		GXPosition3f32(i & 1, i >> 1, 0);
		GXColor4u8(0,0,0,0);
	}
	GXEnd();
	for (const auto& vertex : triangles) assert(vertex.colour == 0);
}

void reflectionCoordinates()
{
	resetState(); resetDraws();
	// Same scale and half-texture translation used by DGX's hull reflection.
	Mtx reflection = {{0.5f,0,0,0.5f},{0,0.5f,0,0.5f},{0,0,0,1}};
	GXLoadTexMtxImm(reflection, GX_TEXMTX0, GX_MTX2x4);
	GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_NRM, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
	f32 positions[][3] = {{10,20,30},{20,30,40},{30,40,50}};
	f32 normals[][3] = {{-1,0,0},{0,1,0},{1,0,0}};
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxDesc(GX_VA_NRM, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	GXSetArray(GX_VA_NRM, normals, sizeof(normals[0]));
	u8 list[] = {GX_TRIANGLES,0,3, 0,0, 1,1, 2,2};
	compareList(list, sizeof(list));
	assert(triangles[0].u == 0 && triangles[0].v == 0.5f);
	assert(triangles[1].u == 0.5f && triangles[1].v == 1);
	assert(triangles[2].u == 1 && triangles[2].v == 0.5f);
	// The normal is the first vector of an NBT entry; tangent/binormal aren't UVs.
	f32 nbt[][9] = {{-1,0,0, 1,0,0, 0,1,0},{0,1,0, 1,0,0, 0,1,0},{1,0,0, 1,0,0, 0,1,0}};
	GXSetVtxDesc(GX_VA_NRM, GX_NONE); GXSetVtxDesc(GX_VA_NBT, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_NBT, GX_NRM_NBT, GX_F32, 0);
	GXSetArray(GX_VA_NBT, nbt, sizeof(nbt[0]));
	compareList(list, sizeof(list));
	assert(triangles[2].u == 1 && triangles[2].v == 0.5f);
	resetDraws();
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	for (u32 i = 0; i < 3; ++i) {
		GXPosition3f32(positions[i][0], positions[i][1], positions[i][2]);
		GXNormal3f32(normals[i][0], normals[i][1], normals[i][2]);
		GXTexCoord2f32(0,0); // Generated coordinates take precedence over FIFO UVs.
	}
	GXEnd();
	assert(triangles[1].u == 0.5f && triangles[1].v == 1);
	// Returning to ordinary title/UI UVs must stop normal-based generation.
	GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
	resetDraws(); GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	for (u32 i = 0; i < 3; ++i) { GXPosition3f32(i,0,0); GXNormal3f32(1,0,0); GXTexCoord2f32(0.2f,0.3f); }
	GXEnd();
	assert(triangles[0].u == 0.2f && triangles[0].v == 0.3f);
}

void onionMaterialTint()
{
	resetState(); resetDraws();
	GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_C0, GX_CC_RASC, GX_CC_ZERO);
	GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
	// Secondary/specular stages and later register writes must not erase C0.
	GXSetTevColorIn(GX_TEVSTAGE1, GX_CC_ZERO, GX_CC_CPREV, GX_CC_ONE, GX_CC_RASC);
	GXSetChanMatColor(GX_COLOR0A0, (GXColor){255,255,255,255});
	for (GXColorS10 tint : {(GXColorS10){255,40,20,255}, (GXColorS10){255,220,0,255}, (GXColorS10){20,60,255,255}}) {
		GXSetTevColorS10(GX_TEVREG0, tint);
		GXSetTevColorS10(GX_TEVREG1, (GXColorS10){0,0,0,0});
		GXSetTevColorS10(GX_TEVREG2, (GXColorS10){0,0,0,0});
		resetDraws(); GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
		for (int i=0; i<3; ++i) { GXPosition3f32(i,0,0); GXColor4u8(255,255,255,128); }
		GXEnd();
		for (const auto& vertex : triangles) assert(vertex.colour == GU_RGBA(tint.r,tint.g,tint.b,128));
	}
	// Indexed geometry must apply the tint once, even with reused triangle indices.
	f32 positions[][3] = {{0,0,0},{1,0,0},{1,1,0},{0,1,0}};
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	u8 list[] = {GX_QUADS,0,4, 0,1,2,3};
	GXSetTevColorS10(GX_TEVREG0, (GXColorS10){128,64,32,255});
	compareList(list,sizeof(list));
	for (const auto& vertex : triangles) assert(vertex.colour == GU_RGBA(128,64,32,255));
	GXSetTevColorS10(GX_TEVREG0, (GXColorS10){-100,512,255,255});
	assert(prepareMaterialTint());
	assert(materialTint(0xffffffff) == GU_RGBA(0,255,255,255));
	// Restore the title's ordinary modulate state with no tint leaking into it.
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	assert(!prepareMaterialTint());
}

// Compare every byte value against the previous arithmetic, including signed
// S10 endpoints, all scales, alpha preservation, and cached-state changes.
void materialTintLookup()
{
	resetState();
	GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_C0, GX_CC_RASC, GX_CC_ZERO);
	for (GXTevScale scale : {GX_CS_SCALE_1, GX_CS_SCALE_2, GX_CS_SCALE_4, GX_CS_DIVIDE_2}) {
		GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, scale, GX_FALSE, GX_TEVPREV);
		for (int tint = -1024; tint <= 1023; ++tint) {
			GXSetTevColorS10(GX_TEVREG0, (GXColorS10){(s16)tint,(s16)(1023-tint-1024),(s16)(tint/2),255});
			assert(prepareMaterialTint());
			assert(prepareMaterialTint()); // Reusing the same table must be identical.
			for (u32 input = 0; input < 256; ++input) {
				u32 colour = GU_RGBA(input, 255-input, input ^ 85, input);
				u32 expected = colour & 0xff000000;
				const s16 components[] = {sState.tevColour0.r,sState.tevColour0.g,sState.tevColour0.b};
				for (u32 c = 0; c < 3; ++c) {
					s32 value = ((colour >> (c*8)) & 255) * (s32)components[c];
					if (scale == GX_CS_DIVIDE_2) value /= 2;
					else value *= 1 << (u32)scale;
					value /= 255;
					expected |= (u32)MAX(0, MIN(255, value)) << (c*8);
				}
				assert(materialTint(colour) == expected);
			}
		}
	}
	GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
	assert(!prepareMaterialTint());
	GXSetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ADDHALF, GX_CS_SCALE_1, GX_FALSE, GX_TEVPREV);
	assert(!prepareMaterialTint());
}

void shadowCopies()
{
	resetState();
	Mtx identity = {{1,0,0,0},{0,1,0,0},{0,0,1,0}};
	GXLoadTexMtxImm(identity, GX_TEXMTX0, GX_MTX3x4);
	GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2X4, GX_TG_POS, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
	PspGxVertex vertex = {};
	generateTexCoord(vertex, 0.25f,0.75f,2);
	assert(vertex.u == 0.25f && vertex.v == 0.75f);
	GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, GX_TG_POS, GX_TEXMTX0, GX_FALSE, GX_PTIDENTITY);
	generateTexCoord(vertex, 0.25f,0.75f,2);
	assert(vertex.u == 0.125f && vertex.v == 0.375f);
	// Validate scaled framebuffer reads, GX tile boundaries and big-endian RGB565.
	std::vector<u32> framebuffer(kBufferWidth*kScreenHeight, GU_RGBA(255,255,255,255));
	std::vector<u8> buffer(16*8*2, 0);
	GXSetTexCopySrc(0,0,640,480); GXSetTexCopyDst(16,8,GX_TF_I8,GX_TRUE);
	copyFramebuffer(buffer.data(), framebuffer.data());
	for (u32 y=0; y<8; ++y) for (u32 x=0; x<16; ++x)
		assert(decodeTexturePixel(buffer.data(),16,GX_TF_I8,x,y) == 0xffffffff);
	for (auto& pixel : framebuffer) pixel = GU_RGBA(255,0,0,255);
	GXSetTexCopyDst(16,8,GX_TF_RGB565,GX_TRUE);
	copyFramebuffer(buffer.data(), framebuffer.data());
	assert(buffer[0] == 0xf8 && buffer[1] == 0);
	for (u32 y=0; y<8; ++y) for (u32 x=0; x<16; ++x)
		assert(decodeTexturePixel(buffer.data(),16,GX_TF_RGB565,x,y) == GU_RGBA(255,0,0,255));
}

void matrixReuse()
{
	resetState();
	modelMatrixUploads = 0;
	loadGuPositionMatrix(0);
	for (u32 i = 0; i < 606; ++i) loadGuPositionMatrix(0);
	assert(modelMatrixUploads == 1);
	Mtx matrix;
	memcpy(matrix, sState.positionMatrices[0], sizeof(matrix));
	GXLoadPosMtxImm(matrix, 0);
	GXSetCurrentMtx(0);
	assert(modelMatrixUploads == 1);
	matrix[0][3] += 1;
	GXLoadPosMtxImm(matrix, 0);
	assert(modelMatrixUploads == 1 && !sState.guPositionMatrixValid);
	loadGuPositionMatrix(0);
	assert(modelMatrixUploads == 2);
	loadGuPositionMatrix(1);
	assert(modelMatrixUploads == 3);
	// Mutating the matrix actually loaded in GU must invalidate it even if
	// GX's current matrix selects a different slot.
	GXLoadPosMtxImm(matrix, 3);
	assert(!sState.guPositionMatrixValid);
	loadGuPositionMatrix(1);
	assert(modelMatrixUploads == 4);
	ScePspFMatrix4 expected = positionMatrix(matrix);
	assert(!memcmp(&expected, &modelMatrix, sizeof(expected)));
	GXSetCurrentMtx(0);
	assert(modelMatrixUploads == 4);
	// Immediate GX draws still load the current matrix before consumption.
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	for (int i = 0; i < 3; ++i) GXPosition3f32(i, 0, 0);
	GXEnd();
	assert(modelMatrixUploads == 5);
}

void deferredModelMatrices()
{
	clearMeshCache(); resetState(); resetDraws();
	f32 positions[][3] = {{1,2,3},{4,5,6},{7,8,9}};
	GXSetVtxDesc(GX_VA_PNMTXIDX, GX_DIRECT);
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	u8 rigid[] = {GX_TRIANGLES,0,3, 3,0, 3,1, 3,2};
	u8 skinned[] = {GX_TRIANGLES,0,3, 0,0, 3,1, 0,2};
	assert(drawMeshList(rigid, rigid + sizeof(rigid))); // Warm conversion.
	assert(drawMeshList(skinned, skinned + sizeof(skinned)));
	sState.guPositionMatrixValid = false;
	modelMatrixUploads = 0;
	for (int draw = 0; draw < 33; ++draw) {
		// This slot is edited/selected by GX but the rigid list uses slot one.
		Mtx current;
		memcpy(current, sState.positionMatrices[0], sizeof(current));
		current[0][3] += 1;
		GXSetCurrentMtx(0); GXLoadPosMtxImm(current, 0);
		assert(modelMatrixUploads == u32(draw == 0 ? 0 : 1));
		triangles.clear();
		assert(drawMeshList(rigid, rigid + sizeof(rigid)));
		assert(modelMatrixUploads == 1);
		for (int i = 0; i < 3; ++i) {
			PspGxVertex expected = {}; expected.x = positions[i][0]; expected.y = positions[i][1]; expected.z = positions[i][2];
			transform(expected, positionMatrix(sState.positionMatrices[1]));
			assert(triangles[i].x == expected.x && triangles[i].y == expected.y && triangles[i].z == expected.z);
		}
	}
	// Skinning, a CPU-transformed fallback and a following immediate draw must
	// each consume the correct matrices without relying on eager restoration.
	triangles.clear(); assert(drawMeshList(skinned, skinned + sizeof(skinned)));
	assert(modelMatrixUploads == 2 && !sState.guPositionMatrixValid);
	GXSetCurrentMtx(0);
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	for (int i = 0; i < 3; ++i) GXPosition3f32(i,0,0);
	GXEnd(); assert(modelMatrixUploads == 3);
	ScePspFMatrix4 expectedMatrix = positionMatrix(sState.positionMatrices[0]);
	assert(!memcmp(&modelMatrix, &expectedMatrix, sizeof(expectedMatrix)));
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	sState.verticesPretransformed = true;
	for (int i = 0; i < 3; ++i) GXPosition3f32(i,0,0);
	GXEnd(); assert(modelMatrixUploads == 4 && !sState.guPositionMatrixValid);
	GXBegin(GX_TRIANGLES, GX_VTXFMT0, 3);
	for (int i = 0; i < 3; ++i) GXPosition3f32(i,0,0);
	GXEnd(); assert(modelMatrixUploads == 5);
	assert(!memcmp(&modelMatrix, &expectedMatrix, sizeof(expectedMatrix)));
	clearMeshCache();
	puts("Deferred model matrices: one upload for 33 rigid draws; skinning, fallback and immediate transitions passed");
}

void swizzledTextures()
{
	const u16 dimensions[][2] = {{1,1}, {4,16}, {16,4}, {8,8}, {13,9}, {32,16}, {64,32}, {512,512}};
	for (const auto& size : dimensions) {
		u32 width = size[0], height = size[1];
		std::vector<u8> source(((width + 7) / 8) * ((height + 3) / 4) * 32);
		for (u32 i = 0; i < source.size(); ++i) source[i] = (i * 37 + i / 11) & 255;
		PspGxTexture* texture = convertTexture(source.data(), width, height, GX_TF_I8);
		assert(texture);
		u32 w = texture->storageWidth, h = texture->storageHeight;
		bool swizzled = w >= 8 && h >= 8;
		// Read the actual output in GE block order, independently of the
		// production offset helper. Check every pixel, including padding.
		for (u32 i = 0; i < w * h; ++i) {
			u32 x = i % w, y = i / w;
			if (swizzled) {
				u32 block = i / 64;
				x = (block % (w / 8)) * 8 + i % 8;
				y = (block / (w / 8)) * 8 + (i % 64) / 8;
			}
			u16 expected = 0;
			if (x < width && y < height) {
				u32 c = decodeTexturePixel(source.data(), width, GX_TF_I8, x, y);
				expected = rgbaTo4444(c & 255, (c >> 8) & 255, (c >> 16) & 255, c >> 24);
			}
			assert(texture->pixels[i] == expected);
		}
		releaseTexture(*texture);
	}
}

void rasterStateReuse()
{
	resetState();
	rasterCalls = 0;
	u64 reuseStart = sTimings.rasterReuses;
	auto material = []() {
		GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
		GXSetCullMode(GX_CULL_BACK);
		GXSetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
		GXSetAlphaCompare(GX_GEQUAL, 128, GX_AOP_AND, GX_LEQUAL, 255);
		GXSetColorUpdate(GX_TRUE);
		setGuEnabled(GU_TEXTURE_2D, true);
	};
	material();
	const u32 firstCalls = rasterCalls;
	assert(firstCalls == 11);
	for (u32 i = 1; i < 606; ++i) material();
	assert(rasterCalls == firstCalls);
	assert(sTimings.rasterReuses - reuseStart == 605 * firstCalls);
	assert(guEnabled[GU_DEPTH_TEST] && guEnabled[GU_CULL_FACE] && guEnabled[GU_BLEND]);
	assert(guEnabled[GU_ALPHA_TEST] && guEnabled[GU_TEXTURE_2D]);
	assert(guDepthFunction == guDepthCompare(GX_LEQUAL) && guDepthMask == GU_FALSE);
	assert(guFrontFace == GU_CW && guPixelMask == 0);
	assert(guAlpha[0] == u32(guCompare(GX_GEQUAL)) && guAlpha[1] == 128 && guAlpha[2] == 255);
	assert(guBlend[0] == GU_ADD && guBlend[1] == GU_SRC_ALPHA && guBlend[2] == GU_ONE_MINUS_SRC_ALPHA);
	assert(guBlend[3] == 0 && guBlend[4] == 0);
	// Changing each live parameter must still reach GU, including fixed blend
	// colours and re-enabling tests after disabled material/overlay draws.
	GXSetZMode(GX_FALSE, GX_GREATER, GX_FALSE);
	assert(!guEnabled[GU_DEPTH_TEST] && guDepthMask == GU_TRUE && guDepthFunction == guDepthCompare(GX_GREATER));
	GXSetCullMode(GX_CULL_FRONT); assert(guFrontFace == GU_CCW);
	GXSetCullMode(GX_CULL_NONE); assert(!guEnabled[GU_CULL_FACE]);
	GXSetBlendMode(GX_BM_SUBTRACT, GX_BL_ONE, GX_BL_ONE, GX_LO_COPY);
	assert(guBlend[0] == GU_REVERSE_SUBTRACT && guBlend[3] == 0xffffffff && guBlend[4] == 0xffffffff);
	GXSetBlendMode(GX_BM_NONE, GX_BL_ZERO, GX_BL_ZERO, GX_LO_COPY); assert(!guEnabled[GU_BLEND]);
	GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_OR, GX_ALWAYS, 0); assert(!guEnabled[GU_ALPHA_TEST]);
	GXSetColorUpdate(GX_FALSE); assert(guPixelMask == 0xffffffff);
	material();
	// A movie overlay changes actual raster state. The next material must
	// restore it even when its requested GX settings match an earlier draw.
	std::vector<u16> movie(512 * 512);
	sState.listOpen = true;
	PikminPsp::drawMovieFrame(movie.data(), 360, 270, 512);
	assert(!guEnabled[GU_DEPTH_TEST] && !guEnabled[GU_BLEND] && !guEnabled[GU_CULL_FACE] && !guEnabled[GU_ALPHA_TEST]);
	material();
	assert(guEnabled[GU_DEPTH_TEST] && guEnabled[GU_BLEND] && guEnabled[GU_CULL_FACE] && guEnabled[GU_ALPHA_TEST]);
	assert(guDepthMask == GU_FALSE);
	// Direct-list splits preserve GE state; invalidation after a clear must
	// conservatively re-emit every parameter. External bootstrap edits bypass
	// reuse altogether because GX does not own that renderer's GU context.
	sceGuStart(GU_DIRECT, memory);
	u32 before = rasterCalls; material(); assert(rasterCalls == before);
	sState.raster = {};
	material(); assert(rasterCalls == before + firstCalls);
	sState.guInitialised = false;
	before = rasterCalls; material(); material(); assert(rasterCalls == before + 2 * firstCalls);
	printf("Repeated material state: %u GU updates -> %u across 606 draws\n", firstCalls * 606, firstCalls);
}

void sampledMeshTiming()
{
	clearMeshCache(); resetState(); resetDraws();
	f32 positions[][3] = {{0,0,0},{1,0,0},{0,1,0}};
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	u8 list[] = {GX_TRIANGLES,0,3,0,1,2};
	sMeshTimingSequence = 0;
	u64 samples = sTimings.meshTimingSamples, hits = sTimings.meshCacheHits, builds = sTimings.meshKeyBuilds;
	meshClockReads = 0;
	for (u32 i = 0; i < 33; ++i) GXCallDisplayList(list, sizeof(list));
	assert(sTimings.meshTimingSamples - samples == 3);
	assert(meshClockReads == 12); // Four reads only for calls 0, 16 and 32.
	assert(drawCalls == 33 && triangles.size() == 33 * 3);
	assert(sTimings.meshCacheHits - hits == 32);
	assert(sTimings.meshKeyBuilds - builds == 1);
	// Every draw position is selected exactly once over 16 rotated phases.
	for (u32 phase = 0; phase < 16; ++phase) {
		sMeshTimingSequence = phase;
		meshClockReads = 0;
		for (u32 draw = 0; draw < 16; ++draw) {
			u32 before = meshClockReads;
			GXCallDisplayList(list, sizeof(list));
			assert(meshClockReads - before == ((draw + phase) % 16 == 0 ? 4 : 0));
		}
		assert(meshClockReads == 4);
	}
	assert(sTimings.meshKeyBuilds - builds == 1); // Hits never construct a key.
	clearMeshCache();
	puts("Sampled mesh timing clock count, phase rotation and unchanged submissions checks passed");
}

void compiledMeshModeChanges()
{
	clearMeshCache(); resetState(); resetDraws();
	f32 positions[][3] = {{0,0,0},{1,0,0},{0,1,0}};
	GXSetVtxDesc(GX_VA_POS, GX_INDEX8);
	GXSetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
	GXSetArray(GX_VA_POS, positions, sizeof(positions[0]));
	u8 list[] = {GX_TRIANGLES,0,3,0,1,2};
	compareList(list, sizeof(list));
	// Native positions can be reused in 2D, but the compiled 3D command list
	// must not be called. Exercise the original dynamic-format command path.
	sState.transform3D = false;
	recordGeometry = false; resetDraws();
	u64 compiled = sTimings.compiledMeshCalls, hits = sTimings.meshCacheHits;
	for (u32 draw = 0; draw < 33; ++draw) assert(drawMeshList(list, list + sizeof(list)));
	assert(sTimings.compiledMeshCalls == compiled && sTimings.meshCacheHits - hits == 33);
	assert(drawCalls == 33 && deferredDrawCalls == 30);
	recordGeometry = true;
	sState.transform3D = true;
	compareList(list, sizeof(list)); // Restoring 3D reuses the compiled list.
	clearMeshCache();
	puts("Compiled mesh CALL/RET, 2D mode changes, batch boundaries and transient UV submissions checks passed");
}

void liveMeshStateValidation()
{
	// Compare direct validation to the former canonical-key byte comparison.
	// Change each dependency independently, including data intentionally ignored
	// by ordinary UV/indexed-color meshes. No geometry decoding is involved.
	u32 checks = 0;
	PikminPsp::MeshValidationScope scope;
	for (GXTexGenSrc texSource : {GX_TG_TEX0, GX_TG_POS, GX_TG_NRM})
	for (bool selected : {false, true}) for (bool tinted : {false, true})
	for (bool indexed : {false, true})
	for (u32 selector : {u32(GX_TEXMTX2), u32(GX_IDENTITY), 0U}) {
		resetState();
		PikminPsp::bindMeshRevision(123);
		for (u32 i = 0; i < GX_VA_MAX_ATTR; ++i) {
			GXSetVtxDesc((GXAttr)i, (GXAttrType)(i % 4));
			GXSetVtxAttrFmt(GX_VTXFMT0, (GXAttr)i, GX_POS_XYZ, GX_F32, 3);
			GXSetArray((GXAttr)i, memory + i * 64, 12);
		}
		GXSetVtxDesc(GX_VA_CLR0, indexed ? GX_INDEX8 : GX_NONE);
		GXSetVtxDesc(GX_VA_PNMTXIDX, indexed ? GX_DIRECT : GX_NONE);
		GXSetVtxDesc(GX_VA_TEX0MTXIDX, selected ? GX_DIRECT : GX_NONE);
		GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX3X4, texSource, selector, GX_FALSE, GX_PTIDENTITY);
		sState.multiplyTevColour0 = tinted;
		sState.tevColourOp = GX_TEV_ADD; sState.tevColourBias = GX_TB_ZERO; sState.tevColourScale = GX_CS_SCALE_2;
		sState.tevColour0 = (GXColorS10){-50,100,255,200};
		MeshCacheKey saved; meshCacheKey(saved);
		const PspGxState baseline = sState;
		auto check = [&]() {
			MeshCacheKey current; meshCacheKey(current);
			assert(meshCacheStateMatches(saved, false) == meshBytesEqual(&saved, &current, meshCacheKeyBytes(current)));
			assert(meshCacheStateMatches(saved, true) == meshBytesEqual(&saved, &current, offsetof(MeshCacheKey, textureMatrices)));
			++checks;
			sState = baseline;
		};
		check();
		for (u32 i = 0; i < GX_VA_MAX_ATTR; ++i) {
			GXSetVtxDesc((GXAttr)i, (GXAttrType)((baseline.attributes[i].source + 1) % 4)); check();
			sState.attributes[i].array += 4; check();
			sState.attributes[i].stride += 4; check();
			sState.attributes[i].fraction ^= 1; check();
			sState.attributes[i].count = (GXCompCnt)(baseline.attributes[i].count ^ 1); check();
			sState.attributes[i].componentType = (GXCompType)(baseline.attributes[i].componentType ^ 1); check();
		}
		sState.currentColour ^= 1; check();
		sState.currentPositionMatrix ^= 1; check();
		sState.multiplyTevColour0 = !tinted; check();
		sState.tevColourOp = GX_TEV_SUB; check();
		sState.tevColourBias = GX_TB_ADDHALF; check();
		sState.tevColourScale = GX_CS_SCALE_4; check();
		sState.tevColour0.r += 1; check();
		sState.tevColour0.g += 1; check();
		sState.tevColour0.b += 1; check();
		sState.tevColour0.a += 1; check();
		for (GXTexGenSrc source : {GX_TG_TEX1, GX_TG_POS, GX_TG_NRM}) { sState.texSource = source; check(); }
		sState.texType = GX_TG_MTX2X4; check();
		for (u32 matrix : {u32(GX_TEXMTX0), u32(GX_IDENTITY), 0U}) { sState.texMatrix = matrix; check(); }
		for (u32 i = 0; i < 10; ++i) for (u32 row = 0; row < 3; ++row) for (u32 column = 0; column < 4; ++column) {
			sState.textureMatrices[i][row][column] += 1; check();
		}
		PikminPsp::bindMeshRevision(124); check();
		PikminPsp::bindMeshRevision(0); check();
	}
	// Clearing and list setters update the compact descriptor shadow as well.
	GXClearVtxDesc();
	for (u32 i = 0; i < GX_VA_MAX_ATTR; ++i) assert(sState.attributeSources[i] == GX_NONE && sState.attributes[i].source == GX_NONE);
	GXVtxDescList descriptors[] = {{GX_VA_POS,GX_INDEX16},{GX_VA_TEX0,GX_INDEX8},{GX_VA_NULL,GX_NONE}};
	GXSetVtxDescv(descriptors);
	for (u32 i = 0; i < GX_VA_MAX_ATTR; ++i) assert(sState.attributeSources[i] == sState.attributes[i].source);
	printf("Live mesh validation: %u state mutations match canonical key comparisons; cached draws build no keys\n", checks);
}

int main(int argc, char** argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	deferredModelMatrices();
	sampledMeshTiming();
	compiledMeshModeChanges();
	liveMeshStateValidation();
	titleMenuPictureAlpha();
	rasterStateReuse();
	matrixReuse();
	swizzledTextures();
	resetState();
	sState.texSource = GX_TG_TEX0;
	MeshCacheKey ordinary, changed;
	meshCacheKey(ordinary);
	sState.textureMatrices[0][0][0] += 1;
	meshCacheKey(changed);
	assert(meshBytesEqual(&ordinary, &changed, meshCacheKeyBytes(ordinary)));
	assert(meshCacheKeyBytes(ordinary) < sizeof(ordinary));
	// Unused secondary UV array metadata does not affect decoded vertices,
	// but its source still affects the byte offsets of subsequent attributes.
	sState.attributes[GX_VA_TEX1].stride = 16;
	meshCacheKey(changed);
	assert(meshBytesEqual(&ordinary, &changed, meshCacheKeyBytes(ordinary)));
	GXSetVtxDesc(GX_VA_TEX1, GX_INDEX16);
	meshCacheKey(changed);
	assert(!meshBytesEqual(&ordinary, &changed, meshCacheKeyBytes(ordinary)));
	GXSetVtxDesc(GX_VA_TEX1, GX_NONE);
	sState.texSource = GX_TG_POS;
	sState.texMatrix = GX_TEXMTX0;
	meshCacheKey(ordinary);
	sState.textureMatrices[0][0][0] += 1;
	meshCacheKey(changed);
	assert(!meshBytesEqual(&ordinary, &changed, meshCacheKeyBytes(ordinary)));
	printf("Mesh state key: ordinary %u bytes, single-matrix generated UV %u bytes\n", (u32)offsetof(MeshCacheKey, textureMatrices), meshCacheKeyBytes(changed));
	// All alignment combinations, tails, and mismatch positions. Exact-sized
	// allocations let ASan catch reads past the requested input buffer.
	for (unsigned aOffset = 0; aOffset < 4; ++aOffset)
		for (unsigned bOffset = 0; bOffset < 4; ++bOffset)
			for (unsigned bytes = 0; bytes <= 129; ++bytes) {
				std::vector<u8> a(bytes + aOffset), b(bytes + bOffset);
				for (unsigned i = 0; i < bytes; ++i) a[aOffset + i] = b[bOffset + i] = (u8)(i * 37);
				assert(meshBytesEqual(a.data() + aOffset, b.data() + bOffset, bytes));
				for (unsigned i = 0; i < bytes; ++i) {
					b[bOffset + i] ^= 1;
					assert(!meshBytesEqual(a.data() + aOffset, b.data() + bOffset, bytes));
					b[bOffset + i] ^= 1;
				}
			}
	onionMaterialTint();
	materialTintLookup();
	reflectionCoordinates();
	titleColourCompatibility();
	shadowCopies();
	syntheticLists();
	meshCacheInvalidation();
	sharedMeshArrays();
	revisionedMeshes();
	animatedCachedMeshes();
	meshMaterialVariants();
	animatedTexCoordMeshes();
	movieFrame();
	particleColours();
	for (int i = 1; i < argc; ++i) modelTest(argv[i]);
	if (recordedModels.size() >= 2) {
		std::vector<ModelDraw> combined = recordedModels[0].draws;
		combined.insert(combined.end(), recordedModels[1].draws.begin(), recordedModels[1].draws.end());
		puts("Combined first two models (shared cache working set):");
		benchmarkModels(combined);
	}
	puts("PSP Onion tint, reflection coordinates, title colour, shadow copy, GPU mesh, particle colour, and movie frame checks passed");
}
