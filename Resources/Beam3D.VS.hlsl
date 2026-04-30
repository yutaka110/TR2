//======================================================
// BeamVS.hlsl : まっすぐな光の柱用 VS
//======================================================

// World * View * Projection
cbuffer BeamVS_CB : register(b0)
{
    float4x4 gWorldViewProj;
};

struct VSInput
{
    float3 pos : POSITION; // ローカル位置
    float2 uv : TEXCOORD0; // 0～1
};

struct VSOutput
{
    float4 svpos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput o;

    // ローカル → クリップ空間
    float4 wpos = mul(float4(input.pos, 1.0f), gWorldViewProj);
    o.svpos = wpos;

    // UV はそのまま PS へ渡す
    o.uv = input.uv;

    return o;
}
