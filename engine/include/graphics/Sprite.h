#pragma once
#pragma once
#include "utils/Math/MathUtils.h"  // Transform, Matrix4x4, Vector4 などの定義があるヘッダに差し替えて

// 単純な 2D/3D スプライト1枚分の情報
struct Sprite {
    Transform transform;  // scale, rotate, translate を全部持つ既存の Transform を利用
    Vector4   color;      // RGBA (0〜1)

    // 描画に使うテクスチャ（SRV）の GPU ハンドル
    // すでに TextureManager で SRV を取っているなら、そこから GPU ハンドルを渡せばOK
    D3D12_GPU_DESCRIPTOR_HANDLE textureGpuHandle{};

    // 便利用の初期化
    void SetPosition(float x, float y, float z = 0.0f) {
        transform.translate = { x, y, z };
    }
    void SetScale(float sx, float sy, float sz = 1.0f) {
        transform.scale = { sx, sy, sz };
    }
    void SetRotationZ(float rad) {
        transform.rotate.z = rad;
    }
};
