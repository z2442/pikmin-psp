#include "Dolphin/gx.h"
#include "Dolphin/os.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>

#include <malloc.h>
#include <math.h>
#include <string.h>

namespace {

struct PspGxVertex {
	f32 u;
	f32 v;
	u32 colour;
	f32 x;
	f32 y;
	f32 z;
};

struct PspGxSkinnedVertex {
	f32 weights[8];
	PspGxVertex vertex;
};

struct PspGxAttribute {
	GXAttrType source;
	GXCompCnt count;
	GXCompType componentType;
	u8 fraction;
	u8* array;
	u8 stride;
};

struct PspGxState {
	GXPrimitive primitive;
	PspGxVertex* vertices;
	u32 expectedVertices;
	u32 vertexCount;
	bool inPrimitive;
	bool transform3D;
	bool textureEnabled;
	bool verticesPretransformed;
	u32 currentColour;
	u32 particleColour;
	bool particleColourEnabled;
	u32 clearColour;
	u32 clearDepth;
	bool guInitialised;
	bool listOpen;
	bool displayEnabled;
	u32 currentPositionMatrix;
	PspGxAttribute attributes[GX_VA_MAX_ATTR];
	Mtx positionMatrices[10];
	Mtx textureMatrices[10];
	GXTexGenSrc texSource;
	GXTexGenType texType;
	u32 texMatrix;
	u16 copyLeft, copyTop, copyWidth, copyHeight, copyDestWidth, copyDestHeight;
	GXTexFmt copyFormat;
	u32 drawBufferOffset;
};

struct PspGxTexture {
	const void* source;
	u16 sourceWidth;
	u16 sourceHeight;
	u16 activeWidth;
	u16 activeHeight;
	u16 storageWidth;
	u16 storageHeight;
	GXTexFmt format;
	u16* pixels;
	u32 byteCount;
	u32 lastUse;
};

PspGxState sState;
GXFifoObj sFifo;
// Match the proven OOT PSP backend's 2 MiB direct-list budget.  Transient
// vertices returned by sceGuGetMemory live inside this same buffer.
u32 sGxDisplayList[262144 * 2] __attribute__((aligned(64)));
PspGxTexture sTextureCache[96];
u32 sTextureCacheBytes;
u32 sTextureUseCounter;
u32 sDebugFrameCount;
u32 sDebugDrawCalls;
u32 sDebugVertexCount;
u32 sDebugTexturedDrawCalls;
u32 sDebugTextureLoads;
u32 sDebugTextureFailures;
u32 sDebugListSplits;
u32 sDebugBatchedPrimitives;

const int kBufferWidth  = 512;
const int kScreenWidth  = 480;
const int kScreenHeight = 272;
const int kFrameBytes   = kBufferWidth * kScreenHeight * 4;
const u32 kTextureCacheLimit = 8 * 1024 * 1024;
const u32 kDisplayListCommandReserve = 4096;

u16 rgbaTo4444(u8 red, u8 green, u8 blue, u8 alpha)
{
	return ((u16)(alpha >> 4) << 12) | ((u16)(blue >> 4) << 8) | ((u16)(green >> 4) << 4) | (red >> 4);
}

u8 expand4(u32 value) { return (u8)((value << 4) | value); }
u8 expand5(u32 value) { return (u8)((value << 3) | (value >> 2)); }
u8 expand6(u32 value) { return (u8)((value << 2) | (value >> 4)); }
u8 expand3(u32 value) { return (u8)((value << 5) | (value << 2) | (value >> 1)); }

u32 decodeRgb565(u16 texel)
{
	u8 red = expand5((texel >> 11) & 0x1f);
	u8 green = expand6((texel >> 5) & 0x3f);
	u8 blue = expand5(texel & 0x1f);
	return GU_RGBA(red, green, blue, 255);
}

u32 decodeTexturePixel(const u8* source, u32 width, GXTexFmt format, u32 x, u32 y)
{
	u32 red = 255;
	u32 green = 255;
	u32 blue = 255;
	u32 alpha = 255;
	switch (format) {
	case GX_TF_I4:
	{
		u32 block = ((y / 8) * ((width + 7) / 8) + x / 8) * 32;
		u8 packed = source[block + (y & 7) * 4 + (x & 7) / 2];
		red = green = blue = expand4((x & 1) ? packed & 0xf : packed >> 4);
		break;
	}
	case GX_TF_I8:
	case GX_TF_IA4:
	{
		u32 block = ((y / 4) * ((width + 7) / 8) + x / 8) * 32;
		u8 texel = source[block + (y & 3) * 8 + (x & 7)];
		if (format == GX_TF_I8) {
			red = green = blue = texel;
		} else {
			alpha = expand4(texel >> 4);
			red = green = blue = expand4(texel & 0xf);
		}
		break;
	}
	case GX_TF_IA8:
	case GX_TF_RGB565:
	case GX_TF_RGB5A3:
	{
		u32 block = ((y / 4) * ((width + 3) / 4) + x / 4) * 32;
		const u8* texel = source + block + ((y & 3) * 4 + (x & 3)) * 2;
		u16 packed = ((u16)texel[0] << 8) | texel[1];
		if (format == GX_TF_IA8) {
			alpha = texel[0];
			red = green = blue = texel[1];
		} else if (format == GX_TF_RGB565) {
			return decodeRgb565(packed);
		} else if (packed & 0x8000) {
			red = expand5((packed >> 10) & 0x1f);
			green = expand5((packed >> 5) & 0x1f);
			blue = expand5(packed & 0x1f);
		} else {
			alpha = expand3((packed >> 12) & 7);
			red = expand4((packed >> 8) & 0xf);
			green = expand4((packed >> 4) & 0xf);
			blue = expand4(packed & 0xf);
		}
		break;
	}
	case GX_TF_RGBA8:
	{
		u32 block = ((y / 4) * ((width + 3) / 4) + x / 4) * 64;
		u32 pixel = ((y & 3) * 4 + (x & 3)) * 2;
		alpha = source[block + pixel];
		red = source[block + pixel + 1];
		green = source[block + 32 + pixel];
		blue = source[block + 32 + pixel + 1];
		break;
	}
	case GX_TF_CMPR:
	{
		u32 block = ((y / 8) * ((width + 7) / 8) + x / 8) * 32;
		u32 subBlock = ((y & 7) / 4) * 2 + (x & 7) / 4;
		const u8* compressed = source + block + subBlock * 8;
		u16 colour0 = ((u16)compressed[0] << 8) | compressed[1];
		u16 colour1 = ((u16)compressed[2] << 8) | compressed[3];
		u32 palette[4];
		palette[0] = decodeRgb565(colour0);
		palette[1] = decodeRgb565(colour1);
		u8 r0 = palette[0] & 0xff;
		u8 g0 = (palette[0] >> 8) & 0xff;
		u8 b0 = (palette[0] >> 16) & 0xff;
		u8 r1 = palette[1] & 0xff;
		u8 g1 = (palette[1] >> 8) & 0xff;
		u8 b1 = (palette[1] >> 16) & 0xff;
		if (colour0 > colour1) {
			palette[2] = GU_RGBA((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
			palette[3] = GU_RGBA((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
		} else {
			palette[2] = GU_RGBA((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
			palette[3] = 0;
		}
		u8 selectors = compressed[4 + (y & 3)];
		return palette[(selectors >> (6 - (x & 3) * 2)) & 3];
	}
	default:
		return GU_RGBA(255, 0, 255, 255);
	}
	return GU_RGBA(red, green, blue, alpha);
}

u16 nextPowerOfTwo(u16 value)
{
	u16 result = 1;
	while (result < value && result < 512) result <<= 1;
	return result;
}

void releaseTexture(PspGxTexture& texture)
{
	if (texture.pixels) {
		free(texture.pixels);
		sTextureCacheBytes -= texture.byteCount;
	}
	memset(&texture, 0, sizeof(texture));
}

PspGxTexture* leastRecentlyUsedTexture()
{
	PspGxTexture* result = nullptr;
	for (u32 i = 0; i < ARRAY_SIZE(sTextureCache); ++i) {
		if (!sTextureCache[i].pixels) return &sTextureCache[i];
		if (!result || sTextureCache[i].lastUse < result->lastUse) result = &sTextureCache[i];
	}
	return result;
}

PspGxTexture* oldestResidentTexture()
{
	PspGxTexture* result = nullptr;
	for (u32 i = 0; i < ARRAY_SIZE(sTextureCache); ++i) {
		if (!sTextureCache[i].pixels) continue;
		if (!result || sTextureCache[i].lastUse < result->lastUse) result = &sTextureCache[i];
	}
	return result;
}

PspGxTexture* convertTexture(const void* source, u16 width, u16 height, GXTexFmt format)
{
	if (!source || !width || !height) return nullptr;
	for (u32 i = 0; i < ARRAY_SIZE(sTextureCache); ++i) {
		PspGxTexture& texture = sTextureCache[i];
		if (texture.pixels && texture.source == source && texture.sourceWidth == width && texture.sourceHeight == height
		    && texture.format == format) {
			texture.lastUse = ++sTextureUseCounter;
			return &texture;
		}
	}

	u32 sampleStep = 1;
	while ((width + sampleStep - 1) / sampleStep > 512 || (height + sampleStep - 1) / sampleStep > 512) sampleStep <<= 1;
	u16 activeWidth = (width + sampleStep - 1) / sampleStep;
	u16 activeHeight = (height + sampleStep - 1) / sampleStep;
	u16 storageWidth = nextPowerOfTwo(activeWidth);
	u16 storageHeight = nextPowerOfTwo(activeHeight);
	u32 byteCount = (u32)storageWidth * storageHeight * sizeof(u16);
	if (byteCount > kTextureCacheLimit) return nullptr;
	while (sTextureCacheBytes + byteCount > kTextureCacheLimit) {
		PspGxTexture* victim = oldestResidentTexture();
		if (!victim) break;
		releaseTexture(*victim);
	}
	PspGxTexture* texture = leastRecentlyUsedTexture();
	if (!texture) return nullptr;
	if (texture->pixels) releaseTexture(*texture);

	u16* pixels = static_cast<u16*>(memalign(16, byteCount));
	if (!pixels) return nullptr;
	memset(pixels, 0, byteCount);
	const u8* input = static_cast<const u8*>(source);
	for (u32 y = 0; y < activeHeight; ++y) {
		for (u32 x = 0; x < activeWidth; ++x) {
			u32 colour = decodeTexturePixel(input, width, format, MIN(x * sampleStep, (u32)width - 1),
			                                MIN(y * sampleStep, (u32)height - 1));
			pixels[y * storageWidth + x] = rgbaTo4444(colour & 0xff, (colour >> 8) & 0xff, (colour >> 16) & 0xff,
			                                                 colour >> 24);
		}
	}
	sceKernelDcacheWritebackRange(pixels, byteCount);
	texture->source = source;
	texture->sourceWidth = width;
	texture->sourceHeight = height;
	texture->activeWidth = activeWidth;
	texture->activeHeight = activeHeight;
	texture->storageWidth = storageWidth;
	texture->storageHeight = storageHeight;
	texture->format = format;
	texture->pixels = pixels;
	texture->byteCount = byteCount;
	texture->lastUse = ++sTextureUseCounter;
	sTextureCacheBytes += byteCount;
	return texture;
}

PspGxVertex* currentVertex()
{
	if (!sState.inPrimitive || !sState.vertices || sState.vertexCount == 0) {
		return nullptr;
	}
	return &sState.vertices[sState.vertexCount - 1];
}

int guPrimitive(GXPrimitive primitive)
{
	switch (primitive) {
	case GX_POINTS:        return GU_POINTS;
	case GX_LINES:         return GU_LINES;
	case GX_LINESTRIP:     return GU_LINE_STRIP;
	case GX_TRIANGLES:     return GU_TRIANGLES;
	case GX_TRIANGLESTRIP: return GU_TRIANGLE_STRIP;
	case GX_TRIANGLEFAN:   return GU_TRIANGLE_FAN;
	default:               return GU_TRIANGLES;
	}
}

int guCompare(GXCompare compare)
{
	static const int functions[] = { GU_NEVER, GU_LESS, GU_EQUAL, GU_LEQUAL, GU_GREATER, GU_NOTEQUAL, GU_GEQUAL, GU_ALWAYS };
	return functions[(u32)compare < ARRAY_SIZE(functions) ? compare : GX_ALWAYS];
}

int guDepthCompare(GXCompare compare)
{
	// The PSP renderer uses the conventional reversed GU depth range
	// (near = 0xffff, far = 0).  GX uses the opposite ordering, so preserve
	// GX's comparison semantics by reversing all ordered depth comparisons.
	static const int functions[] = { GU_NEVER, GU_GREATER, GU_EQUAL, GU_GEQUAL, GU_LESS, GU_NOTEQUAL, GU_LEQUAL, GU_ALWAYS };
	return functions[(u32)compare < ARRAY_SIZE(functions) ? compare : GX_ALWAYS];
}

u16 readBig16(const u8*& input)
{
	u16 value = ((u16)input[0] << 8) | input[1];
	input += 2;
	return value;
}

u32 readBig32(const u8*& input)
{
	u32 value = ((u32)input[0] << 24) | ((u32)input[1] << 16) | ((u32)input[2] << 8) | input[3];
	input += 4;
	return value;
}

void transformPosition(u8 matrixIndex, f32& x, f32& y, f32& z)
{
	const Mtx& matrix = sState.positionMatrices[MIN((u32)(matrixIndex / 3), 9U)];
	f32 outputX = matrix[0][0] * x + matrix[0][1] * y + matrix[0][2] * z + matrix[0][3];
	f32 outputY = matrix[1][0] * x + matrix[1][1] * y + matrix[1][2] * z + matrix[1][3];
	f32 outputZ = matrix[2][0] * x + matrix[2][1] * y + matrix[2][2] * z + matrix[2][3];
	x = outputX;
	y = outputY;
	z = outputZ;
}

void generateTexCoord(PspGxVertex& vertex, f32 x, f32 y, f32 z, GXTexGenSrc source = GX_TG_POS,
                      u32 matrix = GX_IDENTITY)
{
	if (sState.texSource != source) return;
	if (matrix == GX_IDENTITY) matrix = sState.texMatrix;
	if (matrix < GX_TEXMTX0 || matrix >= GX_IDENTITY) return;
	// DGX builds the reflection matrix from the model/view rotation and a
	// half-texture offset. Normals here are native-endian model-space values;
	// applying the position matrix as well would transform them twice.
	const Mtx& m = sState.textureMatrices[(matrix - GX_TEXMTX0) / 3];
	vertex.u = m[0][0]*x + m[0][1]*y + m[0][2]*z + m[0][3];
	vertex.v = m[1][0]*x + m[1][1]*y + m[1][2]*z + m[1][3];
	if (sState.texType == GX_TG_MTX3X4) {
		f32 q = m[2][0]*x + m[2][1]*y + m[2][2]*z + m[2][3];
		if (fabsf(q) > 1e-8f) { vertex.u /= q; vertex.v /= q; }
	}
}

void consumeIndex(const u8*& input, GXAttrType source, u32& index)
{
	if (source == GX_INDEX8) {
		index = *input++;
	} else if (source == GX_INDEX16) {
		index = readBig16(input);
	}
}

void decodeMeshVertex(const u8*& input)
{
	u8 matrixIndex = sState.currentPositionMatrix * 3;
	u32 textureMatrix = sState.texMatrix;
	for (u32 attribute = GX_VA_PNMTXIDX; attribute <= GX_VA_TEX7MTXIDX; ++attribute) {
		PspGxAttribute& state = sState.attributes[attribute];
		if (state.source == GX_DIRECT) {
			u8 value = *input++;
			if (attribute == GX_VA_PNMTXIDX) matrixIndex = value;
			if (attribute == GX_VA_TEX0MTXIDX) textureMatrix = value;
		} else if (state.source != GX_NONE) {
			u32 ignored = 0;
			consumeIndex(input, state.source, ignored);
		}
	}

	PspGxAttribute& position = sState.attributes[GX_VA_POS];
	u32 positionIndex = 0;
	consumeIndex(input, position.source, positionIndex);
	if (position.source == GX_NONE || !position.array) return;
	const f32* sourcePosition = reinterpret_cast<const f32*>(position.array + positionIndex * position.stride);
	f32 x = sourcePosition[0];
	f32 y = sourcePosition[1];
	f32 z = position.count == GX_POS_XY ? 0.0f : sourcePosition[2];
	f32 modelX = x, modelY = y, modelZ = z;
	transformPosition(matrixIndex, x, y, z);
	PspGXPosition3f32(x, y, z);

	PspGxAttribute& normal = sState.attributes[GX_VA_NRM];
	PspGxAttribute& nbt = sState.attributes[GX_VA_NBT];
	u32 ignored = 0;
	consumeIndex(input, normal.source, ignored);
	consumeIndex(input, nbt.source, ignored);
	const PspGxAttribute& normalData = nbt.source != GX_NONE ? nbt : normal;
	if (normalData.source != GX_NONE && normalData.array && normalData.componentType == GX_F32) {
		const f32* n = reinterpret_cast<const f32*>(normalData.array + ignored * normalData.stride);
		PspGxVertex* vertex = currentVertex();
		if (vertex) generateTexCoord(*vertex, n[0], n[1], n[2], GX_TG_NRM, textureMatrix);
	}

	for (u32 attribute = GX_VA_CLR0; attribute <= GX_VA_CLR1; ++attribute) {
		PspGxAttribute& colour = sState.attributes[attribute];
		if (colour.source == GX_NONE) continue;
		u32 colourIndex = 0;
		consumeIndex(input, colour.source, colourIndex);
		if (attribute == GX_VA_CLR0 && colour.array) {
			const u8* rgba = colour.array + colourIndex * colour.stride;
			PspGXColor4u8(rgba[0], rgba[1], rgba[2], rgba[3]);
		}
	}

	for (u32 attribute = GX_VA_TEX0; attribute <= GX_VA_TEX7; ++attribute) {
		PspGxAttribute& texture = sState.attributes[attribute];
		if (texture.source == GX_NONE) continue;
		u32 textureIndex = 0;
		consumeIndex(input, texture.source, textureIndex);
		if (attribute == GX_VA_TEX0 && texture.array) {
			const f32* uv = reinterpret_cast<const f32*>(texture.array + textureIndex * texture.stride);
			PspGXTexCoord2f32(uv[0], texture.count == GX_TEX_S ? 0.0f : uv[1]);
		}
	}
	PspGxVertex* out = currentVertex();
	if (out) generateTexCoord(*out, modelX, modelY, modelZ, GX_TG_POS, textureMatrix);
}

int guBlendFactor(GXBlendFactor factor)
{
	switch (factor) {
	case GX_BL_ZERO:        return GU_FIX;
	case GX_BL_ONE:         return GU_FIX;
	case GX_BL_SRCCOL:      return GU_SRC_COLOR;
	case GX_BL_INVSRCCOL:   return GU_ONE_MINUS_SRC_COLOR;
	case GX_BL_SRCALPHA:    return GU_SRC_ALPHA;
	case GX_BL_INVSRCALPHA: return GU_ONE_MINUS_SRC_ALPHA;
	case GX_BL_DSTALPHA:    return GU_DST_ALPHA;
	case GX_BL_INVDSTALPHA: return GU_ONE_MINUS_DST_ALPHA;
	default:                return GU_SRC_ALPHA;
	}
}

ScePspFMatrix4 projectionMatrix(const Mtx44 source)
{
	ScePspFMatrix4 result;
	f32* output = reinterpret_cast<f32*>(&result);
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 4; row++) {
			output[column * 4 + row] = source[row][column];
		}
	}
	return result;
}

ScePspFMatrix4 positionMatrix(const Mtx source)
{
	ScePspFMatrix4 result;
	f32* output = reinterpret_cast<f32*>(&result);
	memset(output, 0, sizeof(result));
	for (int column = 0; column < 4; column++) {
		for (int row = 0; row < 3; row++) {
			output[column * 4 + row] = source[row][column];
		}
	}
	output[15] = 1.0f;
	return result;
}

ScePspFMatrix4 identityMatrix()
{
	ScePspFMatrix4 result;
	f32* output = reinterpret_cast<f32*>(&result);
	memset(output, 0, sizeof(result));
	output[0] = output[5] = output[10] = output[15] = 1.0f;
	return result;
}

void submitVertices(const void* vertices, u32 count, int primitive, const u16* indices = nullptr, bool skinned = false)
{
	if (!vertices || count == 0) {
		return;
	}
	++sDebugDrawCalls;
	sDebugVertexCount += count;
	if (sState.textureEnabled) ++sDebugTexturedDrawCalls;
	if (sState.textureEnabled) sceGuEnable(GU_TEXTURE_2D); else sceGuDisable(GU_TEXTURE_2D);
	const int format = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
	                 | (indices ? GU_INDEX_16BIT : 0)
	                 | (skinned ? GU_WEIGHT_32BITF | GU_WEIGHTS(8) : 0)
	                 | (sState.transform3D ? GU_TRANSFORM_3D : GU_TRANSFORM_2D);
	sceGuDrawArray(primitive, format, count, indices, vertices);
}

void reserveDisplayListMemory(u32 byteCount)
{
	if (!sState.listOpen) return;
	u32 allocationSize = (byteCount + 3) & ~3U;
	u32 requiredSize = allocationSize + 8 + kDisplayListCommandReserve;
	if ((u32)sceGuCheckList() + requiredSize <= sizeof(sGxDisplayList)) return;

	// GE state and frame/depth buffers persist across direct lists.  Waiting
	// here also makes it safe for later transient allocations to reuse the
	// beginning of the list without clobbering vertices still in flight.
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	sceGuStart(GU_DIRECT, sGxDisplayList);
	if (sDebugListSplits < 12) {
		OSReport("Pikmin PSP GX: continued frame in a fresh GU list (%u-byte vertex request)\n", byteCount);
	}
	++sDebugListSplits;
}

void loadGuPositionMatrix(u32 index)
{
	ScePspFMatrix4 converted = positionMatrix(sState.positionMatrices[MIN(index, 9U)]);
	// Draws are submitted with sceGuDrawArray, not sceGumDrawArray.  Send the
	// matrix directly to the GE rather than only updating GUM's software stack.
	sceGuSetMatrix(GU_MODEL, &converted);
}

struct MeshLayout {
	u32 stride;
	s32 offset[GX_VA_MAX_ATTR];
};

bool meshLayout(MeshLayout& layout)
{
	layout.stride = 0;
	for (u32 i = 0; i < GX_VA_MAX_ATTR; ++i) layout.offset[i] = -1;
	for (u32 i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
		// NBT replaces the normal in the FIFO, despite its separate API enum.
		u32 attr = i == GX_VA_NRM && sState.attributes[GX_VA_NBT].source != GX_NONE ? (u32)GX_VA_NBT : i;
		const PspGxAttribute& state = sState.attributes[attr];
		if (state.source == GX_NONE) continue;
		layout.offset[attr] = layout.stride;
		if (i <= GX_VA_TEX7MTXIDX && state.source == GX_DIRECT) ++layout.stride;
		else if (state.source == GX_INDEX8) ++layout.stride;
		else if (state.source == GX_INDEX16) layout.stride += 2;
		else return false;
	}
	const PspGxAttribute& pos = sState.attributes[GX_VA_POS];
	const PspGxAttribute& col = sState.attributes[GX_VA_CLR0];
	const PspGxAttribute& uv = sState.attributes[GX_VA_TEX0];
	return layout.offset[GX_VA_POS] >= 0 && pos.array && pos.componentType == GX_F32
	    && (col.source == GX_NONE || (col.array && col.componentType == GX_RGBA8))
	    && (uv.source == GX_NONE || (uv.array && uv.componentType == GX_F32));
}

u32 meshIndex(const u8* vertex, const MeshLayout& layout, GXAttr attr)
{
	const u8* input = vertex + layout.offset[attr];
	return sState.attributes[attr].source == GX_INDEX16 ? ((u32)input[0] << 8) | input[1] : input[0];
}

u32 triangleIndexCount(u8 primitive, u32 vertices)
{
	switch (primitive) {
	case GX_TRIANGLES: return vertices % 3 == 0 ? vertices : 0;
	case GX_QUADS: return vertices % 4 == 0 ? vertices / 4 * 6 : 0;
	case GX_TRIANGLESTRIP:
	case GX_TRIANGLEFAN: return vertices >= 3 ? (vertices - 2) * 3 : 0;
	default: return 0;
	}
}

u32 meshMatrixIndex(const u8* vertex, const MeshLayout& layout)
{
	return layout.offset[GX_VA_PNMTXIDX] < 0 ? sState.currentPositionMatrix
	    : MIN(meshIndex(vertex, layout, GX_VA_PNMTXIDX) / 3, 9U);
}

// Retail model lists contain thousands of short strips. Submit a whole list
// as indexed triangles. Positions stay in model space: the GE transforms and
// clips them. Rigid meshes use GU_MODEL, while mixed matrices use GE bones.
bool drawMeshList(const u8* input, const u8* end)
{
	MeshLayout layout;
	if (!meshLayout(layout)) return false;
	u32 vertexCount = 0, indexCount = 0, primitiveCount = 0, matrixMask = 0;
	for (const u8* scan = input; scan < end;) {
		u8 command = *scan++;
		if (command == GX_CMD_NOP) continue;
		if (end - scan < 2) return false;
		u32 count = readBig16(scan);
		u32 indices = triangleIndexCount(command & GX_OPCODE_MASK, count);
		if (!indices || (u32)(end - scan) / layout.stride < count) return false;
		vertexCount += count;
		indexCount += indices;
		++primitiveCount;
		if (vertexCount > 65535 || indexCount > 65535) return false;
		for (u32 i = 0; i < count; ++i) matrixMask |= 1U << meshMatrixIndex(scan + i * layout.stride, layout);
		scan += count * layout.stride;
	}
	if (!vertexCount) return true;
	u8 boneSlots[10] = {};
	u32 matrixCount = 0, rigidMatrix = 0;
	for (u32 i = 0; i < 10; ++i) if (matrixMask & (1U << i)) {
		boneSlots[i] = matrixCount++;
		rigidMatrix = i;
	}
	if (matrixCount > 8) return false;
	bool skinned = matrixCount > 1;
	u32 vertexSize = skinned ? sizeof(PspGxSkinnedVertex) : sizeof(PspGxVertex);
	u32 byteCount = vertexCount * vertexSize + indexCount * sizeof(u16);
	if (byteCount + 8 + kDisplayListCommandReserve > sizeof(sGxDisplayList)) return false;
	reserveDisplayListMemory(byteCount);
	u8* vertices = static_cast<u8*>(sceGuGetMemory(byteCount));
	u16* indices = reinterpret_cast<u16*>(vertices + vertexCount * vertexSize);
	u32 base = 0, index = 0;
	const PspGxAttribute& pos = sState.attributes[GX_VA_POS];
	const PspGxAttribute& col = sState.attributes[GX_VA_CLR0];
	const PspGxAttribute& uv = sState.attributes[GX_VA_TEX0];
	while (input < end) {
		u8 command = *input++;
		if (command == GX_CMD_NOP) continue;
		u8 primitive = command & GX_OPCODE_MASK;
		u32 count = readBig16(input);
		for (u32 i = 0; i < count; ++i, input += layout.stride) {
			u8* destination = vertices + (base + i) * vertexSize;
			if (skinned) {
				PspGxSkinnedVertex* weighted = reinterpret_cast<PspGxSkinnedVertex*>(destination);
				memset(weighted->weights, 0, sizeof(weighted->weights));
				weighted->weights[boneSlots[meshMatrixIndex(input, layout)]] = 1.0f;
				destination = reinterpret_cast<u8*>(&weighted->vertex);
			}
			PspGxVertex& out = *reinterpret_cast<PspGxVertex*>(destination);
			const f32* xyz = reinterpret_cast<const f32*>(pos.array + meshIndex(input, layout, GX_VA_POS) * pos.stride);
			out.x = xyz[0]; out.y = xyz[1]; out.z = pos.count == GX_POS_XY ? 0.0f : xyz[2];
			out.colour = sState.currentColour;
			if (col.source != GX_NONE) memcpy(&out.colour, col.array + meshIndex(input, layout, GX_VA_CLR0) * col.stride, 4);
			out.u = out.v = 0.0f;
			if (uv.source != GX_NONE) {
				const f32* st = reinterpret_cast<const f32*>(uv.array + meshIndex(input, layout, GX_VA_TEX0) * uv.stride);
				out.u = st[0]; out.v = uv.count == GX_TEX_S ? 0.0f : st[1];
			}
			u32 textureMatrix = layout.offset[GX_VA_TEX0MTXIDX] >= 0
			    ? meshIndex(input, layout, GX_VA_TEX0MTXIDX) : sState.texMatrix;
			generateTexCoord(out, out.x, out.y, out.z, GX_TG_POS, textureMatrix);
			if (sState.texSource == GX_TG_NRM) {
				GXAttr normalAttr = layout.offset[GX_VA_NBT] >= 0 ? GX_VA_NBT : GX_VA_NRM;
				const PspGxAttribute& normal = sState.attributes[normalAttr];
				if (layout.offset[normalAttr] >= 0 && normal.array && normal.componentType == GX_F32) {
					const f32* n = reinterpret_cast<const f32*>(normal.array + meshIndex(input, layout, normalAttr) * normal.stride);
					generateTexCoord(out, n[0], n[1], n[2], GX_TG_NRM, textureMatrix);
				}
			}
		}
		if (primitive == GX_TRIANGLES) {
			for (u32 i = 0; i < count; ++i) indices[index++] = base + i;
		} else if (primitive == GX_QUADS) {
			for (u32 i = 0; i < count; i += 4) {
				indices[index++] = base + i; indices[index++] = base + i + 1; indices[index++] = base + i + 2;
				indices[index++] = base + i; indices[index++] = base + i + 2; indices[index++] = base + i + 3;
			}
		} else {
			for (u32 i = 2; i < count; ++i) {
				indices[index++] = base + (primitive == GX_TRIANGLEFAN ? 0 : i - 2 + (i & 1));
				indices[index++] = base + (primitive == GX_TRIANGLEFAN ? i - 1 : i - 1 - (i & 1));
				indices[index++] = base + i;
			}
		}
		base += count;
	}
	if (skinned) {
		ScePspFMatrix4 identity = identityMatrix();
		sceGuSetMatrix(GU_MODEL, &identity);
		for (u32 i = 0; i < 10; ++i) if (matrixMask & (1U << i)) {
			ScePspFMatrix4 matrix = positionMatrix(sState.positionMatrices[i]);
			sceGuBoneMatrix(boneSlots[i], &matrix);
		}
	} else {
		loadGuPositionMatrix(rigidMatrix);
	}
	submitVertices(vertices, indexCount, GU_TRIANGLES, indices, skinned);
	loadGuPositionMatrix(sState.currentPositionMatrix);
	sDebugBatchedPrimitives += primitiveCount;
	return true;
}

} // namespace

namespace PikminPsp {
void drawMovieFrame(const u16* pixels, u32 width, u32 height, u32 stride)
{
	if (!sState.listOpen || !pixels) return;
	reserveDisplayListMemory(512);
	sceGuDisable(GU_DEPTH_TEST); sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_BLEND); sceGuDisable(GU_ALPHA_TEST); sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_FOG); sceGuDisable(GU_LIGHTING);
	sceGuPixelMask(0); sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_5650, 0, 0, GU_FALSE);
	sceGuTexImage(0, 512, 512, stride, pixels);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR); sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexScale(1.0f, 1.0f); sceGuTexOffset(0.0f, 0.0f); sceGuTexFlush();
	struct Vertex { u16 u, v; s16 x, y, z; };
	int left = (kScreenWidth - width) / 2, top = (kScreenHeight - height) / 2;
	for (u32 x = 0; x < width; x += 64) {
		u32 end = MIN(x + 64, width);
		Vertex* pair = (Vertex*)sceGuGetMemory(sizeof(Vertex) * 2);
		pair[0] = {u16(x), 0, s16(left + x), s16(top), 0};
		pair[1] = {u16(end), u16(height), s16(left + end), s16(top + height), 0};
		sceGuDrawArray(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, nullptr, pair);
	}
}
}

