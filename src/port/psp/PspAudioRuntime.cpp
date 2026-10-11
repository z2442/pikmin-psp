#include "port/psp/PspAudio.h"
#include "port/psp/PspPlatform.h"
#include "port/psp/PspPerformance.h"
#include "port/psp/PspMovie.h"
#include "jaudio/aictrl.h"
#include "jaudio/aramcall.h"
#include "jaudio/audiothread.h"
#include "jaudio/dummyrom.h"
#include "jaudio/dvdthread.h"
#include "jaudio/dspinterface.h"
#include "jaudio/dspdriver.h"
#include "jaudio/driverinterface.h"
#include "jaudio/ja_calc.h"
#include "jaudio/playercall.h"
#include "jaudio/rate.h"
#include "jaudio/dspproc.h"
#include "jaudio/dspbuf.h"
#include "jaudio/interleave.h"
#include <pspkernel.h>
#include <pspaudio.h>
#include <pspiofilemgr.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {
SceUID sLock = -1, sThread = -1;
volatile bool sRunning, sReady;
char sRoot[384];
u8* sHeap;
u32 sHeapUsed;
void* sBank;
const u32 HeapSize = 2 * 1024 * 1024;
const u32 WaveArenaSizes[] = {
	4 * 1024 * 1024,
	3 * 1024 * 1024,
	2 * 1024 * 1024,
};
u8* sWaves;
u32 sWaveSize;
u32 sCounter;
s16 sOutput[2][640 * 2] __attribute__((aligned(64)));

struct DvdCompletion {
	Jac_DVDCallback callback;
	u32 owner;
};
const u32 CompletionCount = 128;
DvdCompletion sCompletions[CompletionCount];
u32 sCompletionRead, sCompletionWrite, sCompletionUsed;
u32 sLoggedCompletions;
bool sLoggedOutputSuccess, sLoggedOutputError;

bool queueCompletion(Jac_DVDCallback callback, u32 owner)
{
	if (!callback) return true;
	if (sCompletionUsed == CompletionCount) {
		printf("[audio] DVD completion queue exhausted\n");
		return false;
	}
	sCompletions[sCompletionWrite].callback = callback;
	sCompletions[sCompletionWrite].owner = owner;
	sCompletionWrite = (sCompletionWrite + 1) % CompletionCount;
	++sCompletionUsed;
	return true;
}

bool pathFor(char* path, u32 size, const char* name)
{
	while (*name == '/') ++name;
	u32 rootLength = strlen(sRoot);
	const char* separator = rootLength && sRoot[rootLength - 1] == '/' ? "" : "/";
	int length = snprintf(path, size, "%s%s%s", sRoot, separator, name);
	return length >= 0 && length < (int)size;
}

int audioThread(SceSize, void*)
{
	unsigned buffer = 0;
	while (sRunning) {
		u32 workStart = sceKernelGetSystemTimeLow();
		for (unsigned sub = 0; sub < 8 && sRunning; ++sub) {
			u32 frame = sCounter + 1;
			const u32 timingWeight = PikminPsp::audioTimingWeight(sCounter);
			bool trace = frame >= 9 && frame <= 24;
			if (trace) PikminPsp::audioLog("audio frame %u: lock begin", (unsigned)frame);
			{
				PikminPsp::AudioLock lock;
				++sCounter;
				if (trace) PikminPsp::audioLog("audio frame %u: sequence begin", (unsigned)frame);
				{
					PikminPsp::PerformanceScope scope(PikminPsp::PerformanceAudioSequence, timingWeight);
					DspPlayerCallback();
				}
				if (trace) PikminPsp::audioLog("audio frame %u: channels begin", (unsigned)frame);
				{
					PikminPsp::PerformanceScope scope(PikminPsp::PerformanceAudioChannels, timingWeight);
					UpdateDSPchannelAll();
				if (trace) PikminPsp::audioLog("audio frame %u: callbacks begin", (unsigned)frame);
				PlayerCallback();
				}
				if (trace) PikminPsp::audioLog("audio frame %u: stream begin", (unsigned)frame);
				{
					PikminPsp::PerformanceScope scope(PikminPsp::PerformanceAudioStream, timingWeight);
					PikminPsp::audioStreamUpdate();
				}
				if (trace) PikminPsp::audioLog("audio frame %u: mix begin", (unsigned)frame);
				{
					PikminPsp::PerformanceScope scope(PikminPsp::PerformanceAudioMix, timingWeight);
					PikminPsp::audioMix(sOutput[buffer] + sub * 160, 80);
				}
				{
					PikminPsp::PerformanceScope scope(PikminPsp::PerformanceAudioMovie, timingWeight);
					PikminPsp::movieMix(sOutput[buffer] + sub * 160, 80);
				}
				if (trace) PikminPsp::audioLog("audio frame %u: mix complete", (unsigned)frame);
			}
			if (trace) PikminPsp::audioLog("audio frame %u: lock complete", (unsigned)frame);
		}
		PikminPsp::performanceAdd(PikminPsp::PerformanceAudioWork, (u32)(sceKernelGetSystemTimeLow() - workStart));
		if (sRunning) {
			sceKernelDcacheWritebackRange(sOutput[buffer], sizeof(sOutput[buffer]));
			int result = sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, sOutput[buffer]);
			if (result < 0) {
				if (!sLoggedOutputError) {
					PikminPsp::audioLog("PSP audio output failed result=%d", result);
					sLoggedOutputError = true;
				}
				sceKernelDelayThread(20000);
			} else if (!sLoggedOutputSuccess) {
				PikminPsp::audioLog("first PCM buffer accepted by PSP output");
				sLoggedOutputSuccess = true;
			}
		}
		buffer ^= 1;
	}
	return 0;
}
}

