#include "port/psp/PspAudio.h"
#include "port/psp/PspAudioDecode.h"
#include "port/psp/PspPlatform.h"
#include "jaudio/syncstream.h"
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
extern "C" {
#include <me-core-mapper/me-lib.h>
}

namespace {
struct Stream {
	u8 input[16384];
	u32 cursor, available, remainingBytes;
	u32 sampleCount, sampleRate, frameRate, format, id, position, fraction, step;
	u32 decodedPosition;
	s32 history1[4], history2[4];
	s16 decoded[4][16];
	s16 previous[2];
	s32 volume[2], mixLevel[2];
	bool ready, playing, paused, finished;
};
Stream sStream __attribute__((aligned(64)));
SceUID sFile = -1;
StreamCallback sCallback;

void cacheRange(uintptr_t& start, u32& size)
{
	uintptr_t end = (start + size + 63) & ~uintptr_t(63);
	start &= ~uintptr_t(63);
	size = u32(end - start);
}

u32 be32(const u8* b) { return (u32(b[0]) << 24) | (u32(b[1]) << 16) | (u32(b[2]) << 8) | b[3]; }
u16 be16(const u8* b) { return (u16(b[0]) << 8) | b[1]; }

void closeStream()
{
	sStream.playing = sStream.ready = false;
	sStream.finished = true;
	if (sFile >= 0) sceIoClose(sFile);
	sFile = -1;
	if (sCallback) sCallback(sStream.id, -1);
}

bool nextSample(s16* pair)
{
	using namespace PikminPsp;
	if (sStream.position >= sStream.sampleCount) { sStream.finished = true; return false; }
	if (sStream.format == AUDIOFRMT_ADPCM || sStream.format == AUDIOFRMT_ADPCM4X) {
		unsigned channels = sStream.format == AUDIOFRMT_ADPCM4X ? 4 : 2;
		if (sStream.decodedPosition >= 16) {
			if (sStream.available - sStream.cursor < channels * 9) return false;
			for (unsigned c = 0; c < channels; ++c) {
				audioDecodeAfc(sStream.input + sStream.cursor, sStream.decoded[c], sStream.history1[c], sStream.history2[c]);
				sStream.cursor += 9;
			}
			sStream.decodedPosition = 0;
		}
		unsigned p = sStream.decodedPosition++;
		for (unsigned c = 0; c < 2; ++c) {
			s32 v = sStream.decoded[c][p];
			if (channels == 4) v = ((v * sStream.mixLevel[0]) >> 15) + ((sStream.decoded[c + 2][p] * sStream.mixLevel[1]) >> 15);
			pair[c] = audioClamp(v);
		}
	} else {
		unsigned bytes = sStream.format == AUDIOFRMT_16BIT_PCM ? 4 : 2;
		if (sStream.available - sStream.cursor < bytes) return false;
		const u8* p = sStream.input + sStream.cursor;
		pair[0] = bytes == 4 ? s16(be16(p)) : s16(s8(p[0])) * 256;
		pair[1] = bytes == 4 ? s16(be16(p + 2)) : s16(s8(p[1])) * 256;
		sStream.cursor += bytes;
	}
	++sStream.position;
	return true;
}
}

namespace PikminPsp {
void audioStreamCpuPublish()
{
	uintptr_t start = (uintptr_t)&sStream; u32 size = sizeof(sStream); cacheRange(start, size);
	sceKernelDcacheWritebackRange((void*)start, size);
}

void audioStreamCpuAcquire()
{
	uintptr_t start = (uintptr_t)&sStream; u32 size = sizeof(sStream); cacheRange(start, size);
	sceKernelDcacheInvalidateRange((void*)start, size);
}

void audioStreamMeAcquire()
{
	uintptr_t start = (uintptr_t)&sStream; u32 size = sizeof(sStream); cacheRange(start, size);
	meLibDcacheInvalidateRange((u32)start, size);
}

void audioStreamMePublish()
{
	uintptr_t start = (uintptr_t)&sStream; u32 size = sizeof(sStream); cacheRange(start, size);
	meLibDcacheWritebackRange((u32)start, size);
}

void audioStreamUpdate()
{
	if (sStream.finished && sFile >= 0) { closeStream(); return; }
	if (sFile < 0) return;
	if (sCallback && sCallback(sStream.id, 1) < 0) { closeStream(); return; }
	u32 available = sStream.available - sStream.cursor;
	if (available >= 4096 || !sStream.remainingBytes) return;
	memmove(sStream.input, sStream.input + sStream.cursor, available);
	sStream.cursor = 0;
	sStream.available = available;
	u32 bytes = sizeof(sStream.input) - available;
	if (bytes > sStream.remainingBytes) bytes = sStream.remainingBytes;
	int read = sceIoRead(sFile, sStream.input + available, bytes);
	if (read <= 0) { printf("[audio] Stream read failed\n"); closeStream(); return; }
	sStream.available += read;
	sStream.remainingBytes -= read;
}

void audioStreamMix(s16* output, u32 frames)
{
	if (!sStream.playing || sStream.paused || sStream.finished) return;
	for (u32 i = 0; i < frames; ++i) {
		while (sStream.fraction >= 65536) {
			if (!nextSample(sStream.previous)) return;
			sStream.fraction -= 65536;
		}
		for (unsigned c = 0; c < 2; ++c) {
			s32 value = (sStream.previous[c] * sStream.volume[c]) >> 15;
			output[i * 2 + c] = audioClamp(s32(output[i * 2 + c]) + value);
		}
		sStream.fraction += sStream.step;
	}
}
void audioStreamShutdown() { if (sFile >= 0) closeStream(); }
}