extern "C" {

void GXBegin(GXPrimitive type, GXVtxFmt, u16 numVertices)
{
	sState.primitive = type;
	sState.expectedVertices = numVertices;
	sState.vertexCount = 0;
	sState.inPrimitive = true;
	sState.verticesPretransformed = false;
	// Keep the original vertices followed by the expanded triangle list.  A
	// four-vertex GX quad therefore needs room for 4 + 6 vertices, not 4 + 2.
	u32 allocationCount = type == GX_QUADS ? (u32)numVertices + ((u32)numVertices / 4) * 6 : numVertices;
	reserveDisplayListMemory(sizeof(PspGxVertex) * allocationCount);
	sState.vertices = static_cast<PspGxVertex*>(sceGuGetMemory(sizeof(PspGxVertex) * allocationCount));
}

void PspGXPosition3f32(f32 x, f32 y, f32 z)
{
	if (!sState.inPrimitive || sState.vertexCount >= sState.expectedVertices) return;
	PspGxVertex& vertex = sState.vertices[sState.vertexCount++];
	vertex.u = vertex.v = 0.0f;
	vertex.colour = sState.particleColourEnabled ? sState.particleColour : sState.currentColour;
	vertex.x = x;
	vertex.y = y;
	vertex.z = z;
}

void PspGXPosition2f32(f32 x, f32 y) { PspGXPosition3f32(x, y, 0.0f); }
void PspGXPosition2s16(s16 x, s16 y) { PspGXPosition3f32((f32)x, (f32)y, 0.0f); }
void PspGXPosition2u16(u16 x, u16 y) { PspGXPosition3f32((f32)x, (f32)y, 0.0f); }
void PspGXPosition3s16(s16 x, s16 y, s16 z) { PspGXPosition3f32((f32)x, (f32)y, (f32)z); }
void PspGXPosition3u16(u16 x, u16 y, u16 z) { PspGXPosition3f32((f32)x, (f32)y, (f32)z); }
void PspGXNormal3f32(f32 x, f32 y, f32 z)
{
	PspGxVertex* vertex = currentVertex();
	if (vertex) generateTexCoord(*vertex, x, y, z, GX_TG_NRM);
}

void PspGXColor1u32(u32 colour)
{
	PspGxVertex* vertex = currentVertex();
	// Most game callers pass a u32 view of an in-memory GXColor.  On the
	// little-endian PSP that value is already in the ABGR layout GU expects.
	if (vertex && !sState.particleColourEnabled) vertex->colour = colour;
}

void PspGXColor4u8(u8 r, u8 g, u8 b, u8 a)
{
	PspGxVertex* vertex = currentVertex();
	if (vertex && !sState.particleColourEnabled) vertex->colour = GU_RGBA(r, g, b, a);
}

void PspGXTexCoord2f32(f32 u, f32 v)
{
	PspGxVertex* vertex = currentVertex();
	if (vertex && sState.texSource != GX_TG_NRM) {
		vertex->u = u;
		vertex->v = v;
	}
}

void PspGXTexCoord2s8(s8 u, s8 v)
{
	f32 scale = 1.0f / (1U << sState.attributes[GX_VA_TEX0].fraction);
	PspGXTexCoord2f32((f32)u * scale, (f32)v * scale);
}
void PspGXTexCoord2u8(u8 u, u8 v)
{
	f32 scale = 1.0f / (1U << sState.attributes[GX_VA_TEX0].fraction);
	PspGXTexCoord2f32((f32)u * scale, (f32)v * scale);
}
void PspGXTexCoord2s16(s16 u, s16 v)
{
	f32 scale = 1.0f / (1U << sState.attributes[GX_VA_TEX0].fraction);
	PspGXTexCoord2f32((f32)u * scale, (f32)v * scale);
}
void PspGXTexCoord2u16(u16 u, u16 v)
{
	f32 scale = 1.0f / (1U << sState.attributes[GX_VA_TEX0].fraction);
	PspGXTexCoord2f32((f32)u * scale, (f32)v * scale);
}

// Indexed display-list streams are handled in the next renderer stage.  Keep
// these entry points explicit so accidental raw FIFO use is harmless for now.
void PspGXWriteU8(u8) { }
void PspGXWriteU16(u16) { }
void PspGXWriteS16(s16) { }
void PspGXWriteU32(u32 value) { PspGXColor1u32(value); }
void PspGXWriteF32(f32) { }

void PspGXEnd(void)
{
	if (!sState.inPrimitive) return;
	if (!sState.verticesPretransformed) for (u32 i = 0; i < sState.vertexCount; ++i) {
		PspGxVertex& vertex = sState.vertices[i];
		generateTexCoord(vertex, vertex.x, vertex.y, vertex.z);
	}
	if (sState.verticesPretransformed) {
		ScePspFMatrix4 identity = identityMatrix();
		sceGuSetMatrix(GU_MODEL, &identity);
	}
	if (sState.primitive == GX_QUADS) {
		u32 sourceCount = sState.vertexCount;
		PspGxVertex* source = sState.vertices;
		PspGxVertex* triangles = source + sourceCount;
		u32 triangleCount = 0;
		for (u32 i = 0; i + 3 < sourceCount; i += 4) {
			triangles[triangleCount++] = source[i + 0];
			triangles[triangleCount++] = source[i + 1];
			triangles[triangleCount++] = source[i + 2];
			triangles[triangleCount++] = source[i + 0];
			triangles[triangleCount++] = source[i + 2];
			triangles[triangleCount++] = source[i + 3];
		}
		submitVertices(triangles, triangleCount, GU_TRIANGLES);
	} else {
		submitVertices(sState.vertices, sState.vertexCount, guPrimitive(sState.primitive));
	}
	if (sState.verticesPretransformed) loadGuPositionMatrix(sState.currentPositionMatrix);
	sState.inPrimitive = false;
	sState.verticesPretransformed = false;
}

GXFifoObj* GXInit(void*, u32)
{
	memset(&sState, 0, sizeof(sState));
	sState.currentColour = 0xffffffff;
	sState.clearColour = 0xff000000;
	sState.clearDepth = 0;
	sceGuInit();
	sceDisplaySetMode(0, kScreenWidth, kScreenHeight);
	sceGuStart(GU_DIRECT, sGxDisplayList);
	sceGuDrawBuffer(GU_PSM_8888, (void*)0, kBufferWidth);
	sceGuDispBuffer(kScreenWidth, kScreenHeight, (void*)kFrameBytes, kBufferWidth);
	sceGuDepthBuffer((void*)(kFrameBytes * 2), kBufferWidth);
	sceGuOffset(2048 - kScreenWidth / 2, 2048 - kScreenHeight / 2);
	sceGuViewport(2048, 2048, kScreenWidth, kScreenHeight);
	sceGuDepthRange(65535, 0);
	sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuEnable(GU_CLIP_PLANES);
	sceGuShadeModel(GU_SMOOTH);
	sceGuFrontFace(GU_CW);
	// Match the working OOT PSP renderer: sceGuDrawArray consumes GE matrix
	// state directly, so initialise it with sceGuSetMatrix rather than GUM's
	// deferred software stacks.
	ScePspFMatrix4 identity = identityMatrix();
	sceGuSetMatrix(GU_PROJECTION, &identity);
	sceGuSetMatrix(GU_VIEW, &identity);
	sceGuSetMatrix(GU_MODEL, &identity);
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	void* displayBuffer = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(sceGeEdramGetAddr()) | 0x40000000U) + kFrameBytes);
	sceDisplaySetFrameBuf(displayBuffer, kBufferWidth, GU_PSM_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
	sceGuStart(GU_DIRECT, sGxDisplayList);
	sState.guInitialised = true;
	sState.listOpen = true;
	sState.displayEnabled = true;
	for (u32 i = 0; i < ARRAY_SIZE(sState.positionMatrices); ++i) {
		memset(sState.positionMatrices[i], 0, sizeof(Mtx));
		sState.positionMatrices[i][0][0] = 1.0f;
		sState.positionMatrices[i][1][1] = 1.0f;
		sState.positionMatrices[i][2][2] = 1.0f;
	}
	return &sFifo;
}

