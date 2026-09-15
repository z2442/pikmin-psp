#include "port/psp/PspAudio.h"
#include "port/psp/PspAudioMixerJob.h"
#include "port/psp/PspAudioDecode.h"
#include "port/psp/PspPlatform.h"
#include "port/psp/PspPerformance.h"
#include "jaudio/dspinterface.h"
#include "jaudio/aictrl.h"
#include <stdio.h>
#include <string.h>
extern "C" {
#include <me-core-mapper/me-core.h>
}

namespace {
enum { Booting, Idle, Mix, Stop, Halted, Fault };
u32 sMailbox[16] __attribute__((aligned(64), section(".uncached")));
#define MAIL ((volatile u32*)((uintptr_t)sMailbox | UNCACHED_USER_MASK))
bool sMeAvailable, sMeFailed;
bool sLoggedActiveVoices, sLoggedNonzeroPcm;
u32 sMasterGain = 4096;

struct Voice {
	u32 position, fraction, decodedBlock;
	s32 history1, history2;
	s16 decoded[16];
	s16 previous;
};
Voice sVoices[64] __attribute__((aligned(64)));
// Main CPU publishes a snapshot; only completion flags and volume ramp state
// come back. The ME never calls game, OS, file, or allocator functions.
PikminPsp::AudioMixerJob sJob __attribute__((aligned(64)));

void meInvalidateRange(const void* address, u32 size)
{
	if (!address || !size) return;
	uintptr_t start = (uintptr_t)address & ~uintptr_t(63);
	uintptr_t end = ((uintptr_t)address + size + 63) & ~uintptr_t(63);
	meLibDcacheInvalidateRange((u32)start, (u32)(end - start));
}

void meWritebackRange(const void* address, u32 size)
{
	if (!address || !size) return;
	uintptr_t start = (uintptr_t)address & ~uintptr_t(63);
	uintptr_t end = ((uintptr_t)address + size + 63) & ~uintptr_t(63);
	meLibDcacheWritebackRange((u32)start, (u32)(end - start));
}

__attribute__((noinline, aligned(4))) int initialiseMeWithoutSuspendHook()
{
	// This function runs through kcall_2, just like libme-core's private
	// meLibInit. Direct hardware access here therefore executes in kernel mode.
	int table = meCoreGetTableIdFromWitnessWord();
	if (table < 2) return -6;
	meCoreSelectSystemTable(table);
	meLibReset();
	return table;
}

inline void meIdlePollDelay()
{
	// Do not hammer the shared RAM bus while waiting for a 2.5 ms audio tick.
	// This bounded register-only loop adds a few microseconds of pickup latency
	// without requiring an interrupt/wakeup path on the bare-metal ME.
	for (unsigned i = 0; i < 256; ++i) asm volatile("nop");
}

s16 voiceSample(PikminPsp::AudioMixerChannel& c, Voice& v, bool onMe)
{
	using namespace PikminPsp;
	const u32 end = c.loopStartPosition;
	if (v.position >= end) {
		if (!c.isLooping || c.loopAddress >= end) {
			c.done = 1; return 0;
		}
		v.position = c.loopAddress;
		v.history1 = c.loopYN1; v.history2 = c.loopYN2;
		v.decodedBlock = ~0U;
	}
	const u8* data = (const u8*)(uintptr_t)c.baseAddress;
	if (!data) { c.done = 1; return 0; }
	switch (c.samplesSourceType) {
	case 9:
	case 5: {
		u32 block = v.position / 16;
		if (v.decodedBlock != block) {
			const u8* source = data + block * c.samplesSourceType;
			if (onMe) meInvalidateRange(source, c.samplesSourceType);
			audioDecodeAfc(source, v.decoded, v.history1, v.history2, c.samplesSourceType == 5);
			v.decodedBlock = block;
		}
		return v.decoded[v.position & 15];
	}
	case 8:
		if (onMe && v.decodedBlock != (0x80000000U | v.position / 64)) {
			v.decodedBlock = 0x80000000U | v.position / 64;
			meInvalidateRange(data + (v.position & ~63U), 64);
		}
		return s16(s8(data[v.position])) * 256;
	case 16: {
		u32 bytePosition = v.position * 2;
		if (onMe && v.decodedBlock != (0xc0000000U | bytePosition / 64)) {
			v.decodedBlock = 0xc0000000U | bytePosition / 64;
			meInvalidateRange(data + (bytePosition & ~63U), 64);
		}
		return s16((u16(data[bytePosition]) << 8) | data[bytePosition + 1]);
	}
	default: c.done = 1; return 0;
	}
}

void render()
{
	using namespace PikminPsp;
	const bool onMe = meLibGetCpuId() != 0;
	s32 mix[160] = {};
	for (unsigned index = 0; index < sJob.channelCount; ++index) {
		AudioMixerChannel& c = sJob.channels[index];
		Voice& v = sVoices[sJob.channelIds[index]];
		if (!c.enabled || c.done) continue;
		if (c.resetVpb) {
			v.position = v.fraction = 0;
			v.history1 = v.history2 = v.previous = 0;
			v.decodedBlock = ~0U;
			c.resetVpb = 0;
		}
		if (c.useConstantSample) continue;
		s32 left = 0, right = 0, targetLeft = 0, targetRight = 0;
		for (unsigned bus = 0; bus < 6; ++bus) {
			DSPMixerChannel& m = c.mixChannels[bus];
			// Fold rear speakers into stereo. Reverb sends are handled separately
			// from dry voices, so they must not double their dry amplitude.
			if (m.id == 0x0d00 || m.id == 0x0f40) { left += s16(m.currentVolume); targetLeft += s16(m.targetVolume); }
			if (m.id == 0x0d60 || m.id == 0x0ca0) { right += s16(m.currentVolume); targetRight += s16(m.targetVolume); }
			m.currentVolume = c.endRequested ? 0 : m.targetVolume;
		}
		if (c.endRequested) targetLeft = targetRight = 0;
		for (unsigned i = 0; i < 80; ++i) {
			bool oscillator = c.samplesSourceType < 4;
			s16 sample;
			if (oscillator) {
				if (c.samplesSourceType == 1) sample = s16(v.position);
				else sample = (v.position & 65535) < (c.samplesSourceType == 3 ? 16384U : 32768U) ? 16384 : -16384;
				v.position += c.resamplingRatio / 2;
			} else sample = voiceSample(c, v, onMe);
			if (c.done) break;
			// Linear interpolation with a one-source-sample delay preserves
			// decoder history even when pitch advances several samples at once.
			s32 value = oscillator ? sample : v.previous + ((s32(sample - v.previous) * s32(v.fraction)) >> 12);
			s32 l = left + (targetLeft - left) * s32(i) / 80;
			s32 r = right + (targetRight - right) * s32(i) / 80;
			mix[i * 2] += (value * l) >> 15;
			mix[i * 2 + 1] += (value * r) >> 15;
			if (oscillator) continue;
			v.fraction += c.resamplingRatio;
			while (v.fraction >= 4096 && !c.done) {
				v.previous = voiceSample(c, v, onMe);
				++v.position;
				v.fraction -= 4096;
			}
		}
		if (c.endRequested) c.done = 1;
	}
	for (unsigned i = 0; i < 160; ++i) sJob.output[i] = audioClamp(s32((s64(mix[i]) * sJob.gain) >> 12));
	if (onMe) audioStreamMeAcquire();
	audioStreamMix(sJob.output, 80);
	if (onMe) audioStreamMePublish();
	if (sJob.mono) {
		for (unsigned i = 0; i < 80; ++i) {
			s16 mono = (s32(sJob.output[i * 2]) + sJob.output[i * 2 + 1]) / 2;
			sJob.output[i * 2] = sJob.output[i * 2 + 1] = mono;
		}
	}
}
}