extern "C" {
void Init_StreamAudio() { sFile = -1; memset(&sStream, 0, sizeof(sStream)); }
void RegisterStreamCallback(StreamCallback callback) { sCallback = callback; }
BOOL StreamAudio_Start(u32, int id, const char* name, BOOL, BOOL, StreamHeader_*)
{
	if (sFile >= 0) closeStream();
	memset(&sStream, 0, sizeof(sStream));
	sStream.id = id;
	char path[512], relative[384];
	while (*name == '/') ++name;
	snprintf(relative, sizeof(relative), "data/dataDir/SndData/%s", name);
	if (!PikminPsp::resolvePath(path, sizeof(path), relative)) return FALSE;
	sFile = sceIoOpen(path, PSP_O_RDONLY, 0);
	u8 header[32];
	if (sFile < 0 || sceIoRead(sFile, header, sizeof(header)) != sizeof(header)) {
		printf("[audio] Cannot open stream %s\n", name); closeStream(); return FALSE;
	}
	sStream.remainingBytes = be32(header);
	sStream.sampleCount = be32(header + 4);
	sStream.sampleRate = be16(header + 8);
	sStream.format = be16(header + 10);
	sStream.frameRate = be16(header + 14);
	SceOff size = sceIoLseek(sFile, 0, PSP_SEEK_END);
	sceIoLseek(sFile, 32, PSP_SEEK_SET);
	if (!sStream.sampleCount || sStream.sampleRate < 8000 || sStream.sampleRate > 48000 ||
	    sStream.format < 2 || sStream.format > 5 || size < 32 || sStream.remainingBytes > u32(size - 32)) {
		printf("[audio] Invalid stream header %s\n", name); closeStream(); return FALSE;
	}
	sStream.decodedPosition = 16;
	sStream.volume[0] = sStream.volume[1] = 0x7fff;
	sStream.mixLevel[0] = sStream.mixLevel[1] = 0x7fff;
	sStream.step = (u32)((u64(sStream.sampleRate) << 16) / 32000);
	sStream.fraction = 65536;
	sStream.ready = true;
	PikminPsp::audioStreamUpdate();
	if (sCallback) sCallback(id, 0);
	return sStream.ready;
}
BOOL StreamSyncCheckReady(u32) { return sStream.ready; }
BOOL StreamSyncCheckReadyID(u32, u32 id) { return sStream.ready && sStream.id == id; }
BOOL StreamSyncCheckBusy(u32, u32 id) { return sFile >= 0 && !(sStream.ready && sStream.id == id); }
int StreamSyncPlayAudio(f32 pitch, u32, int left, int right)
{
	if (!sStream.ready) return FALSE;
	if (pitch < 0.25f) pitch = 0.25f;
	if (pitch > 4.0f) pitch = 4.0f;
	sStream.step = u32(sStream.sampleRate * pitch * (65536.0f / 32000.0f));
	sStream.volume[0] = left; sStream.volume[1] = right;
	sStream.ready = false; sStream.playing = true;
	return TRUE;
}
BOOL StreamSyncStopAudio(u32) { closeStream(); return TRUE; }
void StreamChgVolume(u32, int left, int right) { sStream.volume[0] = left; sStream.volume[1] = right; }
void StreamChgMixLevel(u32, int music, int effects) { sStream.mixLevel[0] = music; sStream.mixLevel[1] = effects; }
int StreamGetCurrentFrame(u32, u32) { return sStream.sampleRate ? u64(sStream.position) * sStream.frameRate / sStream.sampleRate : -1; }
BOOL StreamSetDVDPause(u32, BOOL pause) { bool previous = sStream.paused; sStream.paused = pause; return previous; }
u8 StreamCheckAudioFormat(u32) { return sStream.format; }
}