void GXSetProjection(const Mtx44 matrix, GXProjectionType)
{
	ScePspFMatrix4 converted = projectionMatrix(matrix);
	sceGuSetMatrix(GU_PROJECTION, &converted);
	sState.transform3D = true;
}

void GXLoadPosMtxImm(const Mtx matrix, u32 id)
{
	u32 index = MIN(id / 3, 9U);
	memcpy(sState.positionMatrices[index], matrix, sizeof(Mtx));
	if (index == sState.currentPositionMatrix) loadGuPositionMatrix(index);
}

void GXLoadNrmMtxImm(const Mtx, u32) { }
void GXLoadTexMtxImm(const Mtx matrix, u32 id, GXTexMtxType type) {
	if (id >= GX_TEXMTX0 && id < GX_IDENTITY) {
	Mtx& out = sState.textureMatrices[(id - GX_TEXMTX0) / 3];
	memcpy(out, matrix, type == GX_MTX2x4 ? sizeof(f32)*8 : sizeof(Mtx));
	if (type == GX_MTX2x4) { out[2][0] = out[2][1] = out[2][2] = 0; out[2][3] = 1; }
	}
}
void GXSetCurrentMtx(u32 id)
{
	sState.currentPositionMatrix = MIN(id / 3, 9U);
	loadGuPositionMatrix(sState.currentPositionMatrix);
}

