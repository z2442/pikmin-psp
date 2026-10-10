#include "port/psp/PspMovie.h"
#include "port/psp/PspAudio.h"
#include "port/psp/PspPerformance.h"
#include "jaudio/app_inter.h"
#include <pspkernel.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

namespace {
const char* filePath;
bool locked, ready = true;
u64 clockValue;
u32 draws;
u32 publishedBytes;
u32 metrics[PikminPsp::PerformanceMetricCount];
}
namespace PikminPsp {
void performanceAdd(PerformanceMetric metric, u32 microseconds) { metrics[metric] += microseconds; }
bool audioReady() { return ready; }
void audioLock() { assert(!locked); locked = true; }
void audioUnlock() { assert(locked); locked = false; }
void audioDrainCompletions() { assert(locked); }
void audioLog(const char*, ...) {}
u64 timeMicroseconds() { return clockValue; }
bool resolvePath(char* out, u32 size, const char*)
{
	return snprintf(out, size, "%s", filePath) < (int)size;
}
void drawMovieFrame(const u16* rgb, u32 width, u32 height, u32 stride)
{
	assert(rgb && width == 360 && height == 270 && stride == 512); ++draws;
}
}
unsigned int sceKernelGetSystemTimeLow(void) { static u32 time; return ++time; }
void sceKernelDcacheWritebackRange(const void*, unsigned int bytes) { publishedBytes += bytes; }

int main(int argc, char** argv)
{
	assert(argc >= 2);
	filePath = argv[1];
	void* picture = nullptr;
	int width, height;
	// The fixture contains two real frames, so EOF and restart are quick.
	for (int run = 0; run < 3; ++run) {
		publishedBytes = 0;
		clockValue = 0;
		Jac_StreamMovieInit("../MovieData/test.h4m", nullptr, 0);
		assert(publishedBytes == 512 * (512 + 270) * sizeof(u16));
		assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == 1);
		assert(picture && width == 640 && height == 480);
		PikminPsp::movieDraw();
		assert(draws == u32(run * 2 + 1));
		assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == 0 && !picture);
		clockValue = 33366;
		assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == 0);
		clockValue = 33367;
		assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == 2 && picture);
		assert(publishedBytes == 512 * (512 + 270 * 2) * sizeof(u16));
		PikminPsp::movieDraw();
		clockValue = 66734;
		assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == -1 && !picture);
		Jac_StreamMovieStop();
		PikminPsp::movieDraw();
		assert(draws == u32(run * 2 + 2));
		assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == -1);
	}
	assert(metrics[PikminPsp::PerformanceMovieDecode] == 9);
	assert(metrics[PikminPsp::PerformanceMovieConvert] == 6);
	assert(metrics[PikminPsp::PerformanceMoviePublish] == 6);
	Jac_StreamMovieInit("test.h4m", nullptr, 0);
	Jac_StreamMovieStop(); // User skip must release both the movie and audio.
	Jac_StreamMovieStop();
	assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == -1);
	s16 pcm[160]; for (s16& v : pcm) v = 123;
	PikminPsp::movieMix(pcm, 80);
	for (s16 v : pcm) assert(v == 123);
	ready = false;
	Jac_StreamMovieInit("test.h4m", nullptr, 0);
	assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == 1);
	Jac_StreamMovieStop();
	for (int i = 1; i < argc; ++i) {
		filePath = i == 1 ? "/no-such-movie.h4m" : argv[i];
		Jac_StreamMovieInit("test.h4m", nullptr, 0);
		assert(Jac_StreamMovieGetPicture(&picture, &width, &height) == -1 && !picture);
		Jac_StreamMovieStop();
	}
	puts("Movie timing, EOF, skip, restart, invalid-file and audio-disabled playback checks passed");
}
