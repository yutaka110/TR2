//#include"Matrix4x4.h"
//#include <cmath>
//#include<numbers>
//Matrix4x4 MakeIdentity4x4()
//{
//	Matrix4x4 result;
//
//	result.m[0][0] = 1; result.m[0][1] = 0; result.m[0][2] = 0; result.m[0][3] = 0;
//	result.m[1][0] = 0;	result.m[1][1] = 1;	result.m[1][2] = 0;	result.m[1][3] = 0;
//	result.m[2][0] = 0;	result.m[2][1] = 0;	result.m[2][2] = 1;	result.m[2][3] = 0;
//	result.m[3][0] = 0;	result.m[3][1] = 0;	result.m[3][2] = 0;	result.m[3][3] = 1;
//
//	return result;
//}
//
//Matrix4x4 Multiply(Matrix4x4 m1, Matrix4x4 m2)
//{
//	Matrix4x4 result;
//
//	float a11 = m1.m[0][0]; float a12 = m1.m[0][1]; float a13 = m1.m[0][2]; float a14 = m1.m[0][3];
//	float a21 = m1.m[1][0]; float a22 = m1.m[1][1]; float a23 = m1.m[1][2]; float a24 = m1.m[1][3];
//	float a31 = m1.m[2][0]; float a32 = m1.m[2][1]; float a33 = m1.m[2][2]; float a34 = m1.m[2][3];
//	float a41 = m1.m[3][0]; float a42 = m1.m[3][1]; float a43 = m1.m[3][2]; float a44 = m1.m[3][3];
//
//	float b11 = m2.m[0][0]; float b12 = m2.m[0][1]; float b13 = m2.m[0][2]; float b14 = m2.m[0][3];
//	float b21 = m2.m[1][0]; float b22 = m2.m[1][1]; float b23 = m2.m[1][2]; float b24 = m2.m[1][3];
//	float b31 = m2.m[2][0]; float b32 = m2.m[2][1]; float b33 = m2.m[2][2]; float b34 = m2.m[2][3];
//	float b41 = m2.m[3][0]; float b42 = m2.m[3][1]; float b43 = m2.m[3][2]; float b44 = m2.m[3][3];
//
//	result.m[0][0] = a11 * b11 + a12 * b21 + a13 * b31 + a14 * b41; result.m[0][1] = a11 * b12 + a12 * b22 + a13 * b32 + a14 * b42; result.m[0][2] = a11 * b13 + a12 * b23 + a13 * b33 + a14 * b43; result.m[0][3] = a11 * b14 + a12 * b24 + a13 * b34 + a14 * b44;
//	result.m[1][0] = a21 * b11 + a22 * b21 + a23 * b31 + a24 * b41; result.m[1][1] = a21 * b12 + a22 * b22 + a23 * b32 + a24 * b42; result.m[1][2] = a21 * b13 + a22 * b23 + a23 * b33 + a24 * b43; result.m[1][3] = a21 * b14 + a22 * b24 + a23 * b34 + a24 * b44;
//	result.m[2][0] = a31 * b11 + a32 * b21 + a33 * b31 + a34 * b41; result.m[2][1] = a31 * b12 + a32 * b22 + a33 * b32 + a34 * b42; result.m[2][2] = a31 * b13 + a32 * b23 + a33 * b33 + a34 * b43; result.m[2][3] = a31 * b14 + a32 * b24 + a33 * b34 + a34 * b44;
//	result.m[3][0] = a41 * b11 + a42 * b21 + a43 * b31 + a44 * b41; result.m[3][1] = a41 * b12 + a42 * b22 + a43 * b32 + a44 * b42; result.m[3][2] = a41 * b13 + a42 * b23 + a43 * b33 + a44 * b43; result.m[3][3] = a41 * b14 + a42 * b24 + a43 * b34 + a44 * b44;
//
//	return result;
//}
//
//Matrix4x4 Transpose(const Matrix4x4& mat) {
//	Matrix4x4 result;
//
//	for (int row = 0; row < 4; ++row) {
//		for (int col = 0; col < 4; ++col) {
//			result.m[row][col] = mat.m[col][row]; // ← 行と列を入れ替える
//		}
//	}
//
//	return result;
//}
//
//Vector3 Normalize(const Vector3& v) {
//	float length = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
//	if (length == 0.0f) {
//		return { 0.0f, 0.0f, 0.0f };
//	}
//	return { v.x / length, v.y / length, v.z / length };
//}
//
////Matrix3x3 MakeScaleMatrix(const Vector2& scale) {
////	Matrix3x3 result{};
////	result.m[0][0] = scale.x;
////	result.m[1][1] = scale.y;
////	result.m[2][2] = 1.0f;
////	return result;
////}
//
//Matrix4x4 MakeScaleMatrix(const Vector3& scale) {
//	Matrix4x4 result = {};
//	result.m[0][0] = scale.x;
//	result.m[1][1] = scale.y;
//	result.m[2][2] = scale.z;
//	result.m[3][3] = 1.0f;
//	return result;
//}
//
//
////Matrix3x3 MakeTranslateMatrix(const Vector2& translate) {
////	Matrix3x3 result{};
////	result.m[0][0] = 1.0f;
////	result.m[1][1] = 1.0f;
////	result.m[2][2] = 1.0f;
////	result.m[2][0] = translate.x;
////	result.m[2][1] = translate.y;
////	return result;
////}
//
//Matrix4x4 MakeTranslateMatrix(const Vector3& translate) {
//	Matrix4x4 result = {};
//	result.m[0][0] = 1.0f;
//	result.m[1][1] = 1.0f;
//	result.m[2][2] = 1.0f;
//	result.m[3][3] = 1.0f;
//	result.m[3][0] = translate.x;
//	result.m[3][1] = translate.y;
//	result.m[3][2] = translate.z;
//	return result;
//}
//
//
//Matrix3x3 MakeRotateZMatrix(float angleRad) {
//	Matrix3x3 result{};
//	float c = cosf(angleRad);
//	float s = sinf(angleRad);
//	result.m[0][0] = c;
//	result.m[0][1] = s;
//	result.m[1][0] = -s;
//	result.m[1][1] = c;
//	result.m[2][2] = 1.0f;
//	return result;
//}
//
//
//Matrix4x4 MakeRoateXMatrix(float radian)
//{
//	Matrix4x4 result = {};
//
//	result.m[0][0] = 1.0f;
//	result.m[1][1] = cosf(radian);
//	result.m[1][2] = sinf(radian);
//	result.m[2][1] = -sinf(radian);
//	result.m[2][2] = cosf(radian);
//	result.m[3][3] = 1.0f;
//
//	return result;
//}
//Matrix4x4 MakeRoateYMatrix(float radian)
//{
//	Matrix4x4 result = {};
//
//	result.m[0][0] = std::cosf(radian);
//	result.m[0][2] = -std::sinf(radian);
//	result.m[1][1] = 1.0f;
//	result.m[2][0] = std::sin(radian);
//	result.m[2][2] = std::cosf(radian);
//	result.m[3][3] = 1.0f;
//
//	return result;
//}
//Matrix4x4 MakeRoateZMatrix(float radian)
//{
//	Matrix4x4 result = {};
//
//	result.m[0][0] = std::cosf(radian);
//	result.m[0][1] = std::sinf(radian);
//	result.m[1][0] = -std::sinf(radian);
//	result.m[1][1] = std::cosf(radian);
//	result.m[2][2] = 1.0f;
//	result.m[3][3] = 1.0f;
//
//	return result;
//}
//
//Matrix4x4 MakeAffineMatrix(const Vector3& scale, const Vector3& rotate, const Vector3& translate)
//{
//
//	Matrix4x4 scaleMatrix = {};
//	scaleMatrix.m[0][0] = scale.x;
//	scaleMatrix.m[1][1] = scale.y;
//	scaleMatrix.m[2][2] = scale.z;
//	scaleMatrix.m[3][3] = 1.0f;
//
//
//	Matrix4x4 rotateXMatrix = MakeRoateXMatrix(rotate.x);
//	Matrix4x4 rotateYMatrix = MakeRoateYMatrix(rotate.y);
//	Matrix4x4 rotateZMatrix = MakeRoateZMatrix(rotate.z);
//
//	Matrix4x4 rotateMatrix = Multiply(Multiply(rotateXMatrix, rotateYMatrix), rotateZMatrix);
//
//
//	Matrix4x4 translateMatrix = {};
//	translateMatrix.m[0][0] = 1.0f;
//	translateMatrix.m[1][1] = 1.0f;
//	translateMatrix.m[2][2] = 1.0f;
//	translateMatrix.m[3][3] = 1.0f;
//	translateMatrix.m[3][0] = translate.x;
//	translateMatrix.m[3][1] = translate.y;
//	translateMatrix.m[3][2] = translate.z;
//
//
//	Matrix4x4 affineMatrix = Multiply(Multiply(scaleMatrix, rotateMatrix), translateMatrix);
//
//	return affineMatrix;
//}
//
//Matrix4x4 Inverse(Matrix4x4& m) {
//	Matrix4x4 result;
//	float* a = &m.m[0][0];
//
//	float cof[16];
//	float det;
//
//
//	cof[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15]
//		+ a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
//	cof[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15]
//		- a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
//	cof[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15]
//		+ a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
//	cof[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11]
//		- a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
//
//	cof[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15]
//		- a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
//	cof[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15]
//		+ a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
//	cof[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15]
//		- a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
//	cof[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11]
//		+ a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
//
//	cof[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15]
//		+ a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
//	cof[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15]
//		- a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
//	cof[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15]
//		+ a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
//	cof[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11]
//		- a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
//
//	cof[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14]
//		- a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
//	cof[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14]
//		+ a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
//	cof[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14]
//		- a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
//	cof[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10]
//		+ a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
//
//
//	det = a[0] * cof[0] + a[1] * cof[4] + a[2] * cof[8] + a[3] * cof[12];
//
//
//	for (int i = 0; i < 4; ++i)
//		for (int j = 0; j < 4; ++j)
//		{
//			result.m[i][j] = cof[i * 4 + j] / det;
//		}
//
//	return result;
//}
//
//Matrix4x4 MakePerspectiveFovMatrix(float fovY, float aspectRatio, float nearClip, float farClip)
//{
//	Matrix4x4 result;
//	result.m[0][0] = (1.0f / aspectRatio) * (1.0f / tanf(fovY / 2.0f)); result.m[0][1] = 0.0f;    result.m[0][2] = 0.0f;    result.m[0][3] = 0;
//	result.m[1][0] = 0.0f;    result.m[1][1] = 1.0f / tanf(fovY / 2.0f); result.m[1][2] = 0.0f;    result.m[1][3] = 0;
//	result.m[2][0] = 0.0f;    result.m[2][1] = 0.0f;    result.m[2][2] = farClip / (farClip - nearClip); result.m[2][3] = 1.0f;
//	result.m[3][0] = 0.0f;    result.m[3][1] = 0.0f;    result.m[3][2] = -nearClip * farClip / (farClip - nearClip);    result.m[3][3] = 0.0f;
//
//	return result;
//}
//
//Matrix4x4 MakeOrthographicMatrix(float left, float top, float right, float bottom, float nearClip, float farClip)
//{
//	Matrix4x4 result;
//	result.m[0][0] = 2.0f / (right - left); result.m[0][1] = 0.0f;    result.m[0][2] = 0.0f;    result.m[0][3] = 0;
//	result.m[1][0] = 0.0f;    result.m[1][1] = 2.0f / (top - bottom); result.m[1][2] = 0.0f;    result.m[1][3] = 0;
//	result.m[2][0] = 0.0f;    result.m[2][1] = 0.0f;    result.m[2][2] = 1 / (farClip - nearClip); result.m[2][3] = 0;
//	result.m[3][0] = (left + right) / (left - right);    result.m[3][1] = (top + bottom) / (bottom - top);    result.m[3][2] = nearClip / (nearClip - farClip);    result.m[3][3] = 1.0f;
//
//	return result;
//
//}
////
////Vector3 Transform(const Vector3& vector, const Matrix4x4& matrix)
////{
////
////	float x = vector.x * matrix.m[0][0] + vector.y * matrix.m[1][0] + vector.z * matrix.m[2][0] + 1.0f * matrix.m[3][0];
////	float y = vector.x * matrix.m[0][1] + vector.y * matrix.m[1][1] + vector.z * matrix.m[2][1] + 1.0f * matrix.m[3][1];
////	float z = vector.x * matrix.m[0][2] + vector.y * matrix.m[1][2] + vector.z * matrix.m[2][2] + 1.0f * matrix.m[3][2];
////	float w = vector.x * matrix.m[0][3] + vector.y * matrix.m[1][3] + vector.z * matrix.m[2][3] + 1.0f * matrix.m[3][3];
////
////
////	if (w != 0.0f) {
////		x /= w;
////		y /= w;
////		z /= w;
////	}
////
////	return { x, y, z };
////}
////