void GXSetViewport(f32 left, f32 top, f32 width, f32 height, f32, f32)
{
	int screenWidth = (int)(width * 480.0f / 640.0f);
	int screenHeight = (int)(height * 272.0f / 480.0f);
	int centerX = (int)(left * 480.0f / 640.0f) + screenWidth / 2;
	int centerY = (int)(top * 272.0f / 480.0f) + screenHeight / 2;
	sceGuViewport(2048 + centerX - 240, 2048 + centerY - 136, screenWidth, screenHeight);
}

void GXSetScissor(u32 left, u32 top, u32 width, u32 height)
{
	int x0 = (int)(left * 480 / 640);
	int y0 = (int)(top * 272 / 480);
	int x1 = (int)((left + width) * 480 / 640);
	int y1 = (int)((top + height) * 272 / 480);
	sceGuScissor(x0, y0, x1, y1);
}

void GXSetCullMode(GXCullMode mode)
{
	if (mode == GX_CULL_NONE) {
		sceGuDisable(GU_CULL_FACE);
	} else {
		sceGuEnable(GU_CULL_FACE);
		sceGuFrontFace(mode == GX_CULL_FRONT ? GU_CCW : GU_CW);
	}
}

void GXSetZMode(GXBool compareEnable, GXCompare function, GXBool updateEnable)
{
	if (compareEnable) sceGuEnable(GU_DEPTH_TEST); else sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthFunc(guDepthCompare(function));
	sceGuDepthMask(updateEnable ? GU_FALSE : GU_TRUE);
}

