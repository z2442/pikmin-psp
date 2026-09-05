#pragma once

#include "types.h"

namespace PikminPsp {
bool audioBootMe();
void audioStopMe();
void audioFault();
void audioLogReset();
void audioLog(const char* format, ...);
void* audioBankData();
void audioSetMasterGain(f32 gain);
bool audioPrepare(const char* root);
bool audioReady();
void audioStart();
void audioShutdown();
void audioLock();
void audioUnlock();
void audioDrainCompletions();
struct AudioLock {
	AudioLock() { audioLock(); audioDrainCompletions(); }
	~AudioLock() { audioDrainCompletions(); audioUnlock(); }
};
void audioMix(s16* output, u32 frames);
void audioStreamMix(s16* output, u32 frames);
void audioStreamUpdate();
void audioStreamCpuPublish();
void audioStreamCpuAcquire();
void audioStreamMeAcquire();
void audioStreamMePublish();
void audioStreamShutdown();
bool audioConvertBank(void* data, u32 size);
void audioConvertSequenceHeader(void* data);
s16 audioReadEnvelope(const s16* data);
void audioRegisterSequence(const void* data, u32 size);
}