namespace PikminPsp {
bool audioReady() { return sReady; }
void* audioBankData() { return sBank; }
void audioFault() { sReady = sRunning = false; }
void audioLock()
{
	PerformanceScope timing(PerformanceAudioLock);
	if (sLock >= 0) sceKernelWaitSema(sLock, 1, nullptr);
}
void audioUnlock() { if (sLock >= 0) sceKernelSignalSema(sLock, 1); }

void audioLogReset()
{
	char path[512];
	if (!resolvePath(path, sizeof(path), "audio.log")) return;
	SceUID file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
	if (file >= 0) {
		const char header[] = "Pikmin PSP audio diagnostics\n";
		sceIoWrite(file, header, sizeof(header) - 1);
		sceIoClose(file);
	}
}

void audioLog(const char* format, ...)
{
	char line[192], path[512];
	va_list args;
	va_start(args, format);
	int length = vsnprintf(line, sizeof(line) - 1, format, args);
	va_end(args);
	if (length < 0 || !resolvePath(path, sizeof(path), "audio.log")) return;
	if (length >= (int)sizeof(line) - 1) length = sizeof(line) - 2;
	line[length++] = '\n';
	SceUID file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0666);
	if (file >= 0) {
		sceIoWrite(file, line, length);
		sceIoClose(file);
	}
}

void audioDrainCompletions()
{
	// Pop before calling: a completion is allowed to queue the next load.
	while (sCompletionUsed) {
		DvdCompletion completion = sCompletions[sCompletionRead];
		sCompletionRead = (sCompletionRead + 1) % CompletionCount;
		--sCompletionUsed;
		if (sLoggedCompletions < 8) {
			audioLog("DVD completion owner=0x%08x", (unsigned)completion.owner);
			++sLoggedCompletions;
		}
		completion.callback(completion.owner);
	}
}

