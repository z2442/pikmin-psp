#include "../src/port/psp/PspDolphinCompat.cpp"
#include <cassert>
#include <cstdio>

namespace PikminPsp {
void performanceAdd(PerformanceMetric, u32) {}
}
extern "C" unsigned int sceKernelGetSystemTimeLow(void) { return 0; }

static unsigned int vcount;
static u32 preCount, postCount;
extern "C" unsigned int sceDisplayGetVcount(void) { return vcount; }
extern "C" int sceDisplayWaitVblankStart(void) { ++vcount; return 0; }
static void pre(u32 count) { preCount = count; }
static void post(u32 count) { postCount = count; }

int main()
{
	vcount = 40;
	assert(VIGetRetraceCount() == 40);
	// A slow render consumes several refreshes without any explicit VI wait.
	vcount += 5;
	assert(VIGetRetraceCount() == 45);
	VISetPreRetraceCallback(pre);
	VISetPostRetraceCallback(post);
	VIWaitForRetrace();
	assert(VIGetRetraceCount() == 46);
	assert(preCount == 46 && postCount == 46);
	vcount = 0xffffffffU;
	VIWaitForRetrace();
	assert(VIGetRetraceCount() == 0);
	assert(preCount == 0 && postCount == 0);
	puts("PSP retrace elapsed-refresh, callback, and wraparound checks passed");
}
