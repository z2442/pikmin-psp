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
	explicit PerformanceScope(PerformanceMetric metric) : mMetric(metric), mStart(sceKernelGetSystemTimeLow()) {}
	~PerformanceScope() { performanceAdd(mMetric, (u32)(sceKernelGetSystemTimeLow() - mStart)); }
private:
	PerformanceMetric mMetric;
	u32 mStart;
};

} // namespace PikminPsp