extern "C" void meLibOnException() { MAIL[0] = Fault; meLibSync(); meLibHalt(); }
extern "C" void meLibOnExternalInterrupt() { MAIL[0] = Fault; meLibSync(); meLibHalt(); }
extern "C" void meLibOnProcess()
{
	MAIL[1] = 1;
	meLibSync();
	while (MAIL[0] == Booting) { meLibDelayPipeline(); }
	MAIL[1] = 2;
	meLibSync();
	while (MAIL[0] != Stop) {
		if (MAIL[0] == Mix) {
			// Allegrex owns the job snapshot. Discard the ME's prior clean copy
			// before consuming it; writing that old copy back here can replace a
			// newly-started voice with the previous all-idle snapshot.
			meInvalidateRange(&sJob, offsetof(PikminPsp::AudioMixerJob, channels));
			const u32 jobBytes = PikminPsp::audioMixerJobBytes(sJob);
			meInvalidateRange(sJob.channels, jobBytes - offsetof(PikminPsp::AudioMixerJob, channels));
			render();
			meWritebackRange(&sJob, jobBytes);
			meLibSync();
			if (MAIL[0] == Mix) MAIL[0] = Idle;
			meLibSync();
		} else { meIdlePollDelay(); }
	}
	MAIL[0] = Halted;
	meLibSync();
	meLibHalt();
}

