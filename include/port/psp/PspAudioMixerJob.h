#pragma once

#include "jaudio/dspinterface.h"

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

} // namespace PikminPsp
