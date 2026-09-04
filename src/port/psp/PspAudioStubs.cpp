#include "jaudio/app_inter.h"
#include "jaudio/interface.h"
#include "jaudio/piki_bgm.h"
#include "jaudio/piki_player.h"
#include "jaudio/piki_scene.h"
#include "jaudio/pikidemo.h"
#include "jaudio/pikiinter.h"
#include "jaudio/verysimple.h"

// The first PSP milestone intentionally runs silently.  These entry points
// keep game state and menus moving without starting the GameCube DSP/AI stack.
extern "C" {

void Jac_AddDVDBuffer(u8*, u32) { }
void Jac_BackDVDBuffer() { }
int Jac_CheckFreeEvents(void) { return 64; }
int Jac_CreateEvent(u32, SVector_*) { return 0; }
BOOL Jac_DemoFrame(int) { return TRUE; }
BOOL Jac_DestroyEvent(s32) { return TRUE; }
void Jac_EnterBossMode(void) { }
void Jac_ExitBossMode(void) { }
void Jac_FinishDemo(void) { }
void Jac_FinishPartsFindDemo(void) { }
void Jac_FinishTextDemo(void) { }
void Jac_Freeze(void) { }
void Jac_Freeze_Precall(void) { }
void Jac_Gsync(void) { }
void Jac_InitAllEvent(void) { }
void Jac_Orima_Formation(s32, s32) { }
void Jac_Orima_Walk(s32, u32) { }
void Jac_OutputMode(int) { }
void Jac_Piki_Number(u32) { }
BOOL Jac_PlayEventAction(int, int) { return TRUE; }
void Jac_PlayOrimaSe(u32) { }
void Jac_PlaySystemSe(s32) { }
void Jac_SceneExit(u32, u32) { }
void Jac_SceneSetup(u32, u32) { }
void Jac_SetBGMVolume(u8) { }
void Jac_SetDemoOnyons(int) { }
void Jac_SetDemoPartsCount(int) { }
void Jac_SetDemoPartsID(int) { }
void Jac_SetSEVolume(u8) { }
void Jac_Start(void*, u32, u32, immut char*) { }
void Jac_StartDemo(u32) { }
void Jac_StartPartsFindDemo(u32, BOOL) { }
void Jac_StartTextDemo(int) { }
BOOL Jac_StopEventAction(int, int) { return TRUE; }
void Jac_StopOrimaSe(s32) { }
void Jac_StopSe(s32) { }
void Jac_StopSystemSe(s32) { }
int Jac_StreamMovieGetPicture(void*, int* width, int* height)
{
	if (width) *width = 0;
	if (height) *height = 0;
	return 0;
}
void Jac_StreamMovieStop(void) { }
void Jac_UpdateCamera(SVector_*, SVector_*) { }
BOOL Jac_UpdateEventPosition(int, SVector_*) { return TRUE; }

} // extern "C"
