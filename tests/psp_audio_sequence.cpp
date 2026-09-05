// Exercise the original sequence lifecycle without an audio worker. PSP game
// calls hold AudioLock, so a blocking stop must complete on the calling thread.
#include "jaudio/driverinterface.h"
#include "jaudio/fat.h"
#include "jaudio/file_seq.h"
#include "jaudio/jammain_2.h"
#include "jaudio/jamosc.h"
#include "jaudio/noteon.h"
#include "jaudio/oneshot.h"
#include "jaudio/playercall.h"
#include "jaudio/seqsetup.h"
#include "jaudio/virload.h"
#include "port/psp/PspAudio.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// File IO, bytecode execution and voice rendering are outside this lifecycle
// test. Track allocation, stop processing and callback dispatch are real.
BOOL JV_InitHeader_M(const char*, u8*, u8*) { return TRUE; }
u32 JV_GetArchiveHandle(const char*) { return 0; }
u32 JV_CheckSize(u32) { return 1; }
u32 JV_LoadFile(u32, u8* dst, u32, u32) { *dst = 0xff; return TRUE; }
namespace PikminPsp {
void audioRegisterSequence(const void*, u32) {}
}
BOOL OSDisableInterrupts() { return TRUE; }
BOOL OSRestoreInterrupts(BOOL) { return TRUE; }
int FAT_AllocateMemory(u32) { assert(false); return 0; }
int FAT_StoreBlock(u8*, u16, u32, u32) { assert(false); return 0; }
int FAT_FreeMemory(u16) { assert(false); return 0; }
void Jam_InitRegistTrack() {}
void Jam_UnRegistTrack(seqp_*) {}
void Jam_InitExtBuffer(OuterParam_* p) { memset(p, 0, sizeof(*p)); }
BOOL Jam_AssignExtBuffer(seqp_* track, OuterParam_* p)
{
	track->outerParams = p;
	++p->refCount;
	return TRUE;
}
void Jam_UpdateTrackAll(seqp_*) {}
void Osc_Init_Env(seqp_*) {}
void Init_1shot(jcs_*, u32) {}
void SeqUpdate(seqp_*, u32) {}
static u32 sequenceTicks, rootNotesOff, childNotesOff, releasedRoots;
s32 Jam_SeqmainNote(seqp_*, u8) { ++sequenceTicks; return 0; }
s32 NoteOFF_R(seqp_*, u8, u16) { ++rootNotesOff; return 0; }
s32 NoteOFF(seqp_*, u8) { ++childNotesOff; return 0; }
int FixReleaseChannelAll(jcs_*) { ++releasedRoots; return 0; }
int FixMoveChannelAll(jcs_*, jcs_*) { return 0; }

int main()
{
	ResetPlayerCallback();
	Jaq_Reset();
	Jaf_InitSeqArchive2("Seqs/test.arc", nullptr, nullptr);
	u8 data;
	assert(Jaf_LoadSeq(2, &data));
	assert(!Jaf_StopSeq(3));
	assert(!Jaq_StopSeq(-1));

	// Stopping a prepared sequence must also free the root without callbacks.
	u32 handle = Jaf_ReadySeq(3, 2);
	assert(Jaq_HandleToSeq(handle));
	assert(Jaf_StopSeq(3));
	assert(!Jaq_HandleToSeq(handle));

	// Keep another root playing to check that a stop does not advance unrelated
	// music or effects. Repeat beyond the 16 callback slots to catch leaks and
	// stale callbacks when the same root is immediately reused.
	assert(Jaf_StartSeq(0, 2));
	for (u32 i = 0; i < 40; ++i) {
		handle = Jaf_ReadySeq(3, 2);
		assert(Jaf_PlaySeq(3));
		seqp_* track = Jaq_HandleToSeq(handle);
		assert(track && track->trackState == 1);
		DspPlayerCallback();
		assert(sequenceTicks == i * 2 + 2);

		seqp_ child = {};
		child.trackState = 1;
		child.parent = track;
		track->children[0] = &child;
		assert(Jaf_StopSeq(3)); // Hung here before the PSP synchronous stop fix.
		assert(!Jaq_HandleToSeq(handle));
		assert(!track->trackState && !child.trackState);
		assert(!track->children[0] && !track->outerParams);
		assert(sequenceTicks == i * 2 + 2);
		assert(rootNotesOff == (i + 1) * 8);
		assert(childNotesOff == (i + 1) * 8);
		assert(releasedRoots == i + 1);
		assert(!Jaf_StopSeq(3));
	}
	assert(Jaf_StopSeq(0));
	DspPlayerCallback();
	assert(sequenceTicks == 80);
	puts("Sequence stop, child cleanup, immediate restart and callback reuse checks passed");
}
