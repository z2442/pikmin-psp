#include "port/psp/PspMovieDecode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

static void referenceRgb565(const u8* yuv, u32 width, u32 height, u16* output, u32 outWidth, u32 outHeight, u32 stride)
{
	const u8* u = yuv + width * height;
	const u8* v = u + width * height / 4;
	auto clip = [](int n) { return n < 0 ? 0 : n > 255 ? 255 : n; };
	for (u32 y = 0; y < outHeight; ++y) for (u32 x = 0; x < outWidth; ++x) {
		u32 sx = x * width / outWidth, sy = y * height / outHeight;
		u32 uv = (sy / 2) * (width / 2) + sx / 2;
		int luma = 298 * (int(yuv[sy * width + sx]) - 16);
		int cb = int(u[uv]) - 128, cr = int(v[uv]) - 128;
		int r = clip((luma + 409 * cr + 128) >> 8);
		int g = clip((luma - 100 * cb - 208 * cr + 128) >> 8);
		int b = clip((luma + 516 * cb + 128) >> 8);
		output[y * stride + x] = u16((r >> 3) | (g >> 2) << 5 | (b >> 3) << 11);
	}
}

static void colourConversion()
{
	// Exercise every chroma pair at luma endpoints and clipping boundaries.
	std::vector<u8> yuv(512 * 512 * 3 / 2);
	for (u32 i = 0; i < 256 * 256; ++i) {
		yuv[512 * 512 + i] = i % 256;
		yuv[512 * 512 * 5 / 4 + i] = i / 256;
	}
	std::vector<u16> expected(512 * 512), actual(expected.size());
	for (u8 luma : {0, 15, 16, 17, 64, 128, 234, 235, 236, 255}) {
		memset(yuv.data(), luma, 512 * 512);
		referenceRgb565(yuv.data(), 512, 512, expected.data(), 512, 512, 512);
		PikminPsp::movieConvertRgb565(yuv.data(), 512, 512, actual.data(), 512, 512, 512);
		assert(actual == expected);
	}
	// Real playback scaling, upscaling, noninteger ratios, the wide-output
	// fallback and row padding. The sentinel checks all untouched storage.
	const u32 sizes[][4] = {{640,480,360,270}, {16,8,3,5}, {8,8,17,13}, {2,2,1,1}, {8,8,641,3}};
	for (const auto& s : sizes) {
		yuv.resize(s[0] * s[1] * 3 / 2);
		for (u32 i = 0; i < yuv.size(); ++i) yuv[i] = (i * 37 + i / 11) & 255;
		u32 stride = s[2] + 7;
		expected.assign(stride * (s[3] + 1), 0x5a5a); actual = expected;
		referenceRgb565(yuv.data(), s[0], s[1], expected.data(), s[2], s[3], stride);
		PikminPsp::movieConvertRgb565(yuv.data(), s[0], s[1], actual.data(), s[2], s[3], stride);
		assert(actual == expected);
	}
	puts("Movie RGB565 reference, chroma, scaling and padding checks passed");
}

static bool audio(const u8* data, u32 size, void* context)
{
	return static_cast<PikminPsp::MovieAudio*>(context)->append(data, size);
}

int main(int argc, char** argv)
{
	colourConversion();
	u8 black[24]; memset(black, 128, sizeof(black)); memset(black, 16, 16);
	u16 rgb[16];
	PikminPsp::movieConvertRgb565(black, 4, 4, rgb, 4, 4, 4);
	for (u16 pixel : rgb) assert(pixel == 0);
	memset(black, 235, 16);
	PikminPsp::movieConvertRgb565(black, 4, 4, rgb, 4, 4, 4);
	for (u16 pixel : rgb) assert(pixel == 0xffff);

	for (int arg = 1; arg < argc; ++arg) {
		PikminPsp::MovieFile movie;
		PikminPsp::MovieAudio pcm;
		if (!movie.open(argv[arg])) { fprintf(stderr, "%s: %s\n", argv[arg], movie.error()); return 1; }
		assert(pcm.open(movie.audioFormat(), movie.audioRate()));
		pcm.playing = true;
		u32 limit = getenv("PIKMIN_MOVIE_TEST_FRAMES") ? atoi(getenv("PIKMIN_MOVIE_TEST_FRAMES")) : 90;
		u32 frames = 0, hash = 2166136261U, peak = 0;
		u16 image[360 * 270];
		while (!limit || frames < limit) {
			int result = movie.nextFrame(audio, &pcm);
			if (result < 0) { fprintf(stderr, "%s frame %u: %s\n", argv[arg], frames, movie.error()); return 1; }
			if (!result) break;
			++frames;
			const u8* yuv = movie.picture();
			for (u32 i = 0; i < movie.width() * movie.height() * 3 / 2; ++i) hash = (hash ^ yuv[i]) * 16777619U;
			u32 count = u64(frames) * movie.framePeriod() * 32000 / 1000000
			          - u64(frames - 1) * movie.framePeriod() * 32000 / 1000000;
			while (count) {
				s16 output[80 * 2] = {};
				u32 n = count < 80 ? count : 80;
				pcm.mix(output, n);
				for (u32 i = 0; i < n * 2; ++i) {
					u32 v = abs(int(output[i])); if (v > peak) peak = v;
				}
				count -= n;
			}
			if (frames == 60) {
				PikminPsp::movieConvertRgb565(yuv, movie.width(), movie.height(), image, 360, 270, 360);
				u16 reference[360 * 270];
				referenceRgb565(yuv, movie.width(), movie.height(), reference, 360, 270, 360);
				assert(!memcmp(image, reference, sizeof(image)));
				if (const char* path = getenv("PIKMIN_MOVIE_TEST_PPM")) {
					FILE* f = fopen(path, "wb"); assert(f);
					fprintf(f, "P6\n360 270\n255\n");
					for (u16 pixel : image) {
						u8 rgb[] = {u8((pixel & 31) * 255 / 31), u8(((pixel >> 5) & 63) * 255 / 63), u8((pixel >> 11) * 255 / 31)};
						assert(fwrite(rgb, 1, 3, f) == 3);
					}
					fclose(f);
				}
			}
		}
		if (!limit) assert(frames == movie.frameCount());
		printf("%s: %u decoded frames, YUV hash %08x, PCM peak %u\n", argv[arg], frames, hash, peak);
		movie.close(); pcm.close();
		assert(movie.nextFrame() == -1);
	}
}