bool audioPrepare(const char* root)
{
	if (sReady) return false;
	if (!root) root = "/dataDir/SndData/";
	while (*root == '/') ++root;
	char relative[384];
	snprintf(relative, sizeof(relative), "data/%s", root);
	if (!resolvePath(sRoot, sizeof(sRoot), relative)) {
		audioLog("cannot resolve SndData path");
		return false;
	}
	char bankPath[512], sequencePath[512];
	SceIoStat st;
	bool bankPathValid = pathFor(bankPath, sizeof(bankPath), "Banks/pikibank.bx");
	bool sequencePathValid = pathFor(sequencePath, sizeof(sequencePath), "Seqs/pikiseq.arc");
	s32 bankStatus = bankPathValid ? sceIoGetstat(bankPath, &st) : -1;
	s32 sequenceStatus = sequencePathValid ? sceIoGetstat(sequencePath, &st) : -1;
	if (bankStatus < 0 || sequenceStatus < 0) {
		printf("[audio] Missing SndData banks or sequences\n");
		audioLog("required assets: bank=%d %s", (int)bankStatus, bankPathValid ? bankPath : "invalid path");
		audioLog("required assets: sequence=%d %s", (int)sequenceStatus, sequencePathValid ? sequencePath : "invalid path");
		return false;
	}
	audioLog("audio memory before allocation total=%u largest=%u", (unsigned)sceKernelTotalFreeMemSize(),
	         (unsigned)sceKernelMaxFreeMemSize());
	sHeap = (u8*)memalign(64, HeapSize);
	sWaveSize = 0;
	for (u32 i = 0; i < sizeof(WaveArenaSizes) / sizeof(WaveArenaSizes[0]); ++i) {
		sWaves = (u8*)memalign(64, WaveArenaSizes[i]);
		if (sWaves) {
			sWaveSize = WaveArenaSizes[i];
			break;
		}
	}
	sLock = sceKernelCreateSema("Pikmin audio lock", 0, 1, 1, nullptr);
	if (!sHeap || !sWaves || sLock < 0) {
		printf("[audio] Cannot allocate audio memory\n");
		audioLog("audio allocation failed heap=%p waves=%p waveSize=%u lock=%d total=%u largest=%u", sHeap,
		         sWaves, (unsigned)sWaveSize, (int)sLock, (unsigned)sceKernelTotalFreeMemSize(),
		         (unsigned)sceKernelMaxFreeMemSize());
		free(sHeap); free(sWaves); sHeap = sWaves = nullptr;
		if (sLock >= 0) sceKernelDeleteSema(sLock);
		sLock = -1;
		return false;
	}
	sHeapUsed = 0;
	sCompletionRead = sCompletionWrite = sCompletionUsed = sLoggedCompletions = 0;
	sLoggedOutputSuccess = sLoggedOutputError = false;
	memset(sWaves, 0, sWaveSize);
	u32 bankSize = DVDT_CheckFile("Banks/pikibank.bx");
	sBank = OSAlloc2(bankSize);
	if (!sBank || !DVDT_LoadFile("Banks/pikibank.bx", (u8*)sBank)) {
		printf("[audio] Invalid instrument bank\n");
		audioLog("instrument bank load or conversion failed");
		free(sHeap); free(sWaves); sHeap = sWaves = nullptr;
		sceKernelDeleteSema(sLock); sLock = -1;
		return false;
	}
	sReady = true;
	audioLog("audio prepared bank=%u heap=%u wave=%u", (unsigned)bankSize, (unsigned)HeapSize, (unsigned)sWaveSize);
	return true;
}

void audioStart()
{
	if (!sReady || sThread >= 0) return;
	if (sceAudioSRCChReserve(640, 32000, 2) < 0) {
		printf("[audio] Cannot reserve PSP audio output\n");
		audioLog("PSP hardware SRC reservation failed");
		audioFault();
		return;
	}
	sRunning = true;
	sThread = sceKernelCreateThread("Pikmin audio", audioThread, 0x18, 0x10000, PSP_THREAD_ATTR_USER, nullptr);
	performanceSetAudioThread(sThread);
	if (sThread < 0 || sceKernelStartThread(sThread, 0, nullptr) < 0) {
		sRunning = false;
		if (sThread >= 0) sceKernelDeleteThread(sThread);
		sThread = -1;
		sceAudioSRCChRelease();
		printf("[audio] Cannot start audio thread\n");
		audioLog("audio thread creation or start failed");
		audioFault();
	} else {
		audioLog("audio output started: 32000 Hz stereo, 640 frames");
	}
}

void audioShutdown()
{
	sRunning = false;
	if (sThread >= 0) {
		sceKernelWaitThreadEnd(sThread, nullptr);
		sceKernelDeleteThread(sThread);
		sThread = -1;
		sceAudioSRCChRelease();
	}
	AudioLock lock;
	audioStopMe();
	audioStreamShutdown();
	// JAudio stores pointers into these arenas for the lifetime of the program.
	sReady = false;
}
}

