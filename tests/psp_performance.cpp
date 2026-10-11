#include "../src/port/psp/PspPlatform.cpp"
#include <cassert>
#include <cstdio>
#include <initializer_list>

static unsigned int mockTime;
static unsigned int clockReads;
static bool failStatus;
extern "C" unsigned int sceKernelGetSystemTimeLow(void) { ++clockReads; return mockTime; }
extern "C" int sceKernelReferThreadRunStatus(SceUID id, SceKernelThreadRunStatus* status)
{
	assert(status->size == sizeof(*status));
	if (failStatus) return -1;
	status->runClocks.low = id == 7 ? 1234567U : 7654321U;
	status->runClocks.hi = 1;
	return 0;
}
extern "C" int sceKernelSysClock2USec(SceKernelSysClock* clock, unsigned int* low, unsigned int* high)
{
	u64 time = ((u64)clock->hi << 32) | clock->low;
	*low = time / 1000000; *high = time % 1000000; return 0;
}

int main()
{
	using namespace PikminPsp;
	// Timed scopes preserve microsecond units and wrap over the 32-bit clock.
	mockTime = 0xfffffff0U;
	{
		PerformanceScope timer(PerformanceMesh);
		mockTime = 0x20;
	}
	assert(performanceCounter(PerformanceMesh) == 48);
	u32 snapshot = performanceCounter(PerformanceMesh);
	performanceAdd(PerformanceMesh, 25);
	assert(performanceCounter(PerformanceMesh) - snapshot == 25);
	assert(performanceCounter(PerformanceTexture) == 0);
	clockReads = 0;
	{
		PerformanceScope timer(PerformanceMesh, 0);
		mockTime += 100;
	}
	assert(clockReads == 0 && performanceCounter(PerformanceMesh) == snapshot + 25);
	mockTime = 0xfffffff0U;
	{
		PerformanceScope timer(PerformanceMesh, 16);
		mockTime = 0x20;
	}
	assert(clockReads == 2 && performanceCounter(PerformanceMesh) == snapshot + 25 + 48 * 16);
	// Every output buffer samples one tick, with equal coverage of all eight
	// positions over eight buffers. Only enabled scopes read the kernel clock.
	u32 positions[8] = {}, samples = 0;
	clockReads = 0;
	for (u32 tick = 0; tick < 400; ++tick) {
		u32 weight = audioTimingWeight(tick);
		if (weight) { ++samples; ++positions[tick % 8]; }
		for (PerformanceMetric metric : {PerformanceAudioSequence, PerformanceAudioChannels, PerformanceAudioStream,
		                                PerformanceAudioMix, PerformanceAudioMovie}) {
			PerformanceScope timer(metric, weight);
			mockTime += 10;
		}
	}
	assert(samples == 50 && clockReads == 500); // Previously 4,000 clock reads.
	for (u32 position = 0; position < 8; ++position) assert(positions[position] == (position < 2 ? 7 : 6));
	assert(performanceCounter(PerformanceAudioSequence) == 4000);
	for (u32 first : {0U, 0xfffffff8U}) {
		u32 count = 0;
		for (u32 tick = 0; tick < 8; ++tick) if (audioTimingWeight(first + tick)) ++count;
		assert(count == 1);
	}
	sPerformanceMainThread = 7;
	performanceSetAudioThread(8);
	assert(performanceThreadMicroseconds(false) == 1234567);
	assert(performanceThreadMicroseconds(true) == 7654321);
	failStatus = true;
	assert(performanceThreadMicroseconds(false) == 0);
	puts("PSP performance counter, scope wraparound, and thread-clock checks passed");
}
