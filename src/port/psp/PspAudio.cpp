#include "jaudio/app_inter.h"
#include "jaudio/interface.h"
#include "jaudio/piki_bgm.h"
#include "jaudio/piki_player.h"
#include "jaudio/piki_scene.h"
#include "jaudio/pikidemo.h"
#include "jaudio/pikiinter.h"
#include "jaudio/verysimple.h"

#include "port/psp/PspAudio.h"

extern "C" {
void PspImpl_Jac_AddDVDBuffer(u8* a0, u32 a1);
void PspImpl_Jac_BackDVDBuffer();
int PspImpl_Jac_CheckFreeEvents();
int PspImpl_Jac_CreateEvent(u32 a0, SVector_* a1);
BOOL PspImpl_Jac_DemoFrame(int a0);
BOOL PspImpl_Jac_DestroyEvent(s32 a0);
void PspImpl_Jac_EnterBossMode();
void PspImpl_Jac_ExitBossMode();
void PspImpl_Jac_FinishDemo();
void PspImpl_Jac_FinishPartsFindDemo();
void PspImpl_Jac_FinishTextDemo();
void PspImpl_Jac_Freeze();
void PspImpl_Jac_Freeze_Precall();
void PspImpl_Jac_Gsync();
void PspImpl_Jac_InitAllEvent();
void PspImpl_Jac_Orima_Formation(s32 a0, s32 a1);
void PspImpl_Jac_Orima_Walk(s32 a0, u32 a1);
void PspImpl_Jac_OutputMode(int a0);
void PspImpl_Jac_Piki_Number(u32 a0);
BOOL PspImpl_Jac_PlayEventAction(int a0, int a1);
void PspImpl_Jac_PlayOrimaSe(u32 a0);
void PspImpl_Jac_PlaySystemSe(s32 a0);
void PspImpl_Jac_SceneExit(u32 a0, u32 a1);
void PspImpl_Jac_SceneSetup(u32 a0, u32 a1);
void PspImpl_Jac_SetBGMVolume(u8 a0);
void PspImpl_Jac_SetDemoOnyons(int a0);
void PspImpl_Jac_SetDemoPartsCount(int a0);
void PspImpl_Jac_SetDemoPartsID(int a0);
void PspImpl_Jac_SetSEVolume(u8 a0);
void PspImpl_Jac_Start(void* a0, u32 a1, u32 a2, immut char* a3);
void PspImpl_Jac_StartDemo(u32 a0);
void PspImpl_Jac_StartPartsFindDemo(u32 a0, BOOL a1);
void PspImpl_Jac_StartTextDemo(int a0);
BOOL PspImpl_Jac_StopEventAction(int a0, int a1);
void PspImpl_Jac_StopOrimaSe(s32 a0);
void PspImpl_Jac_StopSe(s32 a0);
void PspImpl_Jac_StopSystemSe(s32 a0);
void PspImpl_Jac_UpdateCamera(SVector_* a0, SVector_* a1);
BOOL PspImpl_Jac_UpdateEventPosition(int a0, SVector_* a1);
}

extern "C" void Jac_AddDVDBuffer(u8* a0, u32 a1)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::audioLog("game audio call: AddDVDBuffer begin size=%u", (unsigned)a1);
	PikminPsp::AudioLock lock;
	PspImpl_Jac_AddDVDBuffer(a0, a1);
	PikminPsp::audioLog("game audio call: AddDVDBuffer complete");
}

extern "C" void Jac_BackDVDBuffer()
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_BackDVDBuffer();
}

extern "C" int Jac_CheckFreeEvents()
{
	if (!PikminPsp::audioReady()) return 0;
	PikminPsp::AudioLock lock;
	return PspImpl_Jac_CheckFreeEvents();
}

extern "C" int Jac_CreateEvent(u32 a0, SVector_* a1)
{
	if (!PikminPsp::audioReady()) return -1;
	PikminPsp::AudioLock lock;
	return PspImpl_Jac_CreateEvent(a0, a1);
}

extern "C" BOOL Jac_DemoFrame(int a0)
{
	if (!PikminPsp::audioReady()) return FALSE;
	PikminPsp::AudioLock lock;
	return PspImpl_Jac_DemoFrame(a0);
}

extern "C" BOOL Jac_DestroyEvent(s32 a0)
{
	if (!PikminPsp::audioReady()) return FALSE;
	PikminPsp::AudioLock lock;
	return PspImpl_Jac_DestroyEvent(a0);
}

extern "C" void Jac_EnterBossMode()
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_EnterBossMode();
}

extern "C" void Jac_ExitBossMode()
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_ExitBossMode();
}

extern "C" void Jac_FinishDemo()
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_FinishDemo();
}

extern "C" void Jac_FinishPartsFindDemo()
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_FinishPartsFindDemo();
}

extern "C" void Jac_FinishTextDemo()
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_FinishTextDemo();
}

extern "C" void Jac_Freeze()
{
	PikminPsp::audioShutdown();
}

extern "C" void Jac_Freeze_Precall()
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_Freeze_Precall();
}

extern "C" void Jac_Gsync()
{
	if (!PikminPsp::audioReady()) return ;
	static u32 callCount;
	bool trace = callCount++ < 4;
	if (trace) PikminPsp::audioLog("game audio call: Gsync begin");
	PikminPsp::AudioLock lock;
	PspImpl_Jac_Gsync();
	if (trace) PikminPsp::audioLog("game audio call: Gsync complete");
}

