#pragma once
#include "utils/math/MathUtils.h"
/// <summary>
/// デバッグカメラ
/// </summary>
class DebugCamera {

public:

	/// <summary>
   /// コンストラクタ
   /// </summary>
	//DebugCamera();


	/// <summary>
   /// 初期化
   /// </summary>
	void Initialize();

	Matrix4x4 GetViewMatrix() const { return viewMatrix_; }
	Matrix4x4 GetProjectionMatrix() const { return projectionMatrix_; }

	/// <summary>
	/// 更新
	/// </summary>
	void Update();

	// X, Y, Z軸回りのローカル回転角
	Vector3 rotation_ = { 0, 0, 0 };

	Matrix4x4 matRot_;

	// ローカル座標
	Vector3 translation_ = { 0, 0, -50 };

	// ビュー行列
	Matrix4x4 viewMatrix_;

	// 射影行列
	Matrix4x4 projectionMatrix_;

};
