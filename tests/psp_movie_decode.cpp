#include "port/psp/PspMovieDecode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool audio(const u8* data, u32 size, void* context)
{
	return static_cast<PikminPsp::MovieAudio*>(context)->append(data, size);
}

int main(int argc, char** argv)
{
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