void GXSetBlendMode(GXBlendMode mode, GXBlendFactor source, GXBlendFactor destination, GXLogicOp)
{
	if (mode == GX_BM_NONE) {
		sceGuDisable(GU_BLEND);
		return;
	}
	sceGuEnable(GU_BLEND);
	u32 sourceFix = source == GX_BL_ONE ? 0xffffffff : 0;
	u32 destinationFix = destination == GX_BL_ONE ? 0xffffffff : 0;
	sceGuBlendFunc(mode == GX_BM_SUBTRACT ? GU_REVERSE_SUBTRACT : GU_ADD, guBlendFactor(source), guBlendFactor(destination), sourceFix,
	               destinationFix);
}

// The GU has a fixed rasterized line width.  Wider GX lines will be expanded
// to camera-facing quads once the indexed-geometry path is enabled.
void GXSetLineWidth(u8, GXTexOffset) { }
void GXSetPointSize(u8, GXTexOffset) { }
void GXSetNumTexGens(u8 count) { sState.textureEnabled = count != 0; }
void GXClearVtxDesc(void)
{
	for (u32 i = 0; i < GX_VA_MAX_ATTR; ++i) sState.attributes[i].source = GX_NONE;
}
void GXSetVtxDesc(GXAttr attribute, GXAttrType source)
{
	if ((u32)attribute < GX_VA_MAX_ATTR) sState.attributes[attribute].source = source;
}
void GXSetVtxDescv(GXVtxDescList* list)
{
	if (!list) return;
	for (; list->attr != GX_VA_NULL; ++list) GXSetVtxDesc(list->attr, list->type);
}
void GXSetVtxAttrFmt(GXVtxFmt, GXAttr attribute, GXCompCnt count, GXCompType type, u8 fraction)
{
	if ((u32)attribute >= GX_VA_MAX_ATTR) return;
	sState.attributes[attribute].count = count;
	sState.attributes[attribute].componentType = type;
	sState.attributes[attribute].fraction = fraction;
}
void GXSetVtxAttrFmtv(GXVtxFmt format, GXVtxAttrFmtList* list)
{
	if (!list) return;
	for (; list->attr != GX_VA_NULL; ++list) GXSetVtxAttrFmt(format, list->attr, list->count, list->type, list->frac);
}
void GXSetArray(GXAttr attribute, void* array, u8 stride)
{
	if ((u32)attribute >= GX_VA_MAX_ATTR) return;
	sState.attributes[attribute].array = static_cast<u8*>(array);
	sState.attributes[attribute].stride = stride;
}
void GXInvalidateVtxCache(void) { }

