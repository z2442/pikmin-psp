#pragma once
#include "types.h"

namespace PikminPsp {
inline s16 audioClamp(s32 value)
{
	return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
}

// Nintendo AFC: one predictor/scale byte followed by 16 signed samples.
// HQ uses eight packed nibble bytes; LQ uses four packed two-bit bytes.
inline void audioDecodeAfc(const u8* source, s16* output, s32& history1, s32& history2, bool lowQuality = false)
{
	static const s16 coefficients[16][2] = {
		{0, 0}, {2048, 0}, {0, 2048}, {1024, 1024}, {4096, -2048}, {3584, -1536},
		{3072, -1024}, {4608, -2560}, {4200, -2248}, {4800, -2300}, {5120, -3072},
		{2048, -2048}, {1024, -1024}, {-1024, 1024}, {-1024, 0}, {-2048, 0}
	};
	const s16* c = coefficients[source[0] & 15];
	const s32 scale = 1 << (source[0] >> 4);
	for (unsigned i = 0; i < 16; ++i) {
		s32 sample;
		if (lowQuality) {
			sample = (source[1 + i / 4] >> (6 - 2 * (i & 3))) & 3;
			if (sample >= 2) sample -= 4;
			sample *= 4;
		} else {
			sample = (source[1 + i / 2] >> ((i & 1) ? 0 : 4)) & 15;
			if (sample >= 8) sample -= 16;
		}
		// Products fit in 32 bits; add the residual after the predictor shift
		// so malformed maximum-scale blocks cannot overflow a signed accumulator.
		s32 value = sample * scale + ((history1 * c[0] + history2 * c[1]) >> 11);
		output[i] = audioClamp(value);
		history2 = history1;
		history1 = output[i];
	}
}
}
