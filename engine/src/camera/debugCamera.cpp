#include"camera/DebugCamera.h"
#include<numbers>
#include <Windows.h>
void DebugCamera::Initialize() {
    // 初期値の設定
    rotation_ = { 0.0f, 0.0f, 0.0f };
    translation_ = { 0.0f, 0.0f, -8.0f };
    viewMatrix_ = MakeIdentity4x4();

    // 透視投影行列（FOV 45°, アスペクト比 16:9, Near/Far）
    float fovY = 0.25f * std::numbers::pi_v<float>; // 45度
    float aspectRatio = 16.0f / 9.0f;
    float nearZ = 0.1f;
    float farZ = 1000.0f;
    projectionMatrix_ = MakePerspectiveFovMatrix(fovY, aspectRatio, nearZ, farZ);
}

void DebugCamera::Update() {

    /* KeyInput keyInput;
     keyInput.Initialize( hwnd);*/

    const float moveSpeed = 0.1f;
    const float rotateSpeed = 0.02f;

    //// 追加回転分の回転行列を生成
    //Matrix4x4 matRotDelta = MakeIdentity4x4();
    //matRotDelta *= MakeRoateXMatrix(rotation_.x);
    //matRotDelta *= MakeRoateYMatrix(rotation_.y);

    //// 累積の回転行列と合成
    //matRot_ = matRotDelta * matRot_;

   // printf("Z = %f\n", translation_.z);

    // キー入力で位置を調整（ローカルZ軸前後移動）
    if (GetAsyncKeyState('W') & 0x8000) {
        translation_.z += moveSpeed;
    }
    if (GetAsyncKeyState('S') & 0x8000) {
        translation_.z -= moveSpeed;
    }

    // キー入力で横移動（ローカルX軸）
    if (GetAsyncKeyState('D') & 0x8000) {
        translation_.x += moveSpeed;
    }
    if (GetAsyncKeyState('A') & 0x8000) {
        translation_.x -= moveSpeed;
    }

    // 上下回転（ローカルX軸）
    if (GetAsyncKeyState(VK_UP) & 0x8000) {
        rotation_.x += rotateSpeed;
    }
    if (GetAsyncKeyState(VK_DOWN) & 0x8000) {
        rotation_.x -= rotateSpeed;
    }

    // 左右回転（ローカルY軸）
    if (GetAsyncKeyState(VK_RIGHT) & 0x8000) {
        rotation_.y += rotateSpeed;
    }
    if (GetAsyncKeyState(VK_LEFT) & 0x8000) {
        rotation_.y -= rotateSpeed;
    }


    // ビュー行列の更新
    Matrix4x4 worldMatrix = MakeAffineMatrix(
        Vector3{ 1, 1, 1 }, // scale（等倍）
        rotation_,       // 回転
        translation_     // 平行移動
    );

    viewMatrix_ = Inverse(worldMatrix);
}