void GXFlush(void) { }
void GXDrawDone(void) { }
void GXPixModeSync(void) { }
OSThread* GXSetCurrentGXThread(void) { return OSGetCurrentThread(); }

void GXSetDispCopySrc(u16, u16, u16, u16) { }
void GXSetDispCopyDst(u16, u16) { }
u32 GXSetDispCopyYScale(f32) { return kScreenHeight; }
void GXSetTexCopySrc(u16 left, u16 top, u16 width, u16 height) {
	sState.copyLeft = left; sState.copyTop = top; sState.copyWidth = width; sState.copyHeight = height;
}
void GXSetTexCopyDst(u16 width, u16 height, GXTexFmt format, GXBool) {
	sState.copyDestWidth = width; sState.copyDestHeight = height; sState.copyFormat = format;
}
void GXSetCopyFilter(GXBool, const u8[12][2], GXBool, const u8[7]) { }
void GXSetCopyClear(GXColor colour, u32 depth)
{
	sState.clearColour = GU_RGBA(colour.r, colour.g, colour.b, colour.a);
	u32 gxDepth = depth > 0xffff ? depth >> 8 : depth;
	if (gxDepth > 0xffff) gxDepth = 0xffff;
	// GX clears to its far value (normally 0xffff/0xffffff).  Convert that
	// value to the reversed PSP depth range used above.
	sState.clearDepth = 0xffff - gxDepth;
}
void GXCopyDisp(void*, GXBool clear)
{
	if (!sState.guInitialised || !sState.listOpen) return;
	if (sDebugFrameCount < 12 || sDebugFrameCount == 120 || sDebugFrameCount == 300 || sDebugFrameCount == 600) {
		OSReport("Pikmin PSP GX frame %u: draws=%u vertices=%u textured=%u textures=%u failed=%u batched=%u\n", sDebugFrameCount,
		         sDebugDrawCalls, sDebugVertexCount, sDebugTexturedDrawCalls, sDebugTextureLoads, sDebugTextureFailures,
		         sDebugBatchedPrimitives);
	}
	++sDebugFrameCount;
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	sState.drawBufferOffset = reinterpret_cast<uintptr_t>(sceGuSwapBuffers());
	if (!sState.displayEnabled) {
		sceGuDisplay(GU_TRUE);
		sState.displayEnabled = true;
	}
	sceGuStart(GU_DIRECT, sGxDisplayList);
	sState.listOpen = true;
	if (clear) {
		sceGuClearColor(sState.clearColour);
		sceGuClearDepth((u16)sState.clearDepth);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	}
	sDebugDrawCalls = 0;
	sDebugVertexCount = 0;
	sDebugTexturedDrawCalls = 0;
	sDebugTextureLoads = 0;
	sDebugTextureFailures = 0;
	sDebugBatchedPrimitives = 0;
}
// Shadow maps are GX tiled I8 buffers. Keep the CPU copy in that format so
// eviction and re-upload use the same endian-safe decoder as ordinary assets.
void copyFramebuffer(u8* output, const u32* framebuffer)
{
	const u32 width = sState.copyDestWidth, height = sState.copyDestHeight;
	for (u32 y = 0; y < height; ++y) for (u32 x = 0; x < width; ++x) {
		u32 sx = MIN((sState.copyLeft + (2*x+1)*sState.copyWidth/(2*width))*kScreenWidth/640, kScreenWidth-1U);
		u32 sy = MIN((sState.copyTop + (2*y+1)*sState.copyHeight/(2*height))*kScreenHeight/480, kScreenHeight-1U);
		u32 colour = framebuffer[sy*kBufferWidth + sx];
		u32 r = colour & 255, g = (colour >> 8) & 255, b = (colour >> 16) & 255;
		if (sState.copyFormat == GX_TF_I8) {
			u32 offset = ((y/4)*((width+7)/8)+x/8)*32 + (y%4)*8 + x%8;
			output[offset] = (77*r + 150*g + 29*b) >> 8;
		} else if (sState.copyFormat == GX_TF_RGB565) {
			u32 offset = ((y/4)*((width+3)/4)+x/4)*32 + ((y%4)*4+x%4)*2;
			u16 packed = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
			output[offset] = packed >> 8; output[offset+1] = packed;
		}
	}
}
void GXCopyTex(void* destination, GXBool clear) {
	if (!destination || !sState.listOpen || !sState.copyDestWidth || !sState.copyDestHeight) return;
	if (sState.copyFormat != GX_TF_I8 && sState.copyFormat != GX_TF_RGB565) return;
	sceGuFinish(); sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	const u32* framebuffer = reinterpret_cast<const u32*>((reinterpret_cast<uintptr_t>(sceGeEdramGetAddr()) | 0x40000000U) + sState.drawBufferOffset);
	copyFramebuffer(static_cast<u8*>(destination), framebuffer);
	// Refresh in place: DGX caches the active Texture pointer and may omit
	// GXLoadTexObj when this same render target is sampled again.
	for (u32 i = 0; i < ARRAY_SIZE(sTextureCache); ++i) {
		PspGxTexture& texture = sTextureCache[i];
		if (texture.source != destination || !texture.pixels) continue;
		for (u32 y = 0; y < texture.activeHeight; ++y) {
			for (u32 x = 0; x < texture.activeWidth; ++x) {
				u32 colour = decodeTexturePixel(static_cast<const u8*>(destination), texture.sourceWidth, texture.format,
				                                x * texture.sourceWidth / texture.activeWidth,
				                                y * texture.sourceHeight / texture.activeHeight);
				texture.pixels[y * texture.storageWidth + x] = rgbaTo4444(colour & 255, (colour >> 8) & 255,
				                                                       (colour >> 16) & 255, colour >> 24);
			}
		}
		sceKernelDcacheWritebackRange(texture.pixels, texture.byteCount);
	}
	sceGuStart(GU_DIRECT, sGxDisplayList);
	sceGuTexFlush();
	if (clear) {
		sceGuClearColor(sState.clearColour); sceGuClearDepth(sState.clearDepth);
		sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	}
}

