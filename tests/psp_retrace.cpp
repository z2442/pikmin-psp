#include "../src/port/psp/PspDolphinCompat.cpp"
#include <cassert>
#include <cstdio>

namespace PikminPsp {
void performanceAdd(PerformanceMetric, u32) {}
}
extern "C" unsigned int sceKernelGetSystemTimeLow(void) { return 0; }

static unsigned int vcount;
static u32 preCount, postCount;
static u32 waits, callbacks;
extern "C" unsigned int sceDisplayGetVcount(void) { return vcount; }
extern "C" int sceDisplayWaitVblankStart(void) { ++vcount; ++waits; return 0; }
static void pre(u32 count) { preCount = count; }
static void post(u32 count) { postCount = count; ++callbacks; }

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
	// An already late frame services callbacks without an extra vblank wait.
	vcount = 105; waits = callbacks = 0;
	PikminPsp::waitForFrameRetrace(100, 2);
	assert(waits == 0 && callbacks == 1 && preCount == 105 && postCount == 105);
	// Fast 60/30 FPS frames wait only for their remaining refreshes.
	vcount = 100; waits = callbacks = 0;
	PikminPsp::waitForFrameRetrace(100, 1);
	assert(vcount == 101 && waits == 1 && callbacks == 1);
	vcount = 100; waits = callbacks = 0;
	PikminPsp::waitForFrameRetrace(100, 2);
	assert(vcount == 102 && waits == 2 && callbacks == 2);
	vcount = 101; waits = callbacks = 0;
	PikminPsp::waitForFrameRetrace(100, 2);
	assert(vcount == 102 && waits == 1 && callbacks == 1);
	// Zero interval still waits for a pending NEXTFRAME swap to latch.
	vcount = 100; waits = callbacks = 0;
	PikminPsp::waitForFrameRetrace(100, 0);
	assert(vcount == 101 && waits == 1);
	vcount = 0; waits = callbacks = 0;
	PikminPsp::waitForFrameRetrace(0xffffffffU, 2);
	assert(vcount == 1 && waits == 1 && callbacks == 1);
	puts("PSP retrace elapsed-refresh, callback, and wraparound checks passed");
}
