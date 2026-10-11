#include "Dolphin/gx.h"
#include "Dolphin/os.h"
#include "port/psp/PspPlatform.h"
#include "port/psp/PspPerformance.h"
#include "port/psp/PspGxMeshCache.h"

#include <pspgu.h>
#include <pspgum.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspkernel.h>

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stddef.h>

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

struct GuRasterState {
	u32 knownEnables, enables, knownValues;
	u32 depthMask[1], depthFunction[1], frontFace[1], blend[5], alpha[3], pixelMask[1];
};

struct PspGxState {
	GuRasterState raster;
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
	GXColorS10 tevColour0;
	bool multiplyTevColour0;
	GXTevOp tevColourOp;
	GXTevBias tevColourBias;
	GXTevScale tevColourScale;
	bool particleColourEnabled;
	u32 clearColour;
	u32 clearDepth;
	bool guInitialised;
	bool listOpen;
	bool displayEnabled;
	u32 currentPositionMatrix;
	u32 guPositionMatrix;
	bool guPositionMatrixValid;
	PspGxAttribute attributes[GX_VA_MAX_ATTR];
	// A contiguous descriptor shadow avoids gathering strided attributes on
	// every cached draw. GX descriptor setters keep both representations live.
	u8 attributeSources[GX_VA_MAX_ATTR];
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
u64 sMeshCacheEpoch = 1;
u32 sCachedDrawBatch;
u32 sMeshTimingSequence;
const u32 kMeshTimingInterval = 16;
void meshCacheGpuIdle() { ++sMeshCacheEpoch; sCachedDrawBatch = 0; }

struct FrameTimings {
	u32 start, frames;
	u64 presentWait, listWait, copyTime, draws, vertices, copies, splits;
	u64 cachedDrawKicks, deferredCachedDraws;
	u64 rasterChanges, rasterReuses;
	u64 meshTimingSamples;
	u64 modelMatrixUploads;
	u64 decodedVertices, skinnedVertices, fallbackLists, textureMisses, meshCacheHits, cachedVertices, meshCacheValidation, arrayValidation;
	u64 meshMissAbsent, meshMissState, meshMissSource, meshStoreFailures, meshEvictions;
	u64 texCoordRefreshes, texCoordRefreshVertices;
	u64 meshKeyBuilds;
	u64 compiledMeshDraws;
	u32 pacingBypasses;
	bool wroteHeader;
};
FrameTimings sTimings;

// Track actual GU state, including temporary movie state. GE state survives
// direct-list restarts; clears conservatively invalidate this shadow.
void setGuEnabled(int state, bool enabled)
{
	u32 mask = 1U << state;
	auto& raster = sState.raster;
	// The bootstrap also has an independent renderer that edits GU state.
	if (sState.guInitialised && (raster.knownEnables & mask) && bool(raster.enables & mask) == enabled) {
		++sTimings.rasterReuses;
		return;
	}
	raster.knownEnables |= mask;
	if (enabled) { raster.enables |= mask; sceGuEnable(state); }
	else { raster.enables &= ~mask; sceGuDisable(state); }
	++sTimings.rasterChanges;
}

template <u32 N> bool guValuesChanged(u32 (&saved)[N], const u32 (&values)[N], u32 mask)
{
	bool changed = !sState.guInitialised || !(sState.raster.knownValues & mask);
	for (u32 i = 0; i < N; ++i) if (saved[i] != values[i]) changed = true;
	if (!changed) { ++sTimings.rasterReuses; return false; }
	for (u32 i = 0; i < N; ++i) saved[i] = values[i];
	sState.raster.knownValues |= mask;
	++sTimings.rasterChanges;
	return true;
}

void setGuDepthMask(int value) {
	const u32 values[] = {u32(value)};
	if (guValuesChanged(sState.raster.depthMask, values, 1)) sceGuDepthMask(value);
}
void setGuDepthFunction(int value) {
	const u32 values[] = {u32(value)};
	if (guValuesChanged(sState.raster.depthFunction, values, 2)) sceGuDepthFunc(value);
}
void setGuFrontFace(int value) {
	const u32 values[] = {u32(value)};
	if (guValuesChanged(sState.raster.frontFace, values, 4)) sceGuFrontFace(value);
}
void setGuBlend(int op, int source, int destination, u32 sourceFix, u32 destinationFix) {
	const u32 values[] = {u32(op), u32(source), u32(destination), sourceFix, destinationFix};
	if (guValuesChanged(sState.raster.blend, values, 8)) sceGuBlendFunc(op, source, destination, sourceFix, destinationFix);
}
void setGuAlpha(int function, int reference, int mask) {
	const u32 values[] = {u32(function), u32(reference), u32(mask)};
	if (guValuesChanged(sState.raster.alpha, values, 16)) sceGuAlphaFunc(function, reference, mask);
}
void setGuPixelMask(u32 value) {
	const u32 values[] = {value};
	if (guValuesChanged(sState.raster.pixelMask, values, 32)) sceGuPixelMask(value);
}

u32 sPreviousMetrics[PikminPsp::PerformanceMetricCount];
u32 sPreviousMainCpu, sPreviousAudioCpu;

void snapshotPerformanceCounters()
{
	for (u32 i = 0; i < PikminPsp::PerformanceMetricCount; ++i)
		sPreviousMetrics[i] = PikminPsp::performanceCounter((PikminPsp::PerformanceMetric)i);
	sPreviousMainCpu = PikminPsp::performanceThreadMicroseconds(false);
	sPreviousAudioCpu = PikminPsp::performanceThreadMicroseconds(true);
}

void recordFrameTimings()
{
	sTimings.draws += sDebugDrawCalls;
	sTimings.vertices += sDebugVertexCount;
	if (++sTimings.frames < 120) return;
	u32 now = sceKernelGetSystemTimeLow();
	u32 elapsed = now - sTimings.start;
	u32 metrics[PikminPsp::PerformanceMetricCount];
	for (u32 i = 0; i < PikminPsp::PerformanceMetricCount; ++i)
		metrics[i] = PikminPsp::performanceCounter((PikminPsp::PerformanceMetric)i) - sPreviousMetrics[i];
	u32 mainCpu = PikminPsp::performanceThreadMicroseconds(false) - sPreviousMainCpu;
	u32 audioCpu = PikminPsp::performanceThreadMicroseconds(true) - sPreviousAudioCpu;
	char path[512];
	if (PikminPsp::resolvePath(path, sizeof(path), "performance.log")) {
		FILE* file = fopen(path, sTimings.wroteHeader ? "a" : "w");
		if (file) {
			if (!sTimings.wroteHeader) {
#if defined(PIKMIN_PSP_LTO)
				fputs("Build: main-CPU LTO; ME mixer/stream use ordinary objects.\n", file);
#else
				fputs("Build: standard (no LTO).\n", file);
#endif
				fputs("Mesh cache: live-state validation, compact lookup tags, rigid/skinned buffers and animated UV refresh. GPU: inline compiled mesh words, batched cached-draw kicks, matrix reuse and swizzled textures. Audio: sampled detail timing, active-voice jobs and ME idle backoff.\nAverages over 120 frames. CPU columns are thread runtime; other times are overlapping wall-time scopes, not additive. Render includes mesh and texture work. Audio work includes lock/ME waits, excludes hardware output wait. submitted_indices includes reused vertices.\nmesh_ms and cache_validate_ms are estimates: one in 16 list calls is timed, with duration scaled by 16 and the sampling phase rotated each frame. Frame/render/CPU times and geometry/cache counters remain exact.\n", file);
				fputs("audio_sequence_ms, audio_channels_ms, audio_stream_ms, audio_mix_ms and audio_movie_ms are estimates: one rotating tick per eight is timed. Audio work/lock/ME-wait and thread CPU times remain exact.\n", file);
				fputs("Presentation: triple-buffered 480x272 RGBA8888; GPU completion before pacing, no extra refresh wait for late frames. Animated UV refresh copies geometry and generates coordinates in one pass.\n", file);
			}
			fprintf(file, "frame=%u fps=%.2f frame_ms=%.2f present_wait_ms=%.2f list_wait_ms=%.2f copy_ms=%.2f draws=%.1f submitted_indices=%.1f copies=%.1f splits=%.1f\n",
			        (unsigned)sDebugFrameCount, elapsed ? 120000000.0 / elapsed : 0.0, elapsed / 120000.0,
			        sTimings.presentWait / 120000.0, sTimings.listWait / 120000.0, sTimings.copyTime / 120000.0,
			        sTimings.draws / 120.0, sTimings.vertices / 120.0, sTimings.copies / 120.0, sTimings.splits / 120.0);
			fprintf(file, "  main_cpu_ms=%.2f audio_cpu_ms=%.2f render_ms=%.2f mesh_ms=%.2f texture_ms=%.2f pacing_ms=%.2f audio_work_ms=%.2f audio_lock_ms=%.2f decoded_vertices=%.1f skinned_vertices=%.1f fallback_lists=%.1f texture_misses=%.1f mesh_cache_hits=%.1f cached_vertices=%.1f cache_validate_ms=%.2f array_validate_ms=%.2f\n",
			        mainCpu / 120000.0, audioCpu / 120000.0,
			        metrics[PikminPsp::PerformanceRender] / 120000.0, metrics[PikminPsp::PerformanceMesh] / 120000.0,
			        metrics[PikminPsp::PerformanceTexture] / 120000.0, metrics[PikminPsp::PerformancePacing] / 120000.0,
			        metrics[PikminPsp::PerformanceAudioWork] / 120000.0, metrics[PikminPsp::PerformanceAudioLock] / 120000.0,
			        sTimings.decodedVertices / 120.0, sTimings.skinnedVertices / 120.0,
			        sTimings.fallbackLists / 120.0, sTimings.textureMisses / 120.0,
			        sTimings.meshCacheHits / 120.0, sTimings.cachedVertices / 120.0, sTimings.meshCacheValidation / 120000.0, sTimings.arrayValidation / 120000.0);
			fprintf(file, "  audio_sequence_ms=%.2f audio_channels_ms=%.2f audio_stream_ms=%.2f audio_mix_ms=%.2f audio_me_wait_ms=%.2f audio_movie_ms=%.2f\n",
			        metrics[PikminPsp::PerformanceAudioSequence] / 120000.0,
			        metrics[PikminPsp::PerformanceAudioChannels] / 120000.0,
			        metrics[PikminPsp::PerformanceAudioStream] / 120000.0,
			        metrics[PikminPsp::PerformanceAudioMix] / 120000.0,
			        metrics[PikminPsp::PerformanceAudioMeWait] / 120000.0,
			        metrics[PikminPsp::PerformanceAudioMovie] / 120000.0);
			fprintf(file, "  gpu_cached_kicks=%.1f gpu_deferred_draws=%.1f\n",
			        sTimings.cachedDrawKicks / 120.0, sTimings.deferredCachedDraws / 120.0);
			fprintf(file, "  gpu_state_changes=%.1f gpu_state_reuses=%.1f\n",
			        sTimings.rasterChanges / 120.0, sTimings.rasterReuses / 120.0);
			fprintf(file, "  mesh_timing_samples=%.1f mesh_timing_interval=%u\n",
			        sTimings.meshTimingSamples / 120.0, kMeshTimingInterval);
			fprintf(file, "  gpu_model_uploads=%.1f\n", sTimings.modelMatrixUploads / 120.0);
			fprintf(file, "  mesh_uv_refreshes=%.1f mesh_uv_vertices=%.1f\n",
			        sTimings.texCoordRefreshes / 120.0, sTimings.texCoordRefreshVertices / 120.0);
			fprintf(file, "  mesh_key_builds=%.1f\n", sTimings.meshKeyBuilds / 120.0);
			fprintf(file, "  gpu_compiled_mesh_draws=%.1f\n", sTimings.compiledMeshDraws / 120.0);
			fprintf(file, "  pacing_bypass_frames=%u\n", sTimings.pacingBypasses);
			fprintf(file, "  movie_decode_ms=%.2f movie_convert_ms=%.2f movie_publish_ms=%.2f\n",
			        metrics[PikminPsp::PerformanceMovieDecode] / 120000.0,
			        metrics[PikminPsp::PerformanceMovieConvert] / 120000.0,
			        metrics[PikminPsp::PerformanceMoviePublish] / 120000.0);
			fprintf(file, "  mesh_miss_absent=%.1f mesh_miss_state=%.1f mesh_miss_source=%.1f mesh_store_failures=%.1f mesh_evictions=%.1f\n",
			        sTimings.meshMissAbsent / 120.0, sTimings.meshMissState / 120.0, sTimings.meshMissSource / 120.0,
			        sTimings.meshStoreFailures / 120.0, sTimings.meshEvictions / 120.0);
			fclose(file);
			sTimings.wroteHeader = true;
		}
	}
	bool wroteHeader = sTimings.wroteHeader;
	sTimings = {};
	sTimings.wroteHeader = wroteHeader;
	// Exclude the occasional log write itself from the next timing window.
	snapshotPerformanceCounters();
	sTimings.start = sceKernelGetSystemTimeLow();
}

const int kBufferWidth  = 512;
const int kScreenWidth  = 480;
const int kScreenHeight = 272;
const int kFrameBytes   = kBufferWidth * kScreenHeight * 4;
const int kDepthBytes   = kBufferWidth * kScreenHeight * 2;
static_assert(kFrameBytes * 3 + kDepthBytes <= 2 * 1024 * 1024, "PSP color/depth buffers must fit in 2 MiB VRAM");
void* sSpareFrameBuffer;
u32 sPresentationRetrace;
u32 sPresentationInterval = 1;

void swapTripleBuffers(void** display, void** render)
{
	void* finished = *render;
	*render = sSpareFrameBuffer;
	sSpareFrameBuffer = *display;
	*display = finished;
}
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

// GE swizzling groups 16 bytes by eight rows. Keep tiny images linear so
// texture dimensions, UV scales, and wrap behavior remain unchanged.
bool textureSwizzled(u32 width, u32 height) { return width >= 8 && height >= 8; }

u32 texturePixelOffset(u32 x, u32 y, u32 width, u32 height)
{
	if (!textureSwizzled(width, height)) return y * width + x;
	return (y >> 3) * width * 8 + (x >> 3) * 64 + (y & 7) * 8 + (x & 7);
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

	++sTimings.textureMisses;
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
			pixels[texturePixelOffset(x, y, storageWidth, storageHeight)] = rgbaTo4444(colour & 0xff, (colour >> 8) & 0xff, (colour >> 16) & 0xff,
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

// The Onion's animated body uses C0 * raster colour, followed by an
// additive specular channel. Keep the tint even while specular is approximate.
// Build channel lookup tables only when the tint or scale changes. The same
// material is used by every vertex in a draw, so divisions belong here.
u8 sMaterialTintTable[3][256];
GXColorS10 sCachedMaterialTint;
GXTevScale sCachedMaterialScale;
bool sMaterialTintValid;

bool prepareMaterialTint()
{
	if (!sState.multiplyTevColour0 || sState.tevColourOp != GX_TEV_ADD || sState.tevColourBias != GX_TB_ZERO) return false;
	const GXColorS10& tint = sState.tevColour0;
	if (!sMaterialTintValid || tint.r != sCachedMaterialTint.r || tint.g != sCachedMaterialTint.g
	    || tint.b != sCachedMaterialTint.b || sState.tevColourScale != sCachedMaterialScale) {
		const s16 components[3] = {tint.r, tint.g, tint.b};
		for (u32 component = 0; component < 3; ++component) {
			for (u32 input = 0; input < 256; ++input) {
				s32 value = (s32)input * components[component];
				if (sState.tevColourScale == GX_CS_DIVIDE_2) value /= 2;
				else value *= 1 << (u32)sState.tevColourScale;
				value /= 255;
				sMaterialTintTable[component][input] = MAX(0, MIN(255, value));
			}
		}
		sCachedMaterialTint = tint;
		sCachedMaterialScale = sState.tevColourScale;
		sMaterialTintValid = true;
	}
	return true;
}

u32 materialTint(u32 colour)
{
	return (colour & 0xff000000) | sMaterialTintTable[0][colour & 255]
	    | ((u32)sMaterialTintTable[1][(colour >> 8) & 255] << 8)
	    | ((u32)sMaterialTintTable[2][(colour >> 16) & 255] << 16);
}

void generateTexCoord(PspGxVertex& vertex, f32 x, f32 y, f32 z, GXTexGenSrc source = GX_TG_POS,
                      u32 matrix = ~0U)
{
	if (sState.texSource != source) return;
	if (matrix == ~0U) matrix = sState.texMatrix;
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

// Match PSPSDK sceGuDrawArray's command encoding, but defer its
// sceGeListUpdateStallAddr syscall. Only persistent, GPU-pinned cache buffers
// use this path. Every sixteenth draw uses the SDK to publish the whole batch;
// ordinary draws and sceGuFinish also publish all preceding commands.
void queueCachedDraw(const void* vertices, u32 count, int primitive, const u16* indices, int format)
{
	enum { VertexAddress = 0x01, IndexAddress = 0x02, Primitive = 0x04, BaseAddress = 0x10, VertexType = 0x12 };
	sceGuSendCommandi(VertexType, format);
	uintptr_t address = reinterpret_cast<uintptr_t>(indices);
	sceGuSendCommandi(BaseAddress, (address >> 8) & 0x000f0000);
	sceGuSendCommandi(IndexAddress, address & 0x00ffffff);
	address = reinterpret_cast<uintptr_t>(vertices);
	sceGuSendCommandi(BaseAddress, (address >> 8) & 0x000f0000);
	sceGuSendCommandi(VertexAddress, address & 0x00ffffff);
	sceGuSendCommandi(Primitive, (primitive << 16) | count);
}

void submitVertices(const void* vertices, u32 count, int primitive, const u16* indices = nullptr, bool skinned = false,
                    bool persistent = false, const u32* commands = nullptr)
{
	if (!vertices || count == 0) {
		return;
	}
	++sDebugDrawCalls;
	sDebugVertexCount += count;
	if (sState.textureEnabled) ++sDebugTexturedDrawCalls;
	if (sState.textureEnabled) setGuEnabled(GU_TEXTURE_2D, true); else setGuEnabled(GU_TEXTURE_2D, false);
	const int format = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF
	                 | (indices ? GU_INDEX_16BIT : 0)
	                 | (skinned ? GU_WEIGHT_32BITF | GU_WEIGHTS(8) : 0)
	                 | (sState.transform3D ? GU_TRANSFORM_3D : GU_TRANSFORM_2D);
	if (persistent && (sCachedDrawBatch++ & 15)) {
		++sTimings.deferredCachedDraws;
		if (commands && sState.transform3D) {
			// Keep the precompiled addresses/format, but inline their words.
			// GE CALL/RET saved CPU emissions while adding a GPU control-flow
			// and RAM fetch dependency for every tiny mesh in the title.
			sceGuSendCommandi(0x12, commands[0] & 0x00ffffff);
			sceGuSendCommandi(0x10, commands[1] & 0x00ffffff);
			sceGuSendCommandi(0x02, commands[2] & 0x00ffffff);
			sceGuSendCommandi(0x10, commands[3] & 0x00ffffff);
			sceGuSendCommandi(0x01, commands[4] & 0x00ffffff);
			sceGuSendCommandi(0x04, commands[5] & 0x00ffffff);
			++sTimings.compiledMeshDraws;
		} else queueCachedDraw(vertices, count, primitive, indices, format);
	} else {
		if (!persistent) sCachedDrawBatch = 0;
		else ++sTimings.cachedDrawKicks;
		sceGuDrawArray(primitive, format, count, indices, vertices);
	}
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
	u32 waitStart = sceKernelGetSystemTimeLow();
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	meshCacheGpuIdle();
	sTimings.listWait += (u32)(sceKernelGetSystemTimeLow() - waitStart);
	++sTimings.splits;
	sceGuStart(GU_DIRECT, sGxDisplayList);
	if (sDebugListSplits < 12) {
		OSReport("Pikmin PSP GX: continued frame in a fresh GU list (%u-byte vertex request)\n", byteCount);
	}
	++sDebugListSplits;
}

void loadGuPositionMatrix(u32 index)
{
	index = MIN(index, 9U);
	if (sState.guPositionMatrixValid && sState.guPositionMatrix == index) return;
	ScePspFMatrix4 converted = positionMatrix(sState.positionMatrices[index]);
	// Draws are submitted with sceGuDrawArray, not sceGumDrawArray.  Send the
	// matrix directly to the GE rather than only updating GUM's software stack.
	sceGuSetMatrix(GU_MODEL, &converted);
	++sTimings.modelMatrixUploads;
	sState.guPositionMatrix = index;
	sState.guPositionMatrixValid = true;
}

struct MeshLayout {
	u32 stride;
	s32 offset[GX_VA_MAX_ATTR];
};

// Bone assignments are part of the list, but the matrices animate each draw.
// Reuse the same slot ordering for conversion and cached submissions.
bool loadMeshMatrices(u32 mask, u32 rigidMatrix)
{
	bool skinned = (mask & (mask - 1)) != 0;
	if (skinned) {
		ScePspFMatrix4 identity = identityMatrix();
		sceGuSetMatrix(GU_MODEL, &identity);
		++sTimings.modelMatrixUploads;
		sState.guPositionMatrixValid = false;
		u32 slot = 0;
		for (u32 i = 0; i < 10; ++i) if (mask & (1U << i)) {
			ScePspFMatrix4 matrix = positionMatrix(sState.positionMatrices[i]);
			sceGuBoneMatrix(slot++, &matrix);
		}
	} else loadGuPositionMatrix(rigidMatrix);
	return skinned;
}

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

// Cache native vertices and fixed bone weights, with snapshots of their input
// bytes. Model matrices remain live GE state and are deliberately excluded.
// Native buffers go directly to the GE. Entries stay pinned until a GPU sync
// completes; replacements can never free geometry that is still in flight.
struct MeshAttributeKey {
	const u8* array;
	u8 source, count, componentType, fraction, stride;
};

struct MeshCacheKey {
	u64 modelRevision;
	// Every source affects FIFO offsets; only these five arrays supply output.
	u8 sources[GX_VA_MAX_ATTR];
	MeshAttributeKey attributes[5];
	u32 currentColour, currentMatrix;
	GXColorS10 tint;
	GXTevOp tintOp;
	GXTevBias tintBias;
	GXTevScale tintScale;
	bool tintEnabled;
	GXTexGenSrc texSource;
	GXTexGenType texType;
	u32 texMatrix;
	bool generatedTexCoords;
	Mtx textureMatrices[10];
};

u32 meshCacheKeyBytes(const MeshCacheKey& key)
{
	return offsetof(MeshCacheKey, textureMatrices) + (!key.generatedTexCoords ? 0
	    : key.sources[GX_VA_TEX0MTXIDX] != GX_NONE ? sizeof(key.textureMatrices) : sizeof(Mtx));
}

// Equality only: unlike memcmp, no byte ordering result is needed. PSP libc
// compares an entire buffer bytewise if either input starts unaligned.
struct __attribute__((packed, may_alias)) MeshUnalignedWord { unsigned int value; };
struct __attribute__((may_alias)) MeshAlignedWord { unsigned int value; };

template <typename Word> bool meshBytesEqualWords(const u8* a, const u8* b, u32 bytes)
{
	while (bytes >= 16) {
		unsigned int difference = reinterpret_cast<const Word*>(a)->value ^ reinterpret_cast<const Word*>(b)->value;
		difference |= reinterpret_cast<const Word*>(a + 4)->value ^ reinterpret_cast<const Word*>(b + 4)->value;
		difference |= reinterpret_cast<const Word*>(a + 8)->value ^ reinterpret_cast<const Word*>(b + 8)->value;
		difference |= reinterpret_cast<const Word*>(a + 12)->value ^ reinterpret_cast<const Word*>(b + 12)->value;
		if (difference) return false;
		a += 16; b += 16; bytes -= 16;
	}
	while (bytes >= 4) {
		if (reinterpret_cast<const Word*>(a)->value != reinterpret_cast<const Word*>(b)->value) return false;
		a += 4; b += 4; bytes -= 4;
	}
	while (bytes--) if (*a++ != *b++) return false;
	return true;
}

bool meshBytesEqual(const void* left, const void* right, u32 bytes)
{
	const auto* a = static_cast<const u8*>(left);
	const auto* b = static_cast<const u8*>(right);
	if ((reinterpret_cast<uintptr_t>(a) | reinterpret_cast<uintptr_t>(b)) & 3)
		return meshBytesEqualWords<MeshUnalignedWord>(a, b, bytes);
	return meshBytesEqualWords<MeshAlignedWord>(a, b, bytes);
}

struct MeshArraySnapshot {
	const u8* source;
	u8* snapshot;
	u32 bytes;
	u64 generation, lastUse;
};
MeshArraySnapshot sMeshArrays[64][4];
u32 sMeshArrayBytes;
u64 sMeshArrayGeneration, sMeshArrayUse;
const u32 kMeshArrayLimit = 2 * 1024 * 1024;
PikminPsp::MeshValidationScope* sMeshValidationScope;

void releaseMeshArray(MeshArraySnapshot& array)
{
	if (array.snapshot) { free(array.snapshot); sMeshArrayBytes -= array.bytes; }
	array = {};
}

MeshArraySnapshot* validateMeshArrayStorage(const void* source, u32 bytes)
{
	if (!source || !bytes || bytes > kMeshArrayLimit) return nullptr;
	uintptr_t address = reinterpret_cast<uintptr_t>(source);
	MeshArraySnapshot* set = sMeshArrays[((address >> 5) ^ (address >> 13)) & 63];
	MeshArraySnapshot* slot = nullptr;
	for (u32 i = 0; i < 4; ++i) {
		MeshArraySnapshot& entry = set[i];
		if (entry.snapshot && entry.source == source && entry.bytes == bytes) {
			if (!meshBytesEqual(source, entry.snapshot, bytes)) {
				memcpy(entry.snapshot, source, bytes);
				entry.generation = ++sMeshArrayGeneration;
			}
			entry.lastUse = ++sMeshArrayUse;
			return &entry;
		}
		if (!slot || !entry.snapshot || (slot->snapshot && entry.lastUse < slot->lastUse)) slot = &entry;
	}
	releaseMeshArray(*slot);
	while (sMeshArrayBytes + bytes > kMeshArrayLimit) {
		MeshArraySnapshot* oldest = nullptr;
		for (auto& bucket : sMeshArrays) for (auto& entry : bucket)
			if (entry.snapshot && (!oldest || entry.lastUse < oldest->lastUse)) oldest = &entry;
		if (!oldest) return nullptr;
		releaseMeshArray(*oldest);
	}
	u8* snapshot = static_cast<u8*>(memalign(64, bytes));
	if (!snapshot) return nullptr;
	memcpy(snapshot, source, bytes);
	slot->source = static_cast<const u8*>(source); slot->snapshot = snapshot; slot->bytes = bytes;
	slot->generation = ++sMeshArrayGeneration; slot->lastUse = ++sMeshArrayUse;
	sMeshArrayBytes += bytes;
	return slot;
}

struct MeshInputRange {
	const u8* begin;
	const u8* end;
	u64 generation;
	void include(const void* address, u32 bytes) {
		const u8* p = static_cast<const u8*>(address);
		if (!begin || p < begin) begin = p;
		if (!end || p + bytes > end) end = p + bytes;
	}
};

MeshArraySnapshot* validatedMeshRange(const MeshInputRange& range, u32 index)
{
	if (!sMeshValidationScope || !range.begin) return nullptr;
	GXAttr attr = index == 0 ? GX_VA_POS : index == 1 ? GX_VA_CLR0 : index == 2 ? GX_VA_TEX0
	    : sState.attributes[GX_VA_NBT].source != GX_NONE ? GX_VA_NBT : GX_VA_NRM;
	auto* array = static_cast<MeshArraySnapshot*>(sMeshValidationScope->arrays[attr]);
	if (!array || !array->snapshot || array->source != sState.attributes[attr].array) return nullptr;
	if ((uintptr_t)range.begin < (uintptr_t)array->source
	    || (uintptr_t)range.end > (uintptr_t)array->source + array->bytes) return nullptr;
	return array;
}

struct MeshCacheEntry {
	const u8* list;
	u8* storage;
	u32 listBytes, nativeBytes, allocationBytes, indexCount, vertexCount, primitiveCount, rigidMatrix, matrixMask, vertexSize;
	u64 lastUse, gpuEpoch;
	MeshInputRange inputs[4];
	bool hasTexCoordInputs;
};

// Keep the inputs to generated UVs so animated reflection/projection matrices
// can update UVs without decoding lists, rebuilding indices or replacing buffers.
struct MeshTexCoordInput { f32 x, y, z; u32 matrix; };

struct MeshUvTransform {
	const f32* matrix;
	bool projective;
	void initialise(u32 selector) {
		matrix = selector >= GX_TEXMTX0 && selector < GX_IDENTITY
		    ? &sState.textureMatrices[(selector - GX_TEXMTX0) / 3][0][0] : nullptr;
		projective = matrix && sState.texType == GX_TG_MTX3X4;
		// Reflection matrices commonly have q=1. Preserve the existing
		// division/rounding for all other projective matrices.
		if (projective && matrix[8] == 0 && matrix[9] == 0 && matrix[10] == 0
		    && (matrix[11] == 1 || fabsf(matrix[11]) <= 1e-8f)) projective = false;
	}
	void apply(PspGxVertex& out, const MeshTexCoordInput& in) const {
		const f32* m = matrix;
		out.u = m[0]*in.x + m[1]*in.y + m[2]*in.z + m[3];
		out.v = m[4]*in.x + m[5]*in.y + m[6]*in.z + m[7];
		if (projective) {
			f32 q = m[8]*in.x + m[9]*in.y + m[10]*in.z + m[11];
			if (fabsf(q) > 1e-8f) { out.u /= q; out.v /= q; }
		}
	}
};

template <bool Skinned, bool Selected>
void refreshMeshTexCoords(u8* output, const u8* stored, const MeshTexCoordInput* inputs, u32 count)
{
	MeshUvTransform uniform, transforms[10];
	if (Selected) for (u32 i = 0; i < 10; ++i) transforms[i].initialise(GX_TEXMTX0 + i * 3);
	else uniform.initialise(sState.texMatrix);
	const u32 vertexSize = Skinned ? sizeof(PspGxSkinnedVertex) : sizeof(PspGxVertex);
	for (u32 i = 0; i < count; ++i, output += vertexSize, stored += vertexSize) {
		u32 offset = 0;
		if (Skinned) {
			memcpy(output, stored, sizeof(PspGxSkinnedVertex::weights));
			offset = offsetof(PspGxSkinnedVertex, vertex);
		}
		auto& out = *reinterpret_cast<PspGxVertex*>(output + offset);
		const auto& source = *reinterpret_cast<const PspGxVertex*>(stored + offset);
		// Copy geometry/weights and generate UVs in a single streaming pass.
		out.colour = source.colour;
		out.x = source.x; out.y = source.y; out.z = source.z;
		const MeshUvTransform* transform = nullptr;
		u32 selector = inputs[i].matrix;
		if (Selected) {
			if (selector >= GX_TEXMTX0 && selector < GX_IDENTITY) transform = &transforms[(selector - GX_TEXMTX0) / 3];
		} else if (selector == sState.texMatrix && uniform.matrix) transform = &uniform;
		if (transform) transform->apply(out, inputs[i]);
		else { out.u = source.u; out.v = source.v; }
	}
}

void refreshMeshVertices(u8* output, const u8* stored, const MeshTexCoordInput* inputs, u32 count, bool skinned)
{
	const bool selected = sState.attributes[GX_VA_TEX0MTXIDX].source != GX_NONE;
	if (skinned) {
		if (selected) refreshMeshTexCoords<true, true>(output, stored, inputs, count);
		else refreshMeshTexCoords<true, false>(output, stored, inputs, count);
	} else {
		if (selected) refreshMeshTexCoords<false, true>(output, stored, inputs, count);
		else refreshMeshTexCoords<false, false>(output, stored, inputs, count);
	}
}

const u32 kMeshCacheLimit = 4 * 1024 * 1024;
const u32 kMeshCacheDataOffset = (sizeof(MeshCacheKey) + 63) & ~63U;
const u32 kMeshCacheCommandBytes = 64;
u64 meshCacheAlignedSize(u64 bytes) { return (bytes + 63) & ~63ULL; }
// The title has 606 lists; eight ways avoid repeatedly rebuilding lists that
// collide in the same bucket despite fitting comfortably in the byte budget.
MeshCacheEntry sMeshCache[256][8];
// Eight address/length tags occupy 64 bytes on PSP. A failed probe need not
// fetch a large geometry/lifetime entry just to reject its list.
struct MeshCacheTag { const u8* list; u32 bytes; };
alignas(64) MeshCacheTag sMeshCacheTags[256][8];
u32 sMeshCacheBytes;
u64 sMeshCacheUse;

MeshCacheTag& meshCacheTag(MeshCacheEntry& entry)
{
	// Entries passed here always belong to the fixed cache table.
	uintptr_t index = (reinterpret_cast<uintptr_t>(&entry) - reinterpret_cast<uintptr_t>(sMeshCache)) / sizeof(MeshCacheEntry);
	return sMeshCacheTags[index / 8][index % 8];
}

u32* meshCacheCommands(const MeshCacheEntry& entry)
{
	return reinterpret_cast<u32*>(entry.storage + kMeshCacheDataOffset + meshCacheAlignedSize(entry.nativeBytes));
}

void releaseMeshCacheEntry(MeshCacheEntry& entry)
{
	meshCacheTag(entry) = {};
	if (entry.storage) { free(entry.storage); sMeshCacheBytes -= entry.allocationBytes; }
	entry = {};
}

void clearMeshCache()
{
	bool pinned = false;
	for (auto& set : sMeshCache) for (auto& entry : set)
		if (entry.storage && entry.gpuEpoch == sMeshCacheEpoch) pinned = true;
	if (pinned) { sceGuFinish(); sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE); meshCacheGpuIdle(); }
	for (auto& set : sMeshCache) for (auto& entry : set) releaseMeshCacheEntry(entry);
	sMeshCacheUse = 0;
	for (auto& set : sMeshArrays) for (auto& array : set) releaseMeshArray(array);
}

void meshCacheKey(MeshCacheKey& key)
{
	++sTimings.meshKeyBuilds;
	memset(&key, 0, offsetof(MeshCacheKey, textureMatrices));
	key.modelRevision = sMeshValidationScope ? sMeshValidationScope->modelRevision : 0;
	memcpy(key.sources, sState.attributeSources, sizeof(key.sources));
	const GXAttr consumed[] = { GX_VA_POS, GX_VA_CLR0, GX_VA_TEX0, GX_VA_NRM, GX_VA_NBT };
	for (u32 i = 0; i < ARRAY_SIZE(consumed); ++i) {
		if (i >= 3 && sState.texSource != GX_TG_NRM) continue;
		const auto& source = sState.attributes[consumed[i]];
		auto& target = key.attributes[i];
		target.source = source.source;
		if (source.source == GX_NONE) continue;
		target.array = source.array;
		target.count = source.count;
		target.componentType = source.componentType;
		target.fraction = source.fraction;
		target.stride = source.stride;
	}
	key.currentColour = sState.attributes[GX_VA_CLR0].source == GX_NONE ? sState.currentColour : 0;
	key.currentMatrix = sState.attributes[GX_VA_PNMTXIDX].source == GX_NONE ? sState.currentPositionMatrix : 0;
	key.tintEnabled = sState.multiplyTevColour0 && sState.tevColourOp == GX_TEV_ADD && sState.tevColourBias == GX_TB_ZERO;
	if (key.tintEnabled) {
		key.tint = sState.tevColour0;
		key.tintOp = sState.tevColourOp; key.tintBias = sState.tevColourBias; key.tintScale = sState.tevColourScale;
	}
	// Ordinary UVs do not depend on texture-matrix animation elsewhere.
	if (sState.texSource == GX_TG_POS || sState.texSource == GX_TG_NRM) {
		key.generatedTexCoords = true;
		key.texSource = sState.texSource; key.texType = sState.texType; key.texMatrix = sState.texMatrix;
		if (sState.attributes[GX_VA_TEX0MTXIDX].source != GX_NONE) {
			memcpy(key.textureMatrices, sState.textureMatrices, sizeof(key.textureMatrices));
		} else {
			// No per-vertex matrix selector: compare just the selected matrix,
			// packed into slot zero instead of clearing/copying all ten slots.
			if (key.texMatrix >= GX_TEXMTX0 && key.texMatrix < GX_IDENTITY)
				memcpy(key.textureMatrices[0], sState.textureMatrices[(key.texMatrix - GX_TEXMTX0) / 3], sizeof(Mtx));
			else memset(key.textureMatrices[0], 0, sizeof(Mtx));
		}
	}

}

bool meshTextureMatricesMatch(const MeshCacheKey& saved)
{
	if (!saved.generatedTexCoords) return true;
	if (saved.sources[GX_VA_TEX0MTXIDX] != GX_NONE)
		return meshBytesEqual(saved.textureMatrices, sState.textureMatrices, sizeof(saved.textureMatrices));
	if (saved.texMatrix >= GX_TEXMTX0 && saved.texMatrix < GX_IDENTITY)
		return meshBytesEqual(saved.textureMatrices[0], sState.textureMatrices[(saved.texMatrix - GX_TEXMTX0) / 3], sizeof(Mtx));
	// Key construction stores zero for identity/invalid selectors. Neither
	// consumes a texture matrix, so edits to those matrices cannot affect it.
	return true;
}

// Compare canonical key fields directly against live state. Cache hits need
// no cleared stack key, descriptor gathering, or texture-matrix copies.
bool meshCacheStateMatches(const MeshCacheKey& saved, bool allowTexCoordRefresh)
{
	if (saved.modelRevision != (sMeshValidationScope ? sMeshValidationScope->modelRevision : 0)
	    || !meshBytesEqual(saved.sources, sState.attributeSources, sizeof(saved.sources))) return false;
	const GXAttr consumed[] = { GX_VA_POS, GX_VA_CLR0, GX_VA_TEX0, GX_VA_NRM, GX_VA_NBT };
	for (u32 i = 0; i < ARRAY_SIZE(consumed); ++i) {
		if (i >= 3 && sState.texSource != GX_TG_NRM) continue;
		const auto& source = sState.attributes[consumed[i]];
		if (source.source == GX_NONE) continue;
		const auto& target = saved.attributes[i];
		if (target.array != source.array || target.count != source.count || target.componentType != source.componentType
		    || target.fraction != source.fraction || target.stride != source.stride) return false;
	}
	if (sState.attributes[GX_VA_CLR0].source == GX_NONE && saved.currentColour != sState.currentColour) return false;
	if (sState.attributes[GX_VA_PNMTXIDX].source == GX_NONE && saved.currentMatrix != sState.currentPositionMatrix) return false;
	const bool tinted = sState.multiplyTevColour0 && sState.tevColourOp == GX_TEV_ADD && sState.tevColourBias == GX_TB_ZERO;
	if (saved.tintEnabled != tinted) return false;
	if (tinted && (!meshBytesEqual(&saved.tint, &sState.tevColour0, sizeof(saved.tint))
	    || saved.tintOp != sState.tevColourOp || saved.tintBias != sState.tevColourBias || saved.tintScale != sState.tevColourScale)) return false;
	const bool generated = sState.texSource == GX_TG_POS || sState.texSource == GX_TG_NRM;
	if (saved.generatedTexCoords != generated) return false;
	if (generated && (saved.texSource != sState.texSource || saved.texType != sState.texType || saved.texMatrix != sState.texMatrix)) return false;
	return allowTexCoordRefresh || meshTextureMatricesMatch(saved);
}

u32 meshCacheSet(const u8* list)
{
	uintptr_t address = reinterpret_cast<uintptr_t>(list);
	return ((address >> 5) ^ (address >> 13)) & 255;
}

MeshCacheEntry* findCachedMesh(const u8* list, u32 bytes)
{
	bool foundList = false, foundState = false;
	const u32 set = meshCacheSet(list);
	for (u32 way = 0; way < 8; ++way) {
		const MeshCacheTag& tag = sMeshCacheTags[set][way];
		if (!tag.list || tag.list != list || tag.bytes != bytes) continue;
		MeshCacheEntry& entry = sMeshCache[set][way];
		foundList = true;
		const auto& saved = *reinterpret_cast<const MeshCacheKey*>(entry.storage);
		if (!meshCacheStateMatches(saved, entry.hasTexCoordInputs)) continue;
		foundState = true;
		if (saved.modelRevision) { entry.lastUse = ++sMeshCacheUse; return &entry; }
		const u8* snapshot = reinterpret_cast<const u8*>(meshCacheCommands(entry)) + kMeshCacheCommandBytes;
		if (!meshBytesEqual(snapshot, list, bytes)) continue;
		snapshot += meshCacheAlignedSize(bytes);
		bool equal = true;
		for (u32 i = 0; i < 4; ++i) {
			const auto& range = entry.inputs[i];
			if (!range.begin) continue;
			if (range.generation) {
				MeshArraySnapshot* array = validatedMeshRange(range, i);
				if (!array || array->generation != range.generation) { equal = false; break; }
				continue;
			}
			u32 length = range.end - range.begin;
			if (!meshBytesEqual(snapshot, range.begin, length)) { equal = false; break; }
			snapshot += meshCacheAlignedSize(length);
		}
		if (equal) { entry.lastUse = ++sMeshCacheUse; return &entry; }
	}
	if (foundState) ++sTimings.meshMissSource;
	else if (foundList) ++sTimings.meshMissState;
	else ++sTimings.meshMissAbsent;
	return nullptr;
}

void cacheMesh(const u8* list, u32 listBytes, const MeshCacheKey& key, const MeshInputRange (&inputs)[4],
               const void* vertices, u32 nativeBytes, u32 indexCount, u32 vertexCount, u32 primitives, u32 matrix,
               u32 matrixMask, u32 vertexSize, bool hasTexCoordInputs)
{
	MeshInputRange savedInputs[4];
	memcpy(savedInputs, inputs, sizeof(savedInputs));
	if (key.modelRevision) memset(savedInputs, 0, sizeof(savedInputs));
	const u32 snapshotListBytes = key.modelRevision ? 0 : listBytes;
	u64 total = kMeshCacheDataOffset + meshCacheAlignedSize(snapshotListBytes) + meshCacheAlignedSize(nativeBytes) + kMeshCacheCommandBytes;
	for (u32 i = 0; i < 4; ++i) {
		auto& range = savedInputs[i];
		if (!range.begin) continue;
		if (MeshArraySnapshot* array = validatedMeshRange(range, i)) range.generation = array->generation;
		else total += meshCacheAlignedSize(range.end - range.begin);
	}
	if (total > kMeshCacheLimit) { ++sTimings.meshStoreFailures; return; }
	MeshCacheEntry* slot = nullptr;
	for (auto& entry : sMeshCache[meshCacheSet(list)]) {
		if (!entry.storage) { slot = &entry; break; }
		if (entry.gpuEpoch == sMeshCacheEpoch) continue;
		// A list can have several live material/projection variants. Replacing
		// the first matching address evicts unrelated variants on every pass.
		if (entry.list == list && entry.listBytes == listBytes
		    && meshBytesEqual(entry.storage, &key, meshCacheKeyBytes(key))) { slot = &entry; break; }
		if (!slot || entry.lastUse < slot->lastUse) slot = &entry;
	}
	if (!slot) { ++sTimings.meshStoreFailures; return; } // All ways are GPU-pinned.
	if (slot->storage) ++sTimings.meshEvictions;
	releaseMeshCacheEntry(*slot);
	while (sMeshCacheBytes + total > kMeshCacheLimit) {
		MeshCacheEntry* oldest = nullptr;
		for (auto& set : sMeshCache) for (auto& entry : set)
			if (entry.storage && entry.gpuEpoch != sMeshCacheEpoch && (!oldest || entry.lastUse < oldest->lastUse)) oldest = &entry;
		if (!oldest) { ++sTimings.meshStoreFailures; return; }
		++sTimings.meshEvictions;
		releaseMeshCacheEntry(*oldest);
	}
	u8* storage = static_cast<u8*>(memalign(64, total));
	if (!storage) { ++sTimings.meshStoreFailures; return; }
	memcpy(storage, &key, meshCacheKeyBytes(key));
	memcpy(storage + kMeshCacheDataOffset, vertices, nativeBytes);
	u32* commands = reinterpret_cast<u32*>(storage + kMeshCacheDataOffset + meshCacheAlignedSize(nativeBytes));
	const int format = GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D
	    | (vertexSize == sizeof(PspGxSkinnedVertex) ? GU_WEIGHT_32BITF | GU_WEIGHTS(8) : 0);
	uintptr_t vertexAddress = reinterpret_cast<uintptr_t>(storage + kMeshCacheDataOffset);
	uintptr_t indexAddress = vertexAddress + vertexCount * vertexSize;
	// Same six words as queueCachedDraw, stored once and emitted inline.
	commands[0] = 0x12000000 | format;
	commands[1] = 0x10000000 | ((indexAddress >> 8) & 0x000f0000);
	commands[2] = 0x02000000 | (indexAddress & 0x00ffffff);
	commands[3] = 0x10000000 | ((vertexAddress >> 8) & 0x000f0000);
	commands[4] = 0x01000000 | (vertexAddress & 0x00ffffff);
	commands[5] = 0x04000000 | (GU_TRIANGLES << 16) | indexCount;
	sceKernelDcacheWritebackRange(storage + kMeshCacheDataOffset, meshCacheAlignedSize(nativeBytes) + kMeshCacheCommandBytes);
	u8* snapshot = reinterpret_cast<u8*>(commands) + kMeshCacheCommandBytes;
	if (snapshotListBytes) memcpy(snapshot, list, snapshotListBytes);
	snapshot += meshCacheAlignedSize(snapshotListBytes);
	for (const auto& range : savedInputs) if (range.begin && !range.generation) {
		u32 bytes = range.end - range.begin;
		memcpy(snapshot, range.begin, bytes); snapshot += meshCacheAlignedSize(bytes);
	}
	slot->list = list; slot->storage = storage; slot->listBytes = listBytes; slot->nativeBytes = nativeBytes;
	slot->allocationBytes = total; slot->indexCount = indexCount; slot->vertexCount = vertexCount;
	slot->primitiveCount = primitives; slot->rigidMatrix = matrix; slot->lastUse = ++sMeshCacheUse;
	slot->matrixMask = matrixMask; slot->vertexSize = vertexSize;
	slot->hasTexCoordInputs = hasTexCoordInputs;
	memcpy(slot->inputs, savedInputs, sizeof(slot->inputs));
	sMeshCacheBytes += total;
	meshCacheTag(*slot) = {list, listBytes};
}

// Retail model lists contain thousands of short strips. Submit a whole list
// as indexed triangles. Positions stay in model space: the GE transforms and
// clips them. Rigid meshes use GU_MODEL, while mixed matrices use GE bones.
bool drawMeshList(const u8* input, const u8* end, bool useCache = true, u32 timingWeight = 1)
{
	const u8* listStart = input;
	MeshCacheKey key;
	if (useCache) {
		u32 validationStart = timingWeight ? sceKernelGetSystemTimeLow() : 0;
		MeshCacheEntry* cached = findCachedMesh(input, end - input);
		if (timingWeight) sTimings.meshCacheValidation += (u32)(sceKernelGetSystemTimeLow() - validationStart) * timingWeight;
		if (cached) {
			const auto& saved = *reinterpret_cast<const MeshCacheKey*>(cached->storage);
			const bool refresh = cached->hasTexCoordInputs && !meshTextureMatricesMatch(saved);
			const u32 vertexBytes = cached->vertexCount * cached->vertexSize;
			reserveDisplayListMemory(refresh ? vertexBytes : 0);
			u8* storedVertices = cached->storage + kMeshCacheDataOffset;
			u8* vertices = storedVertices;
			cached->gpuEpoch = sMeshCacheEpoch;
			u16* indices = reinterpret_cast<u16*>(storedVertices + vertexBytes);
			if (refresh) {
				vertices = static_cast<u8*>(sceGuGetMemory(vertexBytes));
				const u32 inputOffset = (vertexBytes + cached->indexCount * sizeof(u16) + 3) & ~3U;
				const auto* inputs = reinterpret_cast<const MeshTexCoordInput*>(storedVertices + inputOffset);
				refreshMeshVertices(vertices, storedVertices, inputs, cached->vertexCount,
				                    cached->vertexSize == sizeof(PspGxSkinnedVertex));
				++sTimings.texCoordRefreshes;
				sTimings.texCoordRefreshVertices += cached->vertexCount;
			}
			bool skinned = loadMeshMatrices(cached->matrixMask, cached->rigidMatrix);
			submitVertices(vertices, cached->indexCount, GU_TRIANGLES, indices, skinned, !refresh,
			               refresh ? nullptr : meshCacheCommands(*cached));
			sDebugBatchedPrimitives += cached->primitiveCount;
			++sTimings.meshCacheHits;
			sTimings.cachedVertices += cached->vertexCount;
			return true;
		}
	}
	MeshLayout layout;
	if (!meshLayout(layout)) return false;
	MeshInputRange inputs[4] = {};
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
	const bool cacheable = useCache;
	if (cacheable) meshCacheKey(key);
	u32 vertexSize = skinned ? sizeof(PspGxSkinnedVertex) : sizeof(PspGxVertex);
	u32 byteCount = vertexCount * vertexSize + indexCount * sizeof(u16);
	// Align the auxiliary float inputs even when there is an odd index count.
	const u32 texCoordOffset = (byteCount + 3) & ~3U;
	const bool saveTexCoords = cacheable && key.generatedTexCoords
	    && texCoordOffset + vertexCount * sizeof(MeshTexCoordInput) + 8 + kDisplayListCommandReserve <= sizeof(sGxDisplayList);
	if (saveTexCoords) byteCount = texCoordOffset + vertexCount * sizeof(MeshTexCoordInput);
	if (byteCount + 8 + kDisplayListCommandReserve > sizeof(sGxDisplayList)) return false;
	reserveDisplayListMemory(byteCount);
	u8* vertices = static_cast<u8*>(sceGuGetMemory(byteCount));
	sTimings.decodedVertices += vertexCount;
	if (skinned) sTimings.skinnedVertices += vertexCount;
	u16* indices = reinterpret_cast<u16*>(vertices + vertexCount * vertexSize);
	auto* texCoordInputs = saveTexCoords ? reinterpret_cast<MeshTexCoordInput*>(vertices + texCoordOffset) : nullptr;
	u32 base = 0, index = 0;
	const bool tintVertices = prepareMaterialTint();
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
			if (cacheable) inputs[0].include(xyz, pos.count == GX_POS_XY ? 8 : 12);
			out.colour = sState.currentColour;
			if (col.source != GX_NONE) {
				const u8* colour = col.array + meshIndex(input, layout, GX_VA_CLR0) * col.stride;
				memcpy(&out.colour, colour, 4);
				if (cacheable) inputs[1].include(colour, 4);
			}
			if (tintVertices) out.colour = materialTint(out.colour);
			out.u = out.v = 0.0f;
			if (uv.source != GX_NONE) {
				const f32* st = reinterpret_cast<const f32*>(uv.array + meshIndex(input, layout, GX_VA_TEX0) * uv.stride);
				out.u = st[0]; out.v = uv.count == GX_TEX_S ? 0.0f : st[1];
				if (cacheable) inputs[2].include(st, uv.count == GX_TEX_S ? 4 : 8);
			}
			u32 textureMatrix = layout.offset[GX_VA_TEX0MTXIDX] >= 0
			    ? meshIndex(input, layout, GX_VA_TEX0MTXIDX) : sState.texMatrix;
			if (texCoordInputs) texCoordInputs[base + i] = {out.x, out.y, out.z,
			    sState.texSource == GX_TG_POS ? textureMatrix : (u32)GX_IDENTITY};
			generateTexCoord(out, out.x, out.y, out.z, GX_TG_POS, textureMatrix);
			if (sState.texSource == GX_TG_NRM) {
				GXAttr normalAttr = layout.offset[GX_VA_NBT] >= 0 ? GX_VA_NBT : GX_VA_NRM;
				const PspGxAttribute& normal = sState.attributes[normalAttr];
				if (layout.offset[normalAttr] >= 0 && normal.array && normal.componentType == GX_F32) {
					const f32* n = reinterpret_cast<const f32*>(normal.array + meshIndex(input, layout, normalAttr) * normal.stride);
					generateTexCoord(out, n[0], n[1], n[2], GX_TG_NRM, textureMatrix);
					if (texCoordInputs) texCoordInputs[base + i] = {n[0], n[1], n[2], textureMatrix};
					if (cacheable) inputs[3].include(n, 12);
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
	if (cacheable) cacheMesh(listStart, end - listStart, key, inputs, vertices, byteCount, indexCount,
	                         vertexCount, primitiveCount, rigidMatrix, matrixMask, vertexSize, saveTexCoords);
	loadMeshMatrices(matrixMask, rigidMatrix);
	submitVertices(vertices, indexCount, GU_TRIANGLES, indices, skinned);
	sDebugBatchedPrimitives += primitiveCount;
	return true;
}

} // namespace

namespace PikminPsp {
void setPresentationInterval(u32 refreshes) { sPresentationInterval = refreshes ? refreshes : 1; }

MeshValidationScope::MeshValidationScope() : previous(sMeshValidationScope), arrays{}, modelRevision(0) { sMeshValidationScope = this; }
MeshValidationScope::~MeshValidationScope() { sMeshValidationScope = previous; }

u64 nextMeshRevision()
{
	static u64 revision;
	return ++revision;
}

void bindMeshRevision(u64 revision)
{
	if (sMeshValidationScope) sMeshValidationScope->modelRevision = revision;
}

void validateMeshArray(GXAttr attribute, const void* source, u32 bytes)
{
	if (!sMeshValidationScope || (u32)attribute >= GX_VA_MAX_ATTR) return;
	u32 start = sceKernelGetSystemTimeLow();
	sMeshValidationScope->arrays[attribute] = validateMeshArrayStorage(source, bytes);
	sTimings.arrayValidation += (u32)(sceKernelGetSystemTimeLow() - start);
}

void drawMovieFrame(const u16* pixels, u32 width, u32 height, u32 stride)
{
	if (!sState.listOpen || !pixels) return;
	reserveDisplayListMemory(512);
	setGuEnabled(GU_DEPTH_TEST, false); setGuDepthMask(GU_TRUE);
	setGuEnabled(GU_BLEND, false); setGuEnabled(GU_ALPHA_TEST, false); setGuEnabled(GU_CULL_FACE, false);
	setGuEnabled(GU_FOG, false); setGuEnabled(GU_LIGHTING, false);
	setGuPixelMask(0); sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
	setGuEnabled(GU_TEXTURE_2D, true);
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
	if (prepareMaterialTint()) {
		for (u32 i = 0; i < sState.vertexCount; ++i) sState.vertices[i].colour = materialTint(sState.vertices[i].colour);
	}
	if (!sState.verticesPretransformed) for (u32 i = 0; i < sState.vertexCount; ++i) {
		PspGxVertex& vertex = sState.vertices[i];
		generateTexCoord(vertex, vertex.x, vertex.y, vertex.z);
	}
	if (sState.verticesPretransformed) {
		ScePspFMatrix4 identity = identityMatrix();
		sceGuSetMatrix(GU_MODEL, &identity);
		++sTimings.modelMatrixUploads;
		sState.guPositionMatrixValid = false;
	} else loadGuPositionMatrix(sState.currentPositionMatrix);
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
	sState.inPrimitive = false;
	sState.verticesPretransformed = false;
}

GXFifoObj* GXInit(void*, u32)
{
	clearMeshCache();
	sTimings = {};
	snapshotPerformanceCounters();
	sTimings.start = sceKernelGetSystemTimeLow();
	memset(&sState, 0, sizeof(sState));
	sMeshTimingSequence = 0;
	sState.currentColour = 0xffffffff;
	sState.clearColour = 0xff000000;
	sState.clearDepth = 0;
	sceGuInit();
	sState.raster = {};
	sceDisplaySetMode(0, kScreenWidth, kScreenHeight);
	sceGuStart(GU_DIRECT, sGxDisplayList);
	sceGuDrawBuffer(GU_PSM_8888, (void*)0, kBufferWidth);
	sceGuDispBuffer(kScreenWidth, kScreenHeight, (void*)kFrameBytes, kBufferWidth);
	sceGuDepthBuffer((void*)(kFrameBytes * 3), kBufferWidth);
	sSpareFrameBuffer = (void*)(kFrameBytes * 2);
	guSwapBuffersCallback(swapTripleBuffers);
	guSwapBuffersBehaviour(PSP_DISPLAY_SETBUF_NEXTFRAME);
	sceGuOffset(2048 - kScreenWidth / 2, 2048 - kScreenHeight / 2);
	sceGuViewport(2048, 2048, kScreenWidth, kScreenHeight);
	sceGuDepthRange(65535, 0);
	sceGuScissor(0, 0, kScreenWidth, kScreenHeight);
	setGuEnabled(GU_SCISSOR_TEST, true);
	setGuEnabled(GU_CLIP_PLANES, true);
	sceGuShadeModel(GU_SMOOTH);
	setGuFrontFace(GU_CW);
	// Match the working OOT PSP renderer: sceGuDrawArray consumes GE matrix
	// state directly, so initialise it with sceGuSetMatrix rather than GUM's
	// deferred software stacks.
	ScePspFMatrix4 identity = identityMatrix();
	sceGuSetMatrix(GU_PROJECTION, &identity);
	sceGuSetMatrix(GU_VIEW, &identity);
	sceGuSetMatrix(GU_MODEL, &identity);
	sState.guPositionMatrixValid = false;
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	meshCacheGpuIdle();
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	void* displayBuffer = reinterpret_cast<void*>((reinterpret_cast<uintptr_t>(sceGeEdramGetAddr()) | 0x40000000U) + kFrameBytes);
	sceDisplaySetFrameBuf(displayBuffer, kBufferWidth, GU_PSM_8888, PSP_DISPLAY_SETBUF_NEXTFRAME);
	sPresentationRetrace = sceDisplayGetVcount();
	sPresentationInterval = 1;
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
	if (!meshBytesEqual(sState.positionMatrices[index], matrix, sizeof(Mtx))) {
		memcpy(sState.positionMatrices[index], matrix, sizeof(Mtx));
		if (sState.guPositionMatrix == index) sState.guPositionMatrixValid = false;
	}
	// Publish the selected matrix only when geometry consumes it. A rigid
	// mesh can select another slot and skinning uses identity plus GE bones.
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
		setGuEnabled(GU_CULL_FACE, false);
	} else {
		setGuEnabled(GU_CULL_FACE, true);
		setGuFrontFace(mode == GX_CULL_FRONT ? GU_CCW : GU_CW);
	}
}

void GXSetZMode(GXBool compareEnable, GXCompare function, GXBool updateEnable)
{
	if (compareEnable) setGuEnabled(GU_DEPTH_TEST, true); else setGuEnabled(GU_DEPTH_TEST, false);
	setGuDepthFunction(guDepthCompare(function));
	setGuDepthMask(updateEnable ? GU_FALSE : GU_TRUE);
}

void GXSetBlendMode(GXBlendMode mode, GXBlendFactor source, GXBlendFactor destination, GXLogicOp)
{
	if (mode == GX_BM_NONE) {
		setGuEnabled(GU_BLEND, false);
		return;
	}
	setGuEnabled(GU_BLEND, true);
	u32 sourceFix = source == GX_BL_ONE ? 0xffffffff : 0;
	u32 destinationFix = destination == GX_BL_ONE ? 0xffffffff : 0;
	setGuBlend(mode == GX_BM_SUBTRACT ? GU_REVERSE_SUBTRACT : GU_ADD, guBlendFactor(source), guBlendFactor(destination), sourceFix,
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
	memset(sState.attributeSources, GX_NONE, sizeof(sState.attributeSources));
}
void GXSetVtxDesc(GXAttr attribute, GXAttrType source)
{
	if ((u32)attribute < GX_VA_MAX_ATTR) {
		sState.attributes[attribute].source = source;
		sState.attributeSources[attribute] = source;
	}
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
	// Rotate the phase so a stable draw order samples all material positions.
	sMeshTimingSequence = sDebugFrameCount;
	u32 waitStart = sceKernelGetSystemTimeLow();
	sceGuFinish();
	sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	meshCacheGpuIdle();
	sTimings.presentWait += (u32)(sceKernelGetSystemTimeLow() - waitStart);
	if (!PikminPsp::waitForFrameRetrace(sPresentationRetrace, sPresentationInterval)) ++sTimings.pacingBypasses;
	sState.drawBufferOffset = reinterpret_cast<uintptr_t>(sceGuSwapBuffers());
	sPresentationRetrace = sceDisplayGetVcount();
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
		sState.raster = {};
	}
	recordFrameTimings();
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
	u32 copyStart = sceKernelGetSystemTimeLow();
	sceGuFinish(); sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);
	meshCacheGpuIdle();
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
				texture.pixels[texturePixelOffset(x, y, texture.storageWidth, texture.storageHeight)] = rgbaTo4444(colour & 255, (colour >> 8) & 255,
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
		sState.raster = {};
	}
	sTimings.copyTime += (u32)(sceKernelGetSystemTimeLow() - copyStart);
	++sTimings.copies;
}

void GXSetColorUpdate(GXBool enabled) { setGuPixelMask(enabled ? 0 : 0xffffffff); }
void GXSetAlphaUpdate(GXBool) { }
void GXSetZCompLoc(GXBool) { }
void GXSetPixelFmt(GXPixelFmt, GXZFmt16) { }
void GXSetDither(GXBool enabled) { if (enabled) setGuEnabled(GU_DITHER, true); else setGuEnabled(GU_DITHER, false); }
void GXSetCoPlanar(GXBool) { }
void GXSetScissorBoxOffset(s32, s32) { }
void GXSetAlphaCompare(GXCompare compare0, u8 reference0, GXAlphaOp, GXCompare, u8)
{
	if (compare0 == GX_ALWAYS) {
		setGuEnabled(GU_ALPHA_TEST, false);
	} else {
		setGuEnabled(GU_ALPHA_TEST, true);
		setGuAlpha(guCompare(compare0), reference0, 0xff);
	}
}
void GXSetFog(GXFogType type, f32 start, f32 end, f32, f32, GXColor colour)
{
	if (type == GX_FOG_NONE) {
		setGuEnabled(GU_FOG, false);
	} else {
		setGuEnabled(GU_FOG, true);
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
	if (stage == GX_TEVSTAGE0) {
		sState.particleColourEnabled = false;
		sState.multiplyTevColour0 = false;
	}
}
void GXSetTevColorIn(GXTevStageID stage, GXTevColorArg a, GXTevColorArg b, GXTevColorArg c, GXTevColorArg d)
{
	if (stage == GX_TEVSTAGE0) sState.multiplyTevColour0 = a == GX_CC_ZERO && d == GX_CC_ZERO
	    && ((b == GX_CC_C0 && c == GX_CC_RASC) || (b == GX_CC_RASC && c == GX_CC_C0));
}
void GXSetTevAlphaIn(GXTevStageID stage, GXTevAlphaArg a, GXTevAlphaArg b, GXTevAlphaArg c, GXTevAlphaArg d)
{
	// Particle presets source opacity from REG0, not the raster colour or
	// the environment register written by setPrimEnv immediately afterwards.
	// GU modulates the texture with this primary colour (an approximation of
	// the RGB TEV presets), preserving the particle's animated alpha.
	if (stage == GX_TEVSTAGE0)
		sState.particleColourEnabled = a == GX_CA_ZERO && b == GX_CA_TEXA && c == GX_CA_A0 && d == GX_CA_ZERO;
}
void GXSetTevColorOp(GXTevStageID stage, GXTevOp op, GXTevBias bias, GXTevScale scale, GXBool, GXTevRegID)
{
	if (stage == GX_TEVSTAGE0) { sState.tevColourOp = op; sState.tevColourBias = bias; sState.tevColourScale = scale; }
}
void GXSetTevAlphaOp(GXTevStageID, GXTevOp, GXTevBias, GXTevScale, GXBool, GXTevRegID) { }
void GXSetTevColor(GXTevRegID reg, GXColor colour)
{
	sState.currentColour = GU_RGBA(colour.r, colour.g, colour.b, colour.a);
	if (reg == GX_TEVREG0) {
		sState.particleColour = sState.currentColour;
		sState.tevColour0 = (GXColorS10){colour.r, colour.g, colour.b, colour.a};
	}
}
void GXSetTevColorS10(GXTevRegID reg, GXColorS10 colour)
{
	if (reg == GX_TEVREG0) sState.tevColour0 = colour;
}
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
	PikminPsp::PerformanceScope timing(PikminPsp::PerformanceTexture);
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
	sceGuTexMode(GU_PSM_4444, 0, 0, textureSwizzled(texture->storageWidth, texture->storageHeight) ? GU_TRUE : GU_FALSE);
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
	const u32 timingWeight = (sMeshTimingSequence++ % kMeshTimingInterval) == 0 ? kMeshTimingInterval : 0;
	if (timingWeight) ++sTimings.meshTimingSamples;
	PikminPsp::PerformanceScope timing(PikminPsp::PerformanceMesh, timingWeight);
	const u8* input = static_cast<const u8*>(displayList);
	const u8* end = input + byteCount;
	if (drawMeshList(input, end, true, timingWeight)) return;
	++sTimings.fallbackLists;
	while (input < end) {
		u8 command = *input++;
		if (command == GX_CMD_NOP) continue;
		u8 opcode = command & GX_OPCODE_MASK;
		if (opcode >= GX_CMD_DRAW_QUADS && opcode <= GX_CMD_DRAW_POINTS) {
			if (end - input < 2) break;
			u16 vertexCount = readBig16(input);
			sTimings.decodedVertices += vertexCount;
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
