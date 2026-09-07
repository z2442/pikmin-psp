// Native GU command recorder: verify the backend against the old per-vertex
// transform, including the retail title lists when extracted data is present.
#include "../src/port/psp/PspGxBackend.cpp"

#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {
alignas(16) u8 memory[8 * 1024 * 1024];
u32 used;
u32 drawCalls;
ScePspFMatrix4 modelMatrix;
ScePspFMatrix4 bones[8];
std::vector<PspGxVertex> triangles;
struct MovieVertex { u16 u, v; s16 x, y, z; };
std::vector<MovieVertex> sprites;
const void* moviePixels;
int movieFormat, movieStride, movieWrapU, movieWrapV;

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
	used = drawCalls = 0;
	triangles.clear();
	sprites.clear();
	modelMatrix = positionMatrix(sState.positionMatrices[sState.currentPositionMatrix]);
}

void resetState()
{
	memset(&sState, 0, sizeof(sState));
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

void equalVertex(const PspGxVertex& a, const PspGxVertex& b)
{
	assert(a.colour == b.colour);
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
	ScePspFMatrix4 expectedModel = positionMatrix(sState.positionMatrices[sState.currentPositionMatrix]);
	assert(memcmp(&modelMatrix, &expectedModel, sizeof(expectedModel)) == 0);
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
}
} // namespace

extern "C" {
void* sceGuGetMemory(int size) { u32 start = used; used += (size + 15) & ~15U; assert(used <= sizeof(memory)); return memory + start; }
int sceGuCheckList() { return used; }
int sceGuFinish() { return 0; }
int sceGuSync(int, int) { return 0; }
int sceGuStart(int, void*) { used = 0; return 0; }
void sceGuEnable(int) {}
void sceGuDisable(int) {}
void sceGuDepthMask(int) {}
void sceGuPixelMask(unsigned int) {}
void sceGuScissor(int, int, int, int) {}
void sceGuTexMode(int mode, int, int, int) { movieFormat = mode; }
void sceGuTexImage(int level, int width, int height, int stride, const void* pixels)
{
	assert(level == 0 && width == 512 && height == 512);
	movieStride = stride; moviePixels = pixels;
}
void sceGuTexFunc(int, int) {}
void sceGuTexFilter(int, int) {}
void sceGuTexWrap(int u, int v) { movieWrapU = u; movieWrapV = v; }
void sceGuTexScale(float, float) {}
void sceGuTexOffset(float, float) {}
void sceGuTexFlush() {}
void OSReport(const char*, ...) {}
void sceGuSetMatrix(int type, const ScePspFMatrix4* matrix) { if (type == GU_MODEL) modelMatrix = *matrix; }
void sceGuBoneMatrix(unsigned int index, const ScePspFMatrix4* matrix) { assert(index < 8); bones[index] = *matrix; }
void sceGuDrawArray(int primitive, int format, int count, const void* indexData, const void* vertexData)
{
	++drawCalls;
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
	assert(materialTint(0xffffffff) == GU_RGBA(0,255,255,255));
	// Restore the title's ordinary modulate state with no tint leaking into it.
	GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
	assert(materialTint(0xffffffff) == 0xffffffff);
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

int main(int argc, char** argv)
{
	onionMaterialTint();
	reflectionCoordinates();
	titleColourCompatibility();
	shadowCopies();
	syntheticLists();
	movieFrame();
	particleColours();
	for (int i = 1; i < argc; ++i) modelTest(argv[i]);
	puts("PSP Onion tint, reflection coordinates, title colour, shadow copy, GPU mesh, particle colour, and movie frame checks passed");
}
