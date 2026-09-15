#include <math.h>
namespace std { using ::sqrtf; }
#include "KMath.h"
#include "Matrix4f.h"
#include <assert.h>
#include <fenv.h>
#include <stdio.h>

#include "impulse.inc"
#include "mult_matrix.inc"

static void check(const Vector3f& position, const Vector3f& before, const Vector3f& after, float expected)
{
	Matrix4f inertia;
	for (int row = 0; row < 4; ++row) {
		for (int col = 0; col < 4; ++col) {
			inertia.mMtx[row][col] = row == col ? 1.0f : 0.0f;
		}
	}
	feclearexcept(FE_ALL_EXCEPT);
	float impulse = calcImpulse(position, 2.0f, Vector3f(0.0f, 1.0f, 0.0f), inertia, before, after);
	assert((fetestexcept(FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW)) == 0);
	assert(isfinite(impulse));
	assert(fabsf(impulse - expected) < 0.00001f);
}

int main()
{
	Vector3f origin(0.0f, 0.0f, 0.0f);
	check(origin, origin, origin, 0.0f); // Resting contact: previously 0/0.
	check(origin, Vector3f(3.0f, 0.0f, 4.0f), Vector3f(3.0f, 0.0f, 4.0f), 0.0f);
	check(origin, Vector3f(0.0f, -10.0f, 0.0f), Vector3f(0.0f, 4.0f, 0.0f), 28.0f);
	check(Vector3f(1.0f, 0.0f, 0.0f), Vector3f(0.0f, -10.0f, 0.0f), Vector3f(0.0f, 4.0f, 0.0f), 14.0f / 1.5f);
	check(origin, origin, Vector3f(0.0f, 2.0f, 0.0f), 4.0f);
	check(origin, Vector3f(0.0f, 2.0f, 0.0f), origin, -4.0f);
	puts("PSP impulse checks passed");
}