void GXSetColorUpdate(GXBool enabled) { sceGuPixelMask(enabled ? 0 : 0xffffffff); }
void GXSetAlphaUpdate(GXBool) { }
void GXSetZCompLoc(GXBool) { }
void GXSetPixelFmt(GXPixelFmt, GXZFmt16) { }
void GXSetDither(GXBool enabled) { if (enabled) sceGuEnable(GU_DITHER); else sceGuDisable(GU_DITHER); }
void GXSetCoPlanar(GXBool) { }
void GXSetScissorBoxOffset(s32, s32) { }
void GXSetAlphaCompare(GXCompare compare0, u8 reference0, GXAlphaOp, GXCompare, u8)
{
	if (compare0 == GX_ALWAYS) {
		sceGuDisable(GU_ALPHA_TEST);
	} else {
		sceGuEnable(GU_ALPHA_TEST);
		sceGuAlphaFunc(guCompare(compare0), reference0, 0xff);
	}
}
void GXSetFog(GXFogType type, f32 start, f32 end, f32, f32, GXColor colour)
{
	if (type == GX_FOG_NONE) {
		sceGuDisable(GU_FOG);
	} else {
		sceGuEnable(GU_FOG);
		sceGuFog(start, end, GU_RGBA(colour.r, colour.g, colour.b, colour.a));
	}
}
void GXSetFogRangeAdj(GXBool, u16, GXFogAdjTable*) { }