extern "C" {
void PPCSync() { asm volatile("sync" ::: "memory"); }
u32 JAC_VFRAME_COUNTER, UNIVERSAL_DACCOUNTER;
void Jac_HeapSetup(void*, s32) {}
void* OSAlloc2(u32 size)
{
	if (size > HeapSize - sHeapUsed) {
		printf("[audio] Audio heap exhausted (%u bytes requested)\n", size);
		PikminPsp::requestQuit();
		return nullptr;
	}
	size = (size + 63) & ~63U;
	void* p = sHeap + sHeapUsed;
	sHeapUsed += size;
	memset(p, 0, size);
	return p;
}
void Jac_SetOutputMode(int mode) { JAC_SYSTEM_OUTPUT_MODE = mode == 0 ? 0 : 1; }
int Jac_GetOutputMode() { return JAC_SYSTEM_OUTPUT_MODE; }
void Jac_SetMixerLevel(f32 channels, f32 gain) { Channel_SetMixerLevel(channels); PikminPsp::audioSetMasterGain(gain); }
void StartAudioThread(void*, s32, u32, u32)
{
	JAC_DAC_RATE = 32000.0f;
	// The GameCube audio thread normally establishes this heap before any
	// scene bank is requested. Its storage is ordinary PSP RAM here so loaded
	// AW data can be decoded directly by the ME.
	PikminPsp::audioLog("audio core: ARAM heap begin");
	Init_AramMotherHeap();
	PikminPsp::audioLog("audio core: player callbacks begin");
	ResetPlayerCallback();
	PikminPsp::audioLog("audio core: sine table begin");
	Jac_InitSinTable();
	PikminPsp::audioLog("audio core: DSP buffers begin");
	DSP_InitBuffer();
	PikminPsp::audioLog("audio core: complete");
}
void StopAudioThread() { sRunning = false; }
void DspSyncCountClear(int) {}
int DspSyncCountCheck() { return JAC_SUBFRAMES; }
}

void* ARAllocFull(u32* size) { *size = sWaveSize; return sWaves; }
void DsetupTable(u32, u32, u32, u32, u32) {}
u32 Jac_GetCurrentSCounter() { return sCounter; }

void DVDT_SetRootPath(const char*) {}
void DVDT_ExtendPath(char* dst, const char* ext) { sprintf(dst, "%s", ext); }
void DVDT_SetBuffer(u8*, u32, u32) {}
s32 DVDT_CloseBuffer(u8*) { return 0; }
s32 Jac_RegisterExtFastOpen(const char*) { return 0; }
s32 DVDT_CheckFile(const char* name)
{
	char path[512]; SceIoStat st;
	return pathFor(path, sizeof(path), name) && sceIoGetstat(path, &st) >= 0 ? st.st_size : 0;
}
s32 DVDT_LoadtoDRAM(u32 owner, const char* name, u32 dst, u32 src, u32 length, u32* status, Jac_DVDCallback callback)
{
	char path[512];
	s32 result = -1;
	if (pathFor(path, sizeof(path), name)) {
		SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
		if (fd >= 0) {
			if (!length) length = DVDT_CheckFile(name);
			if (sceIoLseek32(fd, src, PSP_SEEK_SET) >= 0) result = sceIoRead(fd, (void*)dst, length);
			sceIoClose(fd);
			if (result == (s32)length) {
				// File data is consumed by the ME directly. Publish it now; the
				// decoder invalidates each source cache line before first use.
				uintptr_t start = uintptr_t(dst) & ~uintptr_t(63);
				uintptr_t end = (uintptr_t(dst) + length + 63) & ~uintptr_t(63);
				sceKernelDcacheWritebackRange((void*)start, end - start);
			} else result = -1;
		}
	}
	if (result < 0) printf("[audio] Cannot read %s (%u + %u)\n", name, src, length);
	if (status) *status = result < 0 ? 0xffffffff : 1;
	// GameCube performs the read and callback on its DVD worker.  The PSP read
	// is synchronous, but the callback must stay deferred: scene code updates
	// current_bgm/now_loading only after this function returns.
	if (!queueCompletion(callback, result < 0 ? 0xffffffff : owner)) result = -1;
	return result < 0 ? -1 : 0;
}
s32 DVDT_LoadtoARAM(u32 owner, const char* name, u32 dst, u32 src, u32 length, u32* status, Jac_DVDCallback callback)
{
	return DVDT_LoadtoDRAM(owner, name, dst, src, length, status, callback);
}
s32 DVDT_LoadFile(const char* name, u8* dst)
{
	if (!dst) return 0;
	if (DVDT_LoadtoDRAM(0, name, (u32)dst, 0, 0, nullptr, nullptr) < 0) return 0;
	if (strstr(name, ".bx") && !PikminPsp::audioConvertBank(dst, DVDT_CheckFile(name))) return 0;
	return 1;
}
void DVDT_CheckPass(u32 owner, u32* status, Jac_DVDCallback callback)
{
	if (status) *status = 1;
	queueCompletion(callback, owner);
}
