/*// === 動体検知用 Compute Shader ===
Texture2D<float4> CurrentFrame   : register(t0);
Texture2D<float4> PreviousFrame  : register(t1);
RWTexture2D<float4> MotionMask   : register(u0);  // 出力先

cbuffer ThresholdBuffer : register(b0) {
    float threshold;  // 動体検出のしきい値
}

[numthreads(8, 8, 1)]
void main(uint3 DTid : SV_DispatchThreadID)
{
    float4 curr = CurrentFrame.Load(int3(DTid.xy, 0));
    float4 prev = PreviousFrame.Load(int3(DTid.xy, 0));

    float diff = length(curr.rgb - prev.rgb);  // RGBの差の大きさ

    float mask = diff > threshold ? 1.0 : 0.0;
    MotionMask[DTid.xy] = float4(mask, mask, mask, 1.0); // グレースケール出力
}*/


// --- SRV: t4 = Y (R8_UNORM), t5 = UV (R8G8_UNORM)
// --- UAV: u0 = 出力 RGB (R8G8B8A8_UNORM)
Texture2D<float> texY        : register(t4);
Texture2D<float2> texUV      : register(t5);
RWTexture2D<float4> outTex   : register(u0);

// --- スレッドグループ（1ピクセル単位）
[numthreads(8, 8, 1)]
void main(uint3 dtid : SV_DispatchThreadID)
{
    float y = texY.Load(int3(dtid.xy, 0));
    float2 uv = texUV.Load(int3(dtid.xy / 2, 0)) - float2(0.5f, 0.5f); // UVは1/2解像度

    // BT.601 カラー変換（Rec.601）
    float3 rgb;
    rgb.r = y + 1.402 * uv.y;
    rgb.g = y - 0.344136 * uv.x - 0.714136 * uv.y;
    rgb.b = y + 1.772 * uv.x;

    outTex[dtid.xy] = float4(saturate(rgb), 1.0f); // clampとA=1
}

