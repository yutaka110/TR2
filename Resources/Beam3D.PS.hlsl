struct VSOutput
{
    float4 svpos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

Texture2D rampTex : register(t0); // beamRamp_lightning.png
Texture2D noiseTex : register(t1); // streakNoise.png
SamplerState samp : register(s0);

cbuffer BeamPS_CB : register(b1)
{
    float4 baseColor;
    float intensity;
    float time;
    float2 pad;
};

float4 PSMain(VSOutput input) : SV_TARGET
{
    float2 uv = saturate(input.uv);

    //--------------------------------------------------
    // 1) ノイズを2種類サンプリング（ギザギザ用）
    //--------------------------------------------------
    float t1 = frac(time * 0.40f);
    float t2 = frac(time * 0.83f);

    float2 noiseUV1 = float2(uv.y * 10.0f, t1);
    float2 noiseUV2 = float2(uv.y * 25.0f, t2 + 1.37f);

    float n1 = noiseTex.Sample(samp, noiseUV1).r; // 0-1
    float n2 = noiseTex.Sample(samp, noiseUV2).r;

    // 混ぜて少しランダムっぽく
    float n = saturate(n1 * 0.6f + n2 * 0.4f);

    //--------------------------------------------------
    // 2) 横方向のギザギザオフセット
    //--------------------------------------------------
    float xFromCenter = uv.x - 0.5f;

    // 中心ほどよく曲がる（端はあまり動かさない）
    float centerFactor = 1.0f - saturate(abs(xFromCenter) * 8.0f);

    float amplitude = 0.32f; // 曲がりの強さ
    float offsetX = (n - 0.5f) * amplitude * centerFactor;

    // ちょっとサインも混ぜて「雷」っぽく
    offsetX += 0.05f * sin(uv.y * 40.0f + time * 8.0f);

    float warpedX = xFromCenter + offsetX;

    //--------------------------------------------------
    // 3) 太さマスク（コア＋グロー）＆ 欠けマスク
    //--------------------------------------------------
    const float coreWidth = 0.05f; // 真ん中の白い芯
    const float glowWidth = 0.11f; // 外側の光

    float coreNorm = abs(warpedX) / coreWidth;
    float glowNorm = abs(warpedX) / glowWidth;

    float coreMask = saturate(1.0f - coreNorm);
    float glowMask = saturate(1.0f - glowNorm);

    // 雷っぽい「途切れ」を作る（ノイズが低いところは細く）
    float gapMask = smoothstep(0.25f, 0.65f, n); // 0.25以下ほぼ0, 0.65以上1
    coreMask *= gapMask;
    glowMask *= gapMask;

    // コアはキツめ、グローは柔らかめ
    coreMask = pow(coreMask, 3.0f);
    glowMask = pow(glowMask, 1.5f);

    // UV を戻す
    float2 beamUV = float2(saturate(warpedX + 0.5f), uv.y);

    //--------------------------------------------------
    // 4) Ramp で色付け（白い芯＋外側の緑）
    //--------------------------------------------------
    float4 ramp = rampTex.Sample(samp, float2(0.5f, beamUV.y));

    // コアはかなり明るく、グローは少し弱く
    float3 coreCol = ramp.rgb * 2.0f * coreMask;
    float3 glowCol = ramp.rgb * 0.9f * glowMask;

    float3 col = (coreCol + glowCol) * baseColor.rgb * intensity;

    // アルファもコア＋グローで
    float alpha = saturate(coreMask * 1.0f + glowMask * 0.6f) * baseColor.a;

    return float4(col, alpha);
}
