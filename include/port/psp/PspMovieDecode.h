#pragma once
#include "hvqm4.h"
#include <stdio.h>

namespace PikminPsp {
typedef bool (*MovieAudioSink)(const u8*, u32, void*);

// Disc H4M demuxer for the I/P movies shipped with GPIE01_01. All on-disc
// integers stay big endian; only the decoder's native metadata is converted.
class MovieFile {
public:
	MovieFile() = default;
	~MovieFile() { close(); }
	bool open(const char* path);
	void close();
	int nextFrame(MovieAudioSink sink = nullptr, void* context = nullptr);
	const u8* picture() const { return frames[reference]; }
	u32 width() const { return sequence.frame_width; }
	u32 height() const { return sequence.frame_height; }
	u32 frameCount() const { return totalFrames; }
	u32 framePeriod() const { return period; }
	u32 audioRate() const { return sampleRate; }
	u32 audioFormat() const { return format; }
	u32 decodedFrames() const { return decoded; }
	const char* error() const { return failure; }

private:
	bool read(void* data, u32 size);
	int fail(const char* reason);
	FILE* file = nullptr;
	SeqObj sequence = {};
	u8* frames[2] = {};
	u8* packet = nullptr;
	u32 packetCapacity = 0, fileSize = 0, position = 0, blockEnd = 0;
	u32 totalBlocks = 0, blocks = 0, blockVideos = 0, blockAudio = 0, blockBase = 0;
	u32 totalFrames = 0, decoded = 0, period = 0, sampleRate = 0, format = 0, reference = 0;
	const char* failure = nullptr;
};

// Compressed movie audio is buffered independently of video decoding. The PSP
// audio thread consumes it at 32 kHz; callers serialize access with AudioLock.
class MovieAudio {
public:
	~MovieAudio() { close(); }
	bool open(u32 format, u32 rate);
	void close();
	bool append(const u8* data, u32 size);
	void mix(s16* output, u32 frames);
	u64 timeMicroseconds() const { return samples * 1000000 / 32000; }
	u32 bufferedBytes() const { return used; }
	bool playing = false;
private:
	static const u32 Capacity = 512 * 1024;
	bool nextSample();
	u8* data = nullptr;
	u32 cursor = 0, used = 0, channels = 0, step = 0, fraction = 0, sample = 16;
	s32 history1[4] = {}, history2[4] = {};
	s16 block[4][16] = {}, previous[2] = {};
	u64 samples = 0;
};

void movieConvertRgb565(const u8* yuv, u32 width, u32 height, u16* output, u32 outWidth, u32 outHeight, u32 stride);
}
