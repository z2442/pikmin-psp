#include "port/psp/PspMovie.h"
#include "port/psp/PspMovieDecode.h"
#include "port/psp/PspAudio.h"
#include "port/psp/PspPlatform.h"
#include "jaudio/app_inter.h"
#include <pspkernel.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

namespace {
PikminPsp::MovieFile sMovie;
PikminPsp::MovieAudio sAudio;
u16* sRgb;
const u32 Width = 360, Height = 270, Stride = 512;
u64 sStart;
bool sActive, sDone = true, sPicture, sNewPicture;

bool queueAudio(const u8* data, u32 size, void*)
{
	if (!PikminPsp::audioReady()) return true;
	PikminPsp::AudioLock lock;
	return sAudio.append(data, size);
}
bool decodeFrame()
{
	int result = sMovie.nextFrame(queueAudio);
	if (result <= 0) {
		if (result < 0) PikminPsp::audioLog("movie decode failed: %s", sMovie.error());
		else PikminPsp::audioLog("movie finished: %u frames", (unsigned)sMovie.decodedFrames());
		sDone = true;
		return false;
	}
	PikminPsp::movieConvertRgb565(sMovie.picture(), sMovie.width(), sMovie.height(), sRgb, Width, Height, Stride);
	sceKernelDcacheWritebackRange(sRgb, Stride * 512 * sizeof(u16));
	sPicture = sNewPicture = true;
	return true;
}
}

namespace PikminPsp {
void movieMix(s16* output, u32 frames)
{
	if (!sAudio.playing) return;
	// Keep JAM advancing while the movie owns the audible output. Its tracks
	// resume at normal volume when playback ends or is skipped.
	memset(output, 0, frames * 2 * sizeof(s16));
	sAudio.mix(output, frames);
}
void movieDraw()
{
	if (sActive && sPicture) drawMovieFrame(sRgb, Width, Height, Stride);
}
void movieShutdown()
{
	{
		AudioLock lock;
		sAudio.close();
	}
	sMovie.close();
	// Rendering is submitted on the game thread and GXCopyDisp waits for the
	// GE each frame. Stop is called during the following update, before draw.
	free(sRgb); sRgb = nullptr;
	sActive = sPicture = sNewPicture = false; sDone = true;
}
}

extern "C" void Jac_StreamMovieInit(const char* name, u8*, int)
{
	using namespace PikminPsp;
	movieShutdown();
	// The original names are relative to SndData. The PSP resolves the exact
	// MovieData file beside the extracted assets, independently of audio setup.
	const char* base = strrchr(name, '/'); base = base ? base + 1 : name;
	char relative[384], path[512];
	int n = snprintf(relative, sizeof(relative), "data/dataDir/MovieData/%s", base);
	if (n < 0 || n >= (int)sizeof(relative) || !resolvePath(path, sizeof(path), relative)) return;
	if (!sMovie.open(path)) { audioLog("movie open failed: %s: %s", base, sMovie.error()); return; }
	sRgb = (u16*)memalign(64, Stride * 512 * sizeof(u16));
	if (!sRgb) { audioLog("movie RGB allocation failed"); movieShutdown(); return; }
	memset(sRgb, 0, Stride * 512 * sizeof(u16));
	{
		AudioLock lock;
		if (audioReady()) {
			if (!sAudio.open(sMovie.audioFormat(), sMovie.audioRate())) {
				audioLog("movie audio allocation failed");
				sMovie.close(); free(sRgb); sRgb = nullptr;
				return;
			}
		}
	}
	sActive = true; sDone = false;
	if (!decodeFrame()) { movieShutdown(); return; }
	sStart = timeMicroseconds();
	{
		AudioLock lock;
		sAudio.playing = true;
	}
	audioLog("movie started: %s %ux%u frames=%u period=%u audio=%u/%u", base,
	         (unsigned)sMovie.width(), (unsigned)sMovie.height(), (unsigned)sMovie.frameCount(),
	         (unsigned)sMovie.framePeriod(), (unsigned)sMovie.audioFormat(), (unsigned)sMovie.audioRate());
}

extern "C" void Jac_StreamMovieUpdate()
{
	if (!sActive || sDone) return;
	u64 elapsed = PikminPsp::timeMicroseconds() - sStart;
	// Process one dependent frame per game update, never spin waiting for a
	// worker or a future timestamp. Input and the exit callback stay responsive.
	if (u64(sMovie.decodedFrames()) * sMovie.framePeriod() <= elapsed) decodeFrame();
}

extern "C" int Jac_StreamMovieGetPicture(void* picture, int* width, int* height)
{
	if (picture) *static_cast<void**>(picture) = nullptr;
	if (width) *width = 0;
	if (height) *height = 0;
	Jac_StreamMovieUpdate();
	if (!sActive || sDone) return -1;
	if (!sNewPicture) return 0;
	sNewPicture = false;
	if (picture) *static_cast<void**>(picture) = const_cast<u8*>(sMovie.picture());
	if (width) *width = sMovie.width();
	if (height) *height = sMovie.height();
	return sMovie.decodedFrames();
}

extern "C" void Jac_StreamMovieStop() { PikminPsp::movieShutdown(); }