namespace PikminPsp {
void audioSetMasterGain(f32 gain) { sMasterGain = u32(gain * 4096.0f); }
void audioStopMe()
{
	if (!sMeAvailable) return;
	MAIL[0] = Stop;
	meLibSync();
	u32 start = sceKernelGetSystemTimeLow();
	while (MAIL[0] != Halted && sceKernelGetSystemTimeLow() - start < 250000) sceKernelDelayThread(100);
	sMeAvailable = false;
}

bool audioBootMe()
{
	MAIL[0] = Booting; MAIL[1] = 0;
	meLibSync();
	// libme-core writes and loads its embedded kcall.prx relative to cwd.
	sceIoChdir(executableRoot());
	int result = meLibDefaultInit();
	if (result == -5) {
		/*
		 * -5 means only the optional SceMeReset suspend hook was unavailable;
		 * meLibDefaultInit has already loaded the kernel bridge.  Some CFW
		 * sessions omit that event handler, so run the library's equivalent
		 * core initialization through the bridge and halt it on game shutdown.
		 */
		result = kcall_2(initialiseMeWithoutSuspendHook, 0);
		audioLog("ME suspend hook unavailable; direct initialization result=%d", result);
	}
	if (result >= 0) {
		MAIL[0] = Idle;
		meLibSync();
		u32 start = sceKernelGetSystemTimeLow();
		while (MAIL[1] != 2 && sceKernelGetSystemTimeLow() - start < 250000) sceKernelDelayThread(100);
		sMeAvailable = MAIL[1] == 2;
		if (!sMeAvailable) MAIL[0] = Stop;
	}
	printf("[audio] %s (libme-core %d)\n", sMeAvailable ? "ME decoder ready" : "ME unavailable; using CPU decoder", result);
	audioLog("%s (libme-core result=%d)", sMeAvailable ? "ME decoder ready" : "ME unavailable; CPU decoder active", result);
	audioLog("ME transport: channel=%u max job=%u bytes, active voices only; stream input on refill/reset",
	         (unsigned)sizeof(AudioMixerChannel), (unsigned)sizeof(sJob));
	return sMeAvailable;
}

void audioMix(s16* output, u32 frames)
{
	if (frames != 80 || sMeFailed) { memset(output, 0, frames * 4); return; }
	audioPackMixerJob(sJob, GetDspHandle(0));
	u32 activeVoices = sJob.channelCount;
	if (activeVoices && !sLoggedActiveVoices) {
		audioLog("first active DSP voices=%u", (unsigned)activeVoices);
		sLoggedActiveVoices = true;
	}
	sJob.mono = Jac_GetOutputMode() == 0;
	sJob.gain = sMasterGain;
	if (sMeAvailable) {
		audioStreamCpuPublish();
		sceKernelDcacheWritebackRange(&sJob, audioMixerJobBytes(sJob));
		MAIL[0] = Mix;
		meLibSync();
		u32 start = sceKernelGetSystemTimeLow();
		while (MAIL[0] == Mix && sceKernelGetSystemTimeLow() - start < 250000) sceKernelDelayThread(100);
		performanceAdd(PerformanceAudioMeWait, (u32)(sceKernelGetSystemTimeLow() - start));
		if (MAIL[0] != Idle) {
			// Never race a stalled ME by running the same job on the main CPU.
			// Its private job and arenas remain allocated until process exit.
			sMeFailed = true;
			audioFault();
			MAIL[0] = Stop;
			printf("[audio] ME decoder stalled; audio stopped\n");
			memset(output, 0, frames * 4);
			return;
		}
		sceKernelDcacheInvalidateRange(&sJob, audioMixerJobBytes(sJob));
		audioStreamCpuAcquire();
	} else {
		render();
	}
	audioUnpackMixerJob(GetDspHandle(0), sJob);
	memcpy(output, sJob.output, frames * 4);
	if (!sLoggedNonzeroPcm) {
		u32 peak = 0;
		for (u32 i = 0; i < frames * 2; ++i) {
			u32 magnitude = output[i] < 0 ? u32(-s32(output[i])) : u32(output[i]);
			if (magnitude > peak) peak = magnitude;
		}
		if (peak) {
			audioLog("first nonzero PCM peak=%u", (unsigned)peak);
			sLoggedNonzeroPcm = true;
		}
	}
}
}
