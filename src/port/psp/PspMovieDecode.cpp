#include "port/psp/PspMovieDecode.h"
#include "port/psp/PspAudioDecode.h"
#include <stdlib.h>
#include <string.h>

namespace {
u16 be16(const u8* p) { return u16(p[0]) << 8 | p[1]; }
u32 be32(const u8* p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | p[3]; }
bool validFrame(const u8* p, u32 size, bool intra)
{
	u32 header = intra ? 0x48 : 0x4c;
	if (size < header || p[0] > 7 || p[1] > 15) return false;
	if (!intra && (p[2] > 8 || p[3] > 8 || p[4] > 8 || p[5] > 8)) return false;
	for (u32 field = 8; field < header; field += 4) {
		u32 offset = be32(p + field);
		if (offset > size - header || size - header - offset < 4) return false;
		u32 length = be32(p + header + offset);
		if (length > size - header - offset - 4) return false;
	}
	return true;
}
int clip(int n) { return n < 0 ? 0 : n > 255 ? 255 : n; }

struct MovieColourTable {
	int luma[256], red[256], greenU[256], greenV[256], blue[256];
	constexpr MovieColourTable() : luma{}, red{}, greenU{}, greenV{}, blue{} {
		for (int i = 0; i < 256; ++i) {
			luma[i] = 298 * (i - 16) + 128;
			red[i] = 409 * (i - 128);
			greenU[i] = -100 * (i - 128);
			greenV[i] = -208 * (i - 128);
			blue[i] = 516 * (i - 128);
		}
	}
};
constexpr MovieColourTable movieColours;
}

