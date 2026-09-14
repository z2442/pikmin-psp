#include "Dolphin/ar.h"
#include "Dolphin/card.h"
#include "Dolphin/hio.h"
#include "Dolphin/pad.h"
#include "Dolphin/vi.h"
#include "port/psp/PspPerformance.h"

#include <pspdisplay.h>

#include <malloc.h>
#include <string.h>

namespace {

const u32 kAramBase = 0x00800000;
const u32 kAramSize = 0x00800000;
u8* sAram;
VIRetraceCallback sPreRetrace;
VIRetraceCallback sPostRetrace;

u8* aramAddress(u32 address, u32 length)
{
	if (!sAram || address < kAramBase || length > kAramSize || address - kAramBase > kAramSize - length) {
		return nullptr;
	}
	return sAram + (address - kAramBase);
}

void cardCallback(CARDCallback callback, s32 channel, s32 result)
{
	if (callback) callback(channel, result);
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

void CARDInit(void) { }
BOOL CARDProbe(s32) { return FALSE; }
s32 CARDGetResultCode(s32) { return CARD_RESULT_NOCARD; }
s32 CARDCheck(s32) { return CARD_RESULT_NOCARD; }
s32 CARDCheckAsync(s32 channel, CARDCallback callback)
{
	cardCallback(callback, channel, CARD_RESULT_NOCARD);
	return CARD_RESULT_NOCARD;
}
s32 CARDMount(s32, CARDMemoryCard*, CARDCallback) { return CARD_RESULT_NOCARD; }
s32 CARDMountAsync(s32 channel, CARDMemoryCard*, CARDCallback, CARDCallback callback)
{
	cardCallback(callback, channel, CARD_RESULT_NOCARD);
	return CARD_RESULT_NOCARD;
}
s32 CARDUnmount(s32) { return CARD_RESULT_READY; }
s32 CARDFormat(s32) { return CARD_RESULT_NOCARD; }
s32 CARDFormatAsync(s32 channel, CARDCallback callback)
{
	cardCallback(callback, channel, CARD_RESULT_NOCARD);
	return CARD_RESULT_NOCARD;
}
s32 CARDFreeBlocks(s32, s32* bytes, s32* files)
{
	if (bytes) *bytes = 0;
	if (files) *files = 0;
	return CARD_RESULT_NOCARD;
}
s32 CARDGetSectorSize(s32, u32* size)
{
	if (size) *size = 8192;
	return CARD_RESULT_NOCARD;
}
s32 CARDOpen(s32, const char*, CARDFileInfo*) { return CARD_RESULT_NOCARD; }
s32 CARDFastOpen(s32, s32, CARDFileInfo*) { return CARD_RESULT_NOCARD; }
s32 CARDClose(CARDFileInfo*) { return CARD_RESULT_READY; }
s32 CARDCreate(s32, const char*, u32, CARDFileInfo*) { return CARD_RESULT_NOCARD; }
s32 CARDRead(CARDFileInfo*, void*, s32, s32) { return CARD_RESULT_NOCARD; }
s32 CARDWrite(CARDFileInfo*, void*, s32, s32) { return CARD_RESULT_NOCARD; }
s32 CARDGetXferredBytes(s32) { return 0; }
s32 CARDFastDelete(s32, s32) { return CARD_RESULT_NOCARD; }
s32 CARDRename(s32, const char*, const char*) { return CARD_RESULT_NOCARD; }
s32 CARDGetStatus(s32, s32, CARDStat*) { return CARD_RESULT_NOCARD; }
s32 CARDSetStatus(s32, s32, CARDStat*) { return CARD_RESULT_NOCARD; }

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
	u32 retraceCount = VIGetRetraceCount();
	if (sPreRetrace) sPreRetrace(retraceCount);
	if (sPostRetrace) sPostRetrace(retraceCount);
}

} // extern "C"
