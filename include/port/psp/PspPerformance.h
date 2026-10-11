#pragma once

#include "types.h"
#include <pspkernel.h>

namespace PikminPsp {

enum PerformanceMetric {
	PerformanceRender,
	PerformanceMesh,
	PerformanceTexture,
	PerformancePacing,
	PerformanceAudioWork,
	PerformanceAudioLock,
	PerformanceAudioSequence,
	PerformanceAudioChannels,
	PerformanceAudioStream,
	PerformanceAudioMix,
	PerformanceAudioMeWait,
	PerformanceAudioMovie,
	PerformanceMovieDecode,
	PerformanceMovieConvert,
	PerformanceMoviePublish,
	PerformanceMetricCount
};

// Monotonic microsecond counters. Snapshot differences avoid resetting a
// counter while the audio thread is updating it. Unsigned subtraction wraps.
void performanceAdd(PerformanceMetric metric, u32 microseconds);
u32 performanceCounter(PerformanceMetric metric);
void performanceSetAudioThread(SceUID thread);
u32 performanceThreadMicroseconds(bool audio);

class PerformanceScope {
public:
	// A zero weight disables clock/counter calls; a sampled scope scales its
	// elapsed duration by the sampling interval. Ordinary scopes stay exact.
	explicit PerformanceScope(PerformanceMetric metric, u32 weight = 1)
	    : mMetric(metric), mStart(weight ? sceKernelGetSystemTimeLow() : 0), mWeight(weight) {}
	~PerformanceScope() {
		if (mWeight) performanceAdd(mMetric, (u32)(sceKernelGetSystemTimeLow() - mStart) * mWeight);
	}
private:
	PerformanceMetric mMetric;
	u32 mStart;
	u32 mWeight;
};

} // namespace PikminPsp
