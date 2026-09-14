#include "../src/port/psp/PspAudioStream.cpp"
#include "../src/jaudio/dspinterface.c"
#include "port/psp/PspAudioMixerJob.h"
#include <cassert>
#include <cstdio>

static u32 cpuWriteBytes, cpuReadBytes, meWriteBytes, meReadBytes, channelFlushes;
static void resetCounts() { cpuWriteBytes = cpuReadBytes = meWriteBytes = meReadBytes = channelFlushes = 0; }
extern "C" {
void sceKernelDcacheWritebackRange(const void*, unsigned int bytes) { cpuWriteBytes += bytes; }
void sceKernelDcacheInvalidateRange(const void*, unsigned int bytes) { cpuReadBytes += bytes; }
void meLibDcacheInvalidateRange(u32, u32 bytes) { meReadBytes += bytes; }
void meLibDcacheWritebackRange(u32, u32 bytes) { meWriteBytes += bytes; }
int sceIoRead(SceUID, void* destination, SceSize bytes) { memset(destination, 0x5a, bytes); return bytes; }
int sceIoClose(SceUID) { return 0; }
void DCFlushRange(void*, u32) { ++channelFlushes; }
void DCFlushRangeNoSync(void*, u32) { ++channelFlushes; }
}

int main()
{
	using namespace PikminPsp;
	Init_StreamAudio();
	const u32 stateBytes = sizeof(sStream) - sizeof(sStream.input);
	assert(sizeof(sStream) % 64 == 0 && stateBytes < 512);
	resetCounts();
	audioStreamCpuPublish(); audioStreamMeAcquire(); audioStreamMePublish(); audioStreamCpuAcquire();
	assert(cpuWriteBytes == sizeof(sStream));
	assert(meReadBytes == sizeof(sStream));
	assert(meWriteBytes == stateBytes && cpuReadBytes == stateBytes);
	resetCounts();
	for (u32 i = 0; i < 400; ++i) {
		audioStreamCpuPublish(); audioStreamMeAcquire(); audioStreamMePublish(); audioStreamCpuAcquire();
	}
	assert(cpuWriteBytes == 400 * stateBytes && meReadBytes == 400 * stateBytes);
	assert(meWriteBytes == 400 * stateBytes && cpuReadBytes == 400 * stateBytes);

	// Refill preserves unconsumed bytes and republishes input exactly once.
	sFile = 3; sStream.available = 12; sStream.cursor = 8; sStream.remainingBytes = 64;
	memset(sStream.input + 8, 0x33, 4);
	u32 oldGeneration = sStream.inputGeneration;
	audioStreamUpdate();
	assert(sStream.inputGeneration != oldGeneration);
	assert(sStream.available == 68 && sStream.cursor == 0 && sStream.remainingBytes == 0);
	for (u32 i = 0; i < 68; ++i) assert(sStream.input[i] == (i < 4 ? 0x33 : 0x5a));
	resetCounts();
	audioStreamCpuPublish(); audioStreamMeAcquire();
	assert(cpuWriteBytes == sizeof(sStream) && meReadBytes == sizeof(sStream));
	resetCounts();
	audioStreamCpuPublish(); audioStreamMeAcquire();
	assert(cpuWriteBytes == stateBytes && meReadBytes == stateBytes);
	// Reset must not reuse a generation that the ME has already seen.
	oldGeneration = sStream.inputGeneration;
	Init_StreamAudio(); assert(sStream.inputGeneration != oldGeneration);
	resetCounts(); audioStreamCpuPublish(); audioStreamMeAcquire();
	assert(cpuWriteBytes == sizeof(sStream) && meReadBytes == sizeof(sStream));
	DSPchannel_* channel = GetDspHandle(3);
	channel->enabled = 1; channel->resamplingRatio = 1234;
	DSP_FlushChannel(3); DSP_FlushBuffer();
	assert(channelFlushes == 0 && channel->enabled == 1 && channel->resamplingRatio == 1234);
	// Match every consumed field, then check that ME results change only the
	// same completion/reset/volume fields as the original full-block transport.
	DSPchannel_ original;
	u8* bytes = reinterpret_cast<u8*>(&original);
	for (u32 i = 0; i < sizeof(original); ++i) bytes[i] = (i * 37 + 19) & 255;
	AudioMixerChannel packed;
	audioReadMixerChannel(packed, original);
	assert(packed.baseAddress == original.baseAddress && packed.loopAddress == original.loopAddress);
	assert(packed.loopStartPosition == original.loopStartPosition);
	assert(packed.enabled == original.enabled && packed.done == original.done && packed.resetVpb == original.resetVpb);
	assert(packed.endRequested == original.endRequested && packed.useConstantSample == original.useConstantSample);
	assert(packed.samplesSourceType == original.samplesSourceType && packed.isLooping == original.isLooping);
	assert(packed.resamplingRatio == original.resamplingRatio && packed.loopYN1 == original.loopYN1 && packed.loopYN2 == original.loopYN2);
	assert(!memcmp(packed.mixChannels, original.mixChannels, sizeof(packed.mixChannels)));
	packed.done = 1; packed.resetVpb = 0;
	DSPchannel_ expected = original, returned = original;
	expected.done = 1; expected.resetVpb = 0;
	for (u32 i = 0; i < 6; ++i) {
		packed.mixChannels[i].currentVolume = 100 + i;
		expected.mixChannels[i].currentVolume = 100 + i;
	}
	audioWriteMixerChannel(returned, packed);
	assert(!memcmp(&returned, &expected, sizeof(returned)));
	printf("ME channel snapshot: %u -> %u bytes/channel, return fields preserved\n", (unsigned)sizeof(original), (unsigned)sizeof(packed));
	printf("PSP audio transport: unchanged stream exchange %u -> %u bytes/job per cache operation; refill/reset/channel ownership checks passed\n", (unsigned)sizeof(sStream), (unsigned)stateBytes);
}