namespace PikminPsp {
int MovieFile::fail(const char* reason) { failure = reason; return -1; }
bool MovieFile::read(void* dst, u32 size)
{
	if (!file || size > fileSize - position || fread(dst, 1, size, file) != size) return false;
	position += size;
	return true;
}
void MovieFile::close()
{
	if (file) fclose(file);
	file = nullptr;
	free(sequence.ws); sequence = {};
	for (u8*& frame : frames) { free(frame); frame = nullptr; }
	free(packet); packet = nullptr;
	packetCapacity = fileSize = position = blockEnd = blocks = decoded = reference = 0;
	totalFrames = totalBlocks = blockVideos = blockAudio = blockBase = 0;
}
bool MovieFile::open(const char* path)
{
	close(); failure = nullptr;
	file = fopen(path, "rb");
	if (!file) { fail("cannot open movie"); return false; }
	if (fseek(file, 0, SEEK_END) || ftell(file) < 68 || ftell(file) > 0x7fffffffL) {
		fail("invalid movie length"); close(); return false;
	}
	fileSize = u32(ftell(file));
	rewind(file);
	u8 h[68];
	if (!read(h, sizeof(h)) || memcmp(h, "HVQM4 1.3", 9) || be32(h + 16) != sizeof(h)
	    || be32(h + 20) > fileSize - sizeof(h) || fileSize - sizeof(h) - be32(h + 20) >= 32) {
		fail("invalid H4M 1.3 header"); close(); return false;
	}
	fileSize = sizeof(h) + be32(h + 20); // Disc files include padding to 32 bytes.
	totalBlocks = be32(h + 24); totalFrames = be32(h + 28); period = be32(h + 36);
	format = h[60]; sampleRate = be32(h + 64);
	VideoInfo info = {be16(h + 52), be16(h + 54), h[56], h[57], {0, 0}};
	if (!totalBlocks || !totalFrames || totalBlocks > totalFrames || totalFrames > 1000000
	    || period < 10000 || period > 1000000 || !info.width || !info.height
	    || info.width > 640 || info.height > 480 || info.width % 8 || info.height % 8
	    || info.h_sampling_rate != 2 || info.v_sampling_rate != 2
	    || (format != 4 && format != 5) || sampleRate < 8000 || sampleRate > 48000) {
		fail("unsupported H4M parameters"); close(); return false;
	}
	HVQM4InitDecoder(); HVQM4InitSeqObj(&sequence, &info);
	void* work = calloc(1, HVQM4BuffSize(&sequence));
	u32 pictureSize = u32(info.width) * info.height * 3 / 2;
	frames[0] = (u8*)malloc(pictureSize); frames[1] = (u8*)malloc(pictureSize);
	packetCapacity = 384 * 1024;
	packet = (u8*)malloc(packetCapacity + 64);
	if (!work || !frames[0] || !frames[1] || !packet) {
		free(work); fail("movie allocation failed"); close(); return false;
	}
	HVQM4SetBuffer(&sequence, work);
	blockEnd = position;
	return true;
}
int MovieFile::nextFrame(MovieAudioSink sink, void* context)
{
	if (failure || !file) return -1;
	while (true) {
		if (position == blockEnd) {
			if (blockVideos || blockAudio) return fail("incomplete H4M block");
			if (blocks == totalBlocks) {
				if (decoded != totalFrames || position != fileSize) return fail("incomplete H4M movie");
				return 0;
			}
			u8 h[20];
			if (!read(h, sizeof(h))) return fail("truncated H4M block");
			u32 bytes = be32(h + 4);
			blockVideos = be32(h + 8); blockAudio = be32(h + 12);
			if (bytes > fileSize - position || !blockVideos || blockVideos > totalFrames - decoded || blockAudio > 1024)
				return fail("invalid H4M block");
			blockEnd = position + bytes; blockBase = decoded; ++blocks;
		}
		u8 h[8];
		if (blockEnd - position < sizeof(h) || !read(h, sizeof(h))) return fail("truncated H4M record");
		u32 kind = be16(h), flags = be16(h + 2), size = be32(h + 4);
		if (size > blockEnd - position || size > packetCapacity || !read(packet, size)) return fail("invalid H4M record length");
		memset(packet + size, 0, 64);
		if (kind == 0) {
			if (!blockAudio || flags != 0xff00) return fail("invalid H4M audio record");
			--blockAudio;
			if (sink && !sink(packet, size, context)) return fail("movie audio buffer full");
			continue;
		}
		if (kind != 1 || !blockVideos || size < 4 || (flags != 0x10 && flags != 0x20)
		    || be32(packet) != decoded - blockBase || (!decoded && flags != 0x10)
		    || !validFrame(packet + 4, size - 4, flags == 0x10)) return fail("invalid H4M video record");
		u32 target = reference ^ 1;
		if (flags == 0x10) HVQM4DecodeIpic(&sequence, packet + 4, frames[target]);
		else HVQM4DecodePpic(&sequence, packet + 4, frames[target], frames[reference]);
		reference = target; ++decoded; --blockVideos;
		return 1;
	}
}

bool MovieAudio::open(u32 format, u32 rate)
{
	close();
	if ((format != 4 && format != 5) || rate < 8000 || rate > 48000) return false;
	data = (u8*)malloc(Capacity);
	channels = format == 5 ? 4 : 2;
	step = u32((u64(rate) << 16) / 32000);
	return data != nullptr;
}
void MovieAudio::close()
{
	free(data); data = nullptr; playing = false;
	cursor = used = fraction = 0; sample = 16; samples = 0;
	memset(history1, 0, sizeof(history1)); memset(history2, 0, sizeof(history2));
	memset(previous, 0, sizeof(previous));
}
bool MovieAudio::append(const u8* source, u32 size)
{
	if (!data || size > Capacity - used) return false;
	u32 end = (cursor + used) % Capacity;
	u32 first = size < Capacity - end ? size : Capacity - end;
	memcpy(data + end, source, first); memcpy(data, source + first, size - first); used += size;
	return true;
}
bool MovieAudio::nextSample()
{
	if (sample == 16) {
		if (used < channels * 9) return false;
		for (u32 c = 0; c < channels; ++c) {
			u8 afc[9];
			for (u8& b : afc) { b = data[cursor]; cursor = (cursor + 1) % Capacity; }
			audioDecodeAfc(afc, block[c], history1[c], history2[c]);
		}
		used -= channels * 9; sample = 0;
	}
	for (u32 c = 0; c < 2; ++c) {
		s32 v = block[c][sample];
		if (channels == 4) v += block[c + 2][sample];
		previous[c] = audioClamp(v / 2);
	}
	++sample;
	return true;
}
void MovieAudio::mix(s16* output, u32 frames)
{
	if (!data || !playing) return;
	for (u32 i = 0; i < frames; ++i) {
		if (!samples && !fraction) fraction = 65536;
		while (fraction >= 65536) {
			if (!nextSample()) return; // Preserve the movie clock during underruns.
			fraction -= 65536;
		}
		output[i * 2] = audioClamp(s32(output[i * 2]) + previous[0]);
		output[i * 2 + 1] = audioClamp(s32(output[i * 2 + 1]) + previous[1]);
		fraction += step; ++samples;
	}
}
void movieConvertRgb565(const u8* yuv, u32 width, u32 height, u16* output, u32 outWidth, u32 outHeight, u32 stride)
{
	if (!outWidth || !outHeight) return;
	const u8* u = yuv + width * height;
	const u8* v = u + width * height / 4;
	// Playback uses 360 columns. Hoist scaling divisions out of the pixel
	// loop, with a bounded stack allocation and a generic wide-output path.
	u32 sourceX[640];
	if (outWidth <= 640)
		for (u32 x = 0; x < outWidth; ++x) sourceX[x] = x * width / outWidth;
	for (u32 y = 0; y < outHeight; ++y) {
		u32 sy = y * height / outHeight;
		const u8* rowY = yuv + sy * width;
		const u8* rowU = u + (sy / 2) * (width / 2);
		const u8* rowV = v + (sy / 2) * (width / 2);
		u16* rowOut = output + y * stride;
		for (u32 x = 0; x < outWidth; ++x) {
			u32 sx = outWidth <= 640 ? sourceX[x] : x * width / outWidth;
			int luma = movieColours.luma[rowY[sx]];
			u8 cb = rowU[sx / 2], cr = rowV[sx / 2];
			int r = clip((luma + movieColours.red[cr]) >> 8);
			int g = clip((luma + movieColours.greenU[cb] + movieColours.greenV[cr]) >> 8);
			int b = clip((luma + movieColours.blue[cb]) >> 8);
			rowOut[x] = u16((r >> 3) | (g >> 2) << 5 | (b >> 3) << 11);
		}
	}
}
}
