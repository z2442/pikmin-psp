// Run the original cutscene audio controller with deferred DVD callbacks and
// no worker thread, matching the PSP AudioLock used by the game-facing calls.
#include <math.h>
// The game headers use the PSPSDK spelling of this overload.
namespace std { using ::sqrtf; }
#include "MoviePlayer.h"
#include "jaudio/cmdqueue.h"
#include "jaudio/dvdthread.h"
#include "jaudio/interface.h"
#include "jaudio/jammain_2.h"
#include "jaudio/piki_bgm.h"
#include "jaudio/piki_player.h"
#include "jaudio/piki_scene.h"
#include "jaudio/pikidemo.h"
#include "jaudio/pikiinter.h"
#include "jaudio/verysimple.h"
#include "jaudio/waveread.h"
#include "port/psp/PspAudio.h"
#include <assert.h>
#include <stdio.h>

namespace {
struct Completion { Jac_DVDCallback callback; u32 owner; };
Completion completions[16];
u32 pending;
bool locked, waveAvailable = true;
u32 streamStarts, streamStops, preparedStream, completionCount;
seqp_ track;
}

namespace PikminPsp {
void audioLock() { assert(!locked); locked = true; }
void audioUnlock() { assert(locked); locked = false; }
void audioLog(const char*, ...) {}
void audioDrainCompletions()
{
	assert(locked);
	while (pending) {
		Completion item = completions[0];
		for (u32 i = 1; i < pending; ++i) completions[i - 1] = completions[i];
		--pending;
		++completionCount;
		item.callback(item.owner);
	}
}
}
void DVDT_CheckPass(u32 owner, u32* status, Jac_DVDCallback callback)
{
	assert(locked);
	if (status) *status = 1;
	assert(pending < 16);
	completions[pending++] = {callback, owner};
}

// Only loading and sound output are faked; Jac_StartDemo, Jac_DemoFrame and
// Jac_FinishDemo below execute the real game logic and timed-event tables.
void Jac_SetProcessStatus(u32) {}
BOOL Jac_TellChgMode() { return FALSE; }
void Jac_PlaySystemSe(s32) {}
void Jac_PlayOrimaSe(u32) {}
void Jac_Orima_Formation(s32, s32) {}
void Jac_InitAllEvent() {}
void Jac_Delete_CurrentBgmWave() {}
void Jac_StopBgm(u32) {}
void Jac_FadeOutBgm(u32, u32) {}
void Jac_DemoFade(u8, u32, f32) {}
void Jac_PrepareDemoSound(u32 id) { preparedStream = id; }
void Jac_StartDemoSound(u32 id) { assert(id == preparedStream); ++streamStarts; }
void Jac_StopDemoSound(u32) { ++streamStops; }
BOOL WaveScene_Set(u32, u32) { return waveAvailable; }
void WaveScene_Erase(u32, u32) {}
seqp_* Jam_GetTrackHandle(u32) { return &track; }
void Jam_PauseTrack(seqp_*, u8) {}
BOOL Jam_WritePortAppDirect(seqp_*, u32, u16) { return TRUE; }
void Jal_AddCmdQueue(CmdQueue*, seqp_*, u8) {}
void Jal_SendCmdQueue_Noblock(CmdQueue*, u16) {}

static void start(u32 id)
{
	PikminPsp::AudioLock lock;
	Jac_StartDemo(id);
}

static void finish()
{
	PikminPsp::AudioLock lock;
	Jac_FinishDemo();
}

int main()
{
	Jac_DemoSceneInit();
	start(DEMOID_OpeningIntroPt1);
	assert(preparedStream == 8);
	{
		PikminPsp::AudioLock lock;
		for (int frame = 0; frame < 1200; ++frame) {
			assert(Jac_DemoFrame(frame));
			assert(streamStarts == (frame >= 4 ? 1U : 0U));
		}
	}
	finish();
	assert(streamStops == 0); // The same opening.stx continues into part two.
	start(DEMOID_OpeningIntroPt2); // Previously spins waiting for its DVD callback.
	assert(completionCount == 1 && pending == 0);
	assert(streamStarts == 1 && streamStops == 0);
	{
		PikminPsp::AudioLock lock;
		for (int frame = 0; frame < 420; ++frame) assert(Jac_DemoFrame(frame));
	}
	finish();
	assert(streamStops == 1);
	puts("Opening cutscene handoff and continuous stream checks passed");

	// Non-stream cutscenes use the same deferred barrier, including preloads
	// within Jac_StartDemo. An unavailable wave bank must not block gameplay.
	start(DEMOID_OlimarWakeUp);
	finish();
	start(DEMOID_CollectFirstPartPractice);
	finish();
	waveAvailable = false;
	start(DEMOID_MeetRedPikmin);
	finish();
	puts("Cutscene preloads and unavailable wave bank checks passed");
}

#include "../src/jaudio/pikidemo.c"
