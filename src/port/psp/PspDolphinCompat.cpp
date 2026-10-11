#include "Dolphin/ar.h"
#include "Dolphin/hio.h"
#include "Dolphin/pad.h"
#include "Dolphin/vi.h"
#include "port/psp/PspPerformance.h"
#include "port/psp/PspPlatform.h"

#include <pspdisplay.h>

#include <malloc.h>
#include <string.h>

namespace {

const u32 kAramBase = 0x00800000;
const u32 kAramSize = 0x00800000;
u8* sAram;
VIRetraceCallback sPreRetrace;
VIRetraceCallback sPostRetrace;

void dispatchRetrace(u32 count)
{
	if (sPreRetrace) sPreRetrace(count);
	if (sPostRetrace) sPostRetrace(count);
}

u8* aramAddress(u32 address, u32 length)
{
	if (!sAram || address < kAramBase || length > kAramSize || address - kAramBase > kAramSize - length) {
		return nullptr;
	}
	return sAram + (address - kAramBase);
}

} // namespace

extern "C" {

u32 ARInit(u32*, u32)
{
	if (!sAram) {
		sAram = static_cast<u8*>(memalign(64, kAramSize));
		if (sAram) memset(sAram, 0, kAramSize);
	}
	return sAram ? kAramSize : 0;
}

void ARQInit(void) { }

void ARQPostRequest(ARQRequest* request, u32 owner, u32 type, u32 priority, u32 source, u32 dest, u32 length,
	                 ARQCallback callback)
{
	if (request) {
		request->owner = owner;
		request->type = type;
		request->priority = priority;
		request->source = source;
		request->dest = dest;
		request->length = length;
		request->callback = callback;
	}
	if (type == ARQ_TYPE_MRAM_TO_ARAM) {
		u8* output = aramAddress(dest, length);
		if (output) memcpy(output, reinterpret_cast<const void*>(source), length);
	} else {
		const u8* input = aramAddress(source, length);
		if (input) memcpy(reinterpret_cast<void*>(dest), input, length);
	}
	if (callback) callback(reinterpret_cast<u32>(request));
}

BOOL HIOEnumDevices(HIOEnumCallback) { return FALSE; }
BOOL HIOInit(s32, HIOCallback) { return FALSE; }
BOOL HIOReadMailbox(u32*) { return FALSE; }
BOOL HIOWriteMailbox(u32) { return FALSE; }
BOOL HIOWrite(u32, void*, s32) { return FALSE; }

void PADControlMotor(s32, u32) { }
BOOL PADRecalibrate(u32) { return TRUE; }

void __VIInit(VITVMode) { }
void VIInit(void) { }
void VIConfigure(const GXRenderModeObj*) { }
void VIFlush(void) { }
void VISetNextFrameBuffer(void*) { }
void VISetBlack(BOOL) { }
u32 VIGetDTVStatus(void) { return 1; }
// Count refreshes during rendering too. A synthetic counter incremented only
// by VIWaitForRetrace makes slow frames pay the full pacing delay again.
u32 VIGetRetraceCount(void) { return sceDisplayGetVcount(); }
VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback)
{
	VIRetraceCallback previous = sPreRetrace;
	sPreRetrace = callback;
	return previous;
}
VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback)
{
	VIRetraceCallback previous = sPostRetrace;
	sPostRetrace = callback;
	return previous;
}
void VIWaitForRetrace(void)
{
	PikminPsp::PerformanceScope timing(PikminPsp::PerformancePacing);
	sceDisplayWaitVblankStart();
	dispatchRetrace(VIGetRetraceCount());
}

} // extern "C"

namespace PikminPsp {
u32 waitForFrameRetrace(u32 previousRetrace, u32 refreshes)
{
	// At least one refresh must pass between swaps: NEXTFRAME must latch
	// before the following swap can recycle its formerly displayed buffer.
	if (!refreshes) refreshes = 1;
	u32 waits = 0;
	while ((u32)(VIGetRetraceCount() - previousRetrace) < refreshes) {
		VIWaitForRetrace();
		++waits;
	}
	// Slow frames still service DVD/loading callbacks, without adding a wait.
	if (!waits) dispatchRetrace(VIGetRetraceCount());
	return waits;
}
}
