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

int main(int argc, char** argv)
{
	syntheticLists();
	movieFrame();
	for (int i = 1; i < argc; ++i) modelTest(argv[i]);
	puts("PSP GPU mesh and movie frame checks passed");
}
