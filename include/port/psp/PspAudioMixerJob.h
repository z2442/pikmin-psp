#pragma once

#include "jaudio/dspinterface.h"
#include <stddef.h>

namespace PikminPsp {

// Only the fields used by the software mixer cross to the ME. The original
// 384-byte DSP block also holds unused GameCube filter/resampler history.
struct AudioMixerChannel {
	u32 baseAddress, loopAddress, loopStartPosition;
	u16 enabled, done, resetVpb, endRequested, useConstantSample;
	u16 samplesSourceType, isLooping, resamplingRatio;
	s16 loopYN1, loopYN2;
	DSPMixerChannel mixChannels[6];
};
static_assert(sizeof(AudioMixerChannel) == 80, "ME channel transport layout");

inline void audioReadMixerChannel(AudioMixerChannel& out, const DSPchannel_& in)
{
	out.baseAddress = in.baseAddress; out.loopAddress = in.loopAddress; out.loopStartPosition = in.loopStartPosition;
	out.enabled = in.enabled; out.done = in.done; out.resetVpb = in.resetVpb;
	out.endRequested = in.endRequested; out.useConstantSample = in.useConstantSample;
	out.samplesSourceType = in.samplesSourceType; out.isLooping = in.isLooping;
	out.resamplingRatio = in.resamplingRatio; out.loopYN1 = in.loopYN1; out.loopYN2 = in.loopYN2;
	for (u32 i = 0; i < 6; ++i) out.mixChannels[i] = in.mixChannels[i];
}

inline void audioWriteMixerChannel(DSPchannel_& out, const AudioMixerChannel& in)
{
	out.done = in.done; out.resetVpb = in.resetVpb;
	for (u32 i = 0; i < 6; ++i) out.mixChannels[i].currentVolume = in.mixChannels[i].currentVolume;
}

// Publish only voices that render() can change. Keep hardware channel IDs so
// packing/reordering never moves the ME's persistent decoder history.
struct alignas(64) AudioMixerJob {
	s16 output[80 * 2];
	u32 mono, gain, channelCount;
	u8 channelIds[64];
	alignas(64) AudioMixerChannel channels[64];
};

inline void audioPackMixerJob(AudioMixerJob& job, const DSPchannel_* channels)
{
	job.channelCount = 0;
	for (u32 i = 0; i < 64; ++i) {
		if (!channels[i].enabled || channels[i].done) continue;
		u32 slot = job.channelCount++;
		job.channelIds[slot] = i;
		audioReadMixerChannel(job.channels[slot], channels[i]);
	}
}

inline void audioUnpackMixerJob(DSPchannel_* channels, const AudioMixerJob& job)
{
	for (u32 i = 0; i < job.channelCount; ++i)
		audioWriteMixerChannel(channels[job.channelIds[i]], job.channels[i]);
}

inline u32 audioMixerJobBytes(const AudioMixerJob& job)
{
	return (offsetof(AudioMixerJob, channels) + job.channelCount * sizeof(AudioMixerChannel) + 63) & ~63U;
}

} // namespace PikminPsp
