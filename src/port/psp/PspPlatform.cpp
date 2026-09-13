#include "port/psp/PspPlatform.h"
#if defined(PIKMIN_PSP_AUDIO)
#include "port/psp/PspAudio.h"
#endif

#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psppower.h>
#include <psprtc.h>
#if defined(PIKMIN_PSP_GPROF)
#include <pspprof.h>
#endif

#include <stdio.h>
#include <string.h>

namespace {

volatile bool sQuitRequested;
u32 sPreviousButtons;
char sExecutableRoot[384] = ".";

#if defined(PIKMIN_PSP_GPROF)
char sGprofOutputPath[512];

__attribute__((no_instrument_function, no_profile_instrument_function))
void stopProfiler()
{
	gprof_stop(sGprofOutputPath, 1);
}
#endif

int exitCallback(int, int, void*)
{
	sQuitRequested = true;
#if defined(PIKMIN_PSP_GPROF)
	stopProfiler();
#endif
#if defined(PIKMIN_PSP_AUDIO)
	PikminPsp::audioShutdown();
#endif
	sceKernelExitGame();
	return 0;
}

int callbackThread(SceSize, void*)
{
	SceUID callback = sceKernelCreateCallback("Pikmin PSP exit", exitCallback, nullptr);
	if (callback >= 0) {
		sceKernelRegisterExitCallback(callback);
		sceKernelSleepThreadCB();
	}
	return 0;
}

void setupCallbacks()
{
	SceUID thread = sceKernelCreateThread("Pikmin PSP callback", callbackThread, 0x11, 0x1000, 0, nullptr);
	if (thread >= 0) {
		sceKernelStartThread(thread, 0, nullptr);
	}
}

void findExecutableRoot(int argc, char** argv)
{
	if (argc <= 0 || argv == nullptr || argv[0] == nullptr || argv[0][0] == '\0') {
		return;
	}

	snprintf(sExecutableRoot, sizeof(sExecutableRoot), "%s", argv[0]);
	char* slash = strrchr(sExecutableRoot, '/');
	if (slash) {
		*slash = '\0';
	} else {
		snprintf(sExecutableRoot, sizeof(sExecutableRoot), ".");
	}
}

bool pathExists(const char* path)
{
	SceIoStat stat;
	return sceIoGetstat(path, &stat) >= 0;
}

} // namespace

namespace PikminPsp {

bool initialise(int argc, char** argv)
{
	sQuitRequested  = false;
	sPreviousButtons = 0;
	findExecutableRoot(argc, argv);
#if defined(PIKMIN_PSP_GPROF)
	resolvePath(sGprofOutputPath, sizeof(sGprofOutputPath), "gmon.out");
	gprof_start();
#endif

	scePowerSetClockFrequency(333, 333, 166);
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	setupCallbacks();
	return true;
}

void shutdown()
{
#if defined(PIKMIN_PSP_GPROF)
	stopProfiler();
#endif
#if defined(PIKMIN_PSP_AUDIO)
	audioShutdown();
#endif
	sceKernelExitGame();
}

void pollInput(PspInputState& input)
{
	SceCtrlData pad;
	sceCtrlPeekBufferPositive(&pad, 1);

	input.buttons  = pad.Buttons;
	input.pressed  = pad.Buttons & ~sPreviousButtons;
	input.released = sPreviousButtons & ~pad.Buttons;
	sPreviousButtons = pad.Buttons;

	f32 rawX = (f32)pad.Lx - 128.0f;
	f32 rawY = 128.0f - (f32)pad.Ly;
	const f32 deadZone = 18.0f;
	input.stickX = (rawX > -deadZone && rawX < deadZone) ? 0.0f : rawX / 127.0f;
	input.stickY = (rawY > -deadZone && rawY < deadZone) ? 0.0f : rawY / 127.0f;
}

bool shouldQuit()
{
	return sQuitRequested;
}

void requestQuit()
{
	sQuitRequested = true;
}

u64 timeMicroseconds()
{
	u64 tick = 0;
	u32 resolution = sceRtcGetTickResolution();
	sceRtcGetCurrentTick(&tick);
	return resolution ? (tick * 1000000ULL) / resolution : 0;
}

const char* executableRoot()
{
	return sExecutableRoot;
}

bool resolvePath(char* output, u32 outputSize, const char* relativePath)
{
	if (!output || outputSize == 0 || !relativePath) {
		return false;
	}

	while (*relativePath == '/') {
		relativePath++;
	}
	int written = snprintf(output, outputSize, "%s/%s", sExecutableRoot, relativePath);
	return written >= 0 && (u32)written < outputSize;
}

bool dataReady()
{
	char path[512];
	return resolvePath(path, sizeof(path), "data/dataDir") && pathExists(path);
}

} // namespace PikminPsp
