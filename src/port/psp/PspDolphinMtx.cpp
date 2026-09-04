#include "Dolphin/mtx.h"

#include <math.h>
#include <string.h>

extern "C" {

void PSMTXIdentity(Mtx matrix)
{
	memset(matrix, 0, sizeof(Mtx));
	matrix[0][0] = matrix[1][1] = matrix[2][2] = 1.0f;
}

void PSMTXConcat(const Mtx a, const Mtx b, Mtx output)
{
	Mtx temporary;
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column) {
			temporary[row][column] = a[row][0] * b[0][column] + a[row][1] * b[1][column] + a[row][2] * b[2][column];
		}
		temporary[row][3] = a[row][0] * b[0][3] + a[row][1] * b[1][3] + a[row][2] * b[2][3] + a[row][3];
	}
	memcpy(output, temporary, sizeof(Mtx));
}

void PSMTXTranspose(const Mtx source, Mtx output)
{
	Mtx temporary;
	for (int row = 0; row < 3; ++row) {
		for (int column = 0; column < 3; ++column) temporary[row][column] = source[column][row];
		temporary[row][3] = 0.0f;
	}
	memcpy(output, temporary, sizeof(Mtx));
}

u32 PSMTXInverse(const Mtx source, Mtx output)
{
	const f32 a00 = source[0][0], a01 = source[0][1], a02 = source[0][2];
	const f32 a10 = source[1][0], a11 = source[1][1], a12 = source[1][2];
	const f32 a20 = source[2][0], a21 = source[2][1], a22 = source[2][2];
	const f32 determinant = a00 * (a11 * a22 - a12 * a21) - a01 * (a10 * a22 - a12 * a20)
	                      + a02 * (a10 * a21 - a11 * a20);
	if (fabsf(determinant) < 1.0e-12f) return 0;
	const f32 reciprocal = 1.0f / determinant;
	Mtx inverse;
	inverse[0][0] = (a11 * a22 - a12 * a21) * reciprocal;
	inverse[0][1] = (a02 * a21 - a01 * a22) * reciprocal;
	inverse[0][2] = (a01 * a12 - a02 * a11) * reciprocal;
	inverse[1][0] = (a12 * a20 - a10 * a22) * reciprocal;
	inverse[1][1] = (a00 * a22 - a02 * a20) * reciprocal;
	inverse[1][2] = (a02 * a10 - a00 * a12) * reciprocal;
	inverse[2][0] = (a10 * a21 - a11 * a20) * reciprocal;
	inverse[2][1] = (a01 * a20 - a00 * a21) * reciprocal;
	inverse[2][2] = (a00 * a11 - a01 * a10) * reciprocal;
	for (int row = 0; row < 3; ++row) {
		inverse[row][3] = -(inverse[row][0] * source[0][3] + inverse[row][1] * source[1][3]
		                  + inverse[row][2] * source[2][3]);
	}
	memcpy(output, inverse, sizeof(Mtx));
	return 1;
}

void MTXTrans(Mtx matrix, f32 x, f32 y, f32 z)
{
	PSMTXIdentity(matrix);
	matrix[0][3] = x;
	matrix[1][3] = y;
	matrix[2][3] = z;
}

void MTXScale(Mtx matrix, f32 x, f32 y, f32 z)
{
	memset(matrix, 0, sizeof(Mtx));
	matrix[0][0] = x;
	matrix[1][1] = y;
	matrix[2][2] = z;
}

void MTXPerspective(Mtx44 matrix, f32 fovY, f32 aspect, f32 nearZ, f32 farZ)
{
	memset(matrix, 0, sizeof(Mtx44));
	const f32 cotangent = 1.0f / tanf(fovY * (3.14159265359f / 360.0f));
	matrix[0][0] = cotangent / aspect;
	matrix[1][1] = cotangent;
	matrix[2][2] = -nearZ / (farZ - nearZ);
	matrix[2][3] = -(farZ * nearZ) / (farZ - nearZ);
	matrix[3][2] = -1.0f;
}

void MTXOrtho(Mtx44 matrix, f32 top, f32 bottom, f32 left, f32 right, f32 nearZ, f32 farZ)
{
	memset(matrix, 0, sizeof(Mtx44));
	matrix[0][0] = 2.0f / (right - left);
	matrix[0][3] = -(right + left) / (right - left);
	matrix[1][1] = 2.0f / (top - bottom);
	matrix[1][3] = -(top + bottom) / (top - bottom);
	matrix[2][2] = -1.0f / (farZ - nearZ);
	matrix[2][3] = -farZ / (farZ - nearZ);
	matrix[3][3] = 1.0f;
}

void MTXLightPerspective(Mtx matrix, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT, f32 transS, f32 transT)
{
	memset(matrix, 0, sizeof(Mtx));
	const f32 cotangent = 1.0f / tanf(fovY * (3.14159265359f / 360.0f));
	matrix[0][0] = scaleS * cotangent / aspect;
	matrix[0][2] = -transS;
	matrix[1][1] = scaleT * cotangent;
	matrix[1][2] = -transT;
	matrix[2][2] = -1.0f;
}

f32 VECMag(const Vec* vector)
{
	if (!vector) return 0.0f;
	return sqrtf(vector->x * vector->x + vector->y * vector->y + vector->z * vector->z);
}

} // extern "C"