void GXSetTexCoordGen2(GXTexCoordID id, GXTexGenType type, GXTexGenSrc source, u32 matrix, GXBool, u32) {
	if (id == GX_TEXCOORD0) { sState.texSource = source; sState.texType = type; sState.texMatrix = matrix; }
}
void GXSetNumTevStages(u8) { }
void GXSetTevOp(GXTevStageID stage, GXTevMode)
{
	if (stage == GX_TEVSTAGE0) sState.particleColourEnabled = false;
}
void GXSetTevColorIn(GXTevStageID, GXTevColorArg, GXTevColorArg, GXTevColorArg, GXTevColorArg) { }
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d)
{
	// Particle presets source opacity from REG0, not the raster colour or
	// the environment register written by setPrimEnv immediately afterwards.
	// GU modulates the texture with this primary colour (an approximation of
	// the RGB TEV presets), preserving the particle's animated alpha.
	if (stage == GX_TEVSTAGE0)
		sState.particleColourEnabled = a == GX_CA_ZERO && b == GX_CA_TEXA && c == GX_CA_A0 && d == GX_CA_ZERO;
}
void GXSetTevColorOp(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool, GXTevRegID) { }
void GXSetTevAlphaOp(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool, GXTevRegID) { }
void GXSetTevColor(GXTevRegID reg, GXColor colour)
{
	sState.currentColour = GU_RGBA(colour.r, colour.g, colour.b, colour.a);
	if (reg == GX_TEVREG0) sState.particleColour = sState.currentColour;
}
void GXSetTevColorS10(GXTevRegID, GXColorS10) { }
void GXSetTevKColor(GXTevKColorID, GXColor colour) { sState.currentColour = GU_RGBA(colour.r, colour.g, colour.b, colour.a); }
void GXSetTevKColorSel(GXTevStageID, GXTevKColorSel) { }
void GXSetTevKAlphaSel(GXTevStageID, GXTevKAlphaSel) { }
void GXSetTevSwapMode(GXTevStageID, GXTevSwapSel, GXTevSwapSel) { }
void GXSetTevSwapModeTable(GXTevSwapSel, GXTevColorChan, GXTevColorChan, GXTevColorChan, GXTevColorChan) { }
void GXSetTevOrder(GXTevStageID, GXTexCoordID, GXTexMapID, GXChannelID) { }

void GXInitTexObj(GXTexObj* object, void* image, u16 width, u16 height, GXTexFmt format, GXTexWrapMode sWrap,
	              GXTexWrapMode tWrap, GXBool mipmap)
{
	if (!object) return;
	memset(object, 0, sizeof(*object));
	GXTexObjPriv* texture = reinterpret_cast<GXTexObjPriv*>(object);
	texture->mode0 = ((u32)sWrap << 16) | (u32)tWrap;
	texture->image0 = ((u32)width << 16) | height;
	texture->image3 = reinterpret_cast<u32>(image);
	texture->format = format;
	texture->flags = mipmap ? 1 : 0;
}
void GXInitTexObjLOD(GXTexObj* object, GXTexFilter minFilter, GXTexFilter magFilter, f32, f32, f32, GXBool, GXBool,
	                 GXAnisotropy)
{
	if (!object) return;
	GXTexObjPriv* texture = reinterpret_cast<GXTexObjPriv*>(object);
	texture->mode1 = (minFilter != GX_NEAR || magFilter == GX_LINEAR) ? 1 : 0;
}
void GXLoadTexObj(GXTexObj* object, GXTexMapID id)
{
	// The first TEV texture is the closest useful approximation on the PSP's
	// fixed-function single-texture stage.  Extra TEV maps are ignored for now.
	if (!object || id != GX_TEXMAP0) return;
	GXTexObjPriv* source = reinterpret_cast<GXTexObjPriv*>(object);
	u16 width = source->image0 >> 16;
	u16 height = source->image0 & 0xffff;
	PspGxTexture* texture = convertTexture(reinterpret_cast<void*>(source->image3), width, height, source->format);
	if (!texture) {
		++sDebugTextureFailures;
		sState.textureEnabled = false;
		return;
	}
	++sDebugTextureLoads;
	sceGuTexMode(GU_PSM_4444, 0, 0, GU_FALSE);
	sceGuTexImage(0, texture->storageWidth, texture->storageHeight, texture->storageWidth, texture->pixels);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	int filter = source->mode1 ? GU_LINEAR : GU_NEAREST;
	sceGuTexFilter(filter, filter);
	u32 sWrap = (source->mode0 >> 16) & 0xffff;
	u32 tWrap = source->mode0 & 0xffff;
	sceGuTexWrap(sWrap == GX_CLAMP ? GU_CLAMP : GU_REPEAT, tWrap == GX_CLAMP ? GU_CLAMP : GU_REPEAT);
	sceGuTexScale((f32)texture->activeWidth / texture->storageWidth, (f32)texture->activeHeight / texture->storageHeight);
	sceGuTexOffset(0.0f, 0.0f);
	sState.textureEnabled = true;
}
void GXInvalidateTexAll(void) { }

void GXInitLightAttn(GXLightObj* object, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2)
{
	if (!object) return;
	GXLightObjPriv* light = reinterpret_cast<GXLightObjPriv*>(object);
	light->a[0] = a0; light->a[1] = a1; light->a[2] = a2;
	light->k[0] = k0; light->k[1] = k1; light->k[2] = k2;
}
void GXInitLightPos(GXLightObj* object, f32 x, f32 y, f32 z)
{
	if (!object) return;
	GXLightObjPriv* light = reinterpret_cast<GXLightObjPriv*>(object);
	light->lpos[0] = x; light->lpos[1] = y; light->lpos[2] = z;
}
void GXInitLightDir(GXLightObj* object, f32 x, f32 y, f32 z)
{
	if (!object) return;
	GXLightObjPriv* light = reinterpret_cast<GXLightObjPriv*>(object);
	light->ldir[0] = x; light->ldir[1] = y; light->ldir[2] = z;
}
void GXInitSpecularDir(GXLightObj* object, f32 x, f32 y, f32 z) { GXInitLightDir(object, x, y, z); }
void GXInitLightColor(GXLightObj* object, GXColor colour)
{
	if (object) reinterpret_cast<GXLightObjPriv*>(object)->color = colour;
}
void GXLoadLightObjImm(GXLightObj*, GXLightID) { }
void GXSetChanAmbColor(GXChannelID, GXColor colour) { sceGuAmbient(GU_RGBA(colour.r, colour.g, colour.b, colour.a)); }
void GXSetChanMatColor(GXChannelID, GXColor colour) { sState.currentColour = GU_RGBA(colour.r, colour.g, colour.b, colour.a); }
void GXSetNumChans(u8) { }
void GXSetChanCtrl(GXChannelID, GXBool, GXColorSrc, GXColorSrc, u32, GXDiffuseFn, GXAttnFn) { }

void GXBeginDisplayList(void*, u32) { }
u32 GXEndDisplayList(void) { return 0; }
void GXCallDisplayList(void* displayList, u32 byteCount)
{
	if (!displayList || byteCount == 0) return;
	const u8* input = static_cast<const u8*>(displayList);
	const u8* end = input + byteCount;
	if (drawMeshList(input, end)) return;
	while (input < end) {
		u8 command = *input++;
		if (command == GX_CMD_NOP) continue;
		u8 opcode = command & GX_OPCODE_MASK;
		if (opcode >= GX_CMD_DRAW_QUADS && opcode <= GX_CMD_DRAW_POINTS) {
			if (end - input < 2) break;
			u16 vertexCount = readBig16(input);
			GXBegin((GXPrimitive)opcode, (GXVtxFmt)(command & GX_VAT_MASK), vertexCount);
			sState.verticesPretransformed = true;
			for (u32 vertex = 0; vertex < vertexCount && input < end; ++vertex) decodeMeshVertex(input);
			PspGXEnd();
			continue;
		}
		switch (opcode) {
		case GX_CMD_LOAD_INDX_A:
		case GX_CMD_LOAD_INDX_B:
		case GX_CMD_LOAD_INDX_C:
		case GX_CMD_LOAD_INDX_D:
			input += MIN((u32)(end - input), 4U);
			break;
		case GX_CMD_LOAD_BP_REG:
			input += MIN((u32)(end - input), 4U);
			break;
		case GX_CMD_LOAD_CP_REG:
			input += MIN((u32)(end - input), 5U);
			break;
		case GX_CMD_LOAD_XF_REG:
			if (end - input >= 4) {
				u32 header = readBig32(input);
				u32 words = ((header >> 16) & 0xffff) + 1;
				input += MIN((u32)(end - input), words * 4);
			}
			break;
		case GX_CMD_CALL_DL:
			input += MIN((u32)(end - input), 8U);
			break;
		default:
			return;
		}
	}
}

} // extern "C"
