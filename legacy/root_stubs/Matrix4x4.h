//#pragma once
//
//
//struct Vector3
//{
//    float x;
//    float y;
//    float z;
//};
//
//struct Matrix3x3
//{
//    float m[3][3];
//}; 
//
//struct Matrix4x4
//{
//	float m[4][4];
//};
//
//struct Transform {
//    Vector3 scale;
//    Vector3 rotate;
//    Vector3 translate;
//};
//
//struct Sphere
//{
//    Vector3 center;
//    float radius;
//};
//
//struct TransformationMatrix {
//    Matrix4x4 WVP;
//    Matrix4x4 World;
//   
//};
//
//Matrix4x4 MakeIdentity4x4();
//Matrix4x4 Multiply(Matrix4x4 m1, Matrix4x4 m2);
//Matrix4x4 Inverse(Matrix4x4& m);
//Matrix4x4 Transpose(const Matrix4x4& mat);
//Vector3 Normalize(const Vector3& v);
////Vector3 Transform(const Vector3& vector, const Matrix4x4& matrix);
////Matrix3x3 MakeScaleMatrix(const Vector2& scale);
////Matrix3x3 MakeTranslateMatrix(const Vector2& translate);
//Matrix4x4 MakeScaleMatrix(const Vector3& scale);
//Matrix4x4 MakeTranslateMatrix(const Vector3& translate);
//Matrix3x3 MakeRotateZMatrix(float angleRad);
//Matrix4x4 MakePerspectiveFovMatrix(float fovY, float aspectRatio, float nearClip, float farClip);
//Matrix4x4 MakeRoateXMatrix(float radian);
//Matrix4x4 MakeRoateYMatrix(float radian);
//Matrix4x4 MakeRoateZMatrix(float radian);
//Matrix4x4 MakeAffineMatrix(const Vector3& scale, const Vector3& rotate, const Vector3& translate);
////Matrix4x4 MakeOrthographicMatrix(float left, float top, float right, float bottom, float nearClip, float farClip);
