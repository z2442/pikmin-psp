#include "../src/port/psp/PspPlatform.cpp"
#include <cassert>
#include <cstdio>

static unsigned int mockTime;
static bool failStatus;
extern "C" unsigned int sceKernelGetSystemTimeLow(void) { return mockTime; }
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
	sPerformanceMainThread = 7;
	performanceSetAudioThread(8);
	assert(performanceThreadMicroseconds(false) == 1234567);
	assert(performanceThreadMicroseconds(true) == 7654321);
	failStatus = true;
	assert(performanceThreadMicroseconds(false) == 0);
	puts("PSP performance counter, scope wraparound, and thread-clock checks passed");
}