extern "C" void Jac_InitAllEvent()
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_InitAllEvent();
}

extern "C" void Jac_Orima_Formation(s32 a0, s32 a1)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_Orima_Formation(a0, a1);
}

extern "C" void Jac_Orima_Walk(s32 a0, u32 a1)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_Orima_Walk(a0, a1);
}

extern "C" void Jac_OutputMode(int a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::audioLog("game audio call: OutputMode begin mode=%d", a0);
	PikminPsp::AudioLock lock;
	PspImpl_Jac_OutputMode(a0);
	PikminPsp::audioLog("game audio call: OutputMode complete");
}

extern "C" void Jac_Piki_Number(u32 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_Piki_Number(a0);
}

extern "C" BOOL Jac_PlayEventAction(int a0, int a1)
{
	if (!PikminPsp::audioReady()) return FALSE;
	PikminPsp::AudioLock lock;
	return PspImpl_Jac_PlayEventAction(a0, a1);
}

extern "C" void Jac_PlayOrimaSe(u32 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::audioLog("game audio call: PlayOrimaSe begin id=0x%x", (unsigned)a0);
	PikminPsp::AudioLock lock;
	PspImpl_Jac_PlayOrimaSe(a0);
	PikminPsp::audioLog("game audio call: PlayOrimaSe complete");
}

extern "C" void Jac_PlaySystemSe(s32 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_PlaySystemSe(a0);
}

extern "C" void Jac_SceneExit(u32 a0, u32 a1)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::audioLog("game audio call: SceneExit begin scene=%u stage=%u", (unsigned)a0, (unsigned)a1);
	PikminPsp::AudioLock lock;
	PspImpl_Jac_SceneExit(a0, a1);
	PikminPsp::audioLog("game audio call: SceneExit complete scene=%u", (unsigned)a0);
}

extern "C" void Jac_SceneSetup(u32 a0, u32 a1)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::audioLog("game audio call: SceneSetup begin scene=%u stage=%u", (unsigned)a0, (unsigned)a1);
	PikminPsp::AudioLock lock;
	PspImpl_Jac_SceneSetup(a0, a1);
	PikminPsp::audioLog("game audio call: SceneSetup complete scene=%u", (unsigned)a0);
}

extern "C" void Jac_SetBGMVolume(u8 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::audioLog("game audio call: SetBGMVolume begin value=%u", (unsigned)a0);
	PikminPsp::AudioLock lock;
	PspImpl_Jac_SetBGMVolume(a0);
	PikminPsp::audioLog("game audio call: SetBGMVolume complete");
}

extern "C" void Jac_SetDemoOnyons(int a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_SetDemoOnyons(a0);
}

extern "C" void Jac_SetDemoPartsCount(int a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_SetDemoPartsCount(a0);
}

extern "C" void Jac_SetDemoPartsID(int a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_SetDemoPartsID(a0);
}

extern "C" void Jac_SetSEVolume(u8 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::audioLog("game audio call: SetSEVolume begin value=%u", (unsigned)a0);
	PikminPsp::AudioLock lock;
	PspImpl_Jac_SetSEVolume(a0);
	PikminPsp::audioLog("game audio call: SetSEVolume complete");
}

extern "C" void Jac_Start(void* a0, u32 a1, u32 a2, immut char* a3)
{
	if (!PikminPsp::audioPrepare(a3)) return;
	PikminPsp::audioLock();
	PspImpl_Jac_Start(a0, a1, a2, a3);
	// The original DVD worker completes this after Jac_Start returns.  Publish
	// the boot-sound completion before the game can poll Jac_CheckBootOk().
	PikminPsp::audioDrainCompletions();
	PikminPsp::audioUnlock();
	PikminPsp::audioStart();
}

extern "C" void Jac_StartDemo(u32 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_StartDemo(a0);
}

extern "C" void Jac_StartPartsFindDemo(u32 a0, BOOL a1)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_StartPartsFindDemo(a0, a1);
}

extern "C" void Jac_StartTextDemo(int a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_StartTextDemo(a0);
}

extern "C" BOOL Jac_StopEventAction(int a0, int a1)
{
	if (!PikminPsp::audioReady()) return FALSE;
	PikminPsp::AudioLock lock;
	return PspImpl_Jac_StopEventAction(a0, a1);
}

extern "C" void Jac_StopOrimaSe(s32 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_StopOrimaSe(a0);
}

extern "C" void Jac_StopSe(s32 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_StopSe(a0);
}

extern "C" void Jac_StopSystemSe(s32 a0)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_StopSystemSe(a0);
}

extern "C" void Jac_UpdateCamera(SVector_* a0, SVector_* a1)
{
	if (!PikminPsp::audioReady()) return ;
	PikminPsp::AudioLock lock;
	PspImpl_Jac_UpdateCamera(a0, a1);
}

extern "C" BOOL Jac_UpdateEventPosition(int a0, SVector_* a1)
{
	if (!PikminPsp::audioReady()) return FALSE;
	PikminPsp::AudioLock lock;
	return PspImpl_Jac_UpdateEventPosition(a0, a1);
}

extern "C" int Jac_StreamMovieGetPicture(void*, int* width, int* height)
{
	if (width) *width = 0;
	if (height) *height = 0;
	return 0;
}
extern "C" void Jac_StreamMovieStop() {}
