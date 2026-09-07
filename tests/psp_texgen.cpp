#include "Dolphin/mtx.h"
#include <assert.h>
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// Capture the matrix upload; use the production texgen block and PSP VECMag.
static Mtx uploaded;
static int uploads;
static const int GX_MTX2x4 = 1;
static void GXLoadTexMtxImm(const Mtx matrix, int id, int type)
{
	assert(id == 36 && type == GX_MTX2x4);
	memcpy(uploaded, matrix, sizeof(float) * 8);
	++uploads;
}
struct Matrix4f { Mtx mMtx; };

__attribute__((noinline)) static void generate(const Matrix4f& mtx, bool mHasTexGen = true)
{
	int gxID = 6;
	int mTexMtxBaseID = 30;
#include "texgen_block.inc"
}

int main()
{
	Matrix4f matrix = {};
	feclearexcept(FE_ALL_EXCEPT);
	generate(matrix);
	assert(!(fetestexcept(FE_DIVBYZERO | FE_INVALID)));
	for (int row = 0; row < 2; ++row) {
		for (int column = 0; column < 3; ++column) {
			assert(isfinite(uploaded[row][column]) && uploaded[row][column] == 0.0f);
		}
		assert(uploaded[row][3] == 0.5f);
	}

	// Ordinary, mirrored, and very small scales keep their normalized mapping.
	const float scales[] = {1.0f, 2.0f, -2.0f, 0.0001f};
	for (float scale : scales) {
		memset(&matrix, 0, sizeof(matrix));
		matrix.mMtx[0][1] = -scale;
		matrix.mMtx[1][0] = scale;
		matrix.mMtx[2][2] = scale;
		generate(matrix);
		float sign = scale > 0.0f ? 1.0f : -1.0f;
		assert(fabsf(uploaded[0][1] + sign * 0.5f) < 0.00001f);
		assert(fabsf(uploaded[1][0] - sign * 0.5f) < 0.00001f);
		assert(uploaded[0][3] == 0.5f && uploaded[1][3] == 0.5f);
	}
	assert(uploads == 5);
	generate(matrix, false);
	assert(uploads == 5);
	puts("PSP zero-scale and regular texture-matrix checks passed");
}
