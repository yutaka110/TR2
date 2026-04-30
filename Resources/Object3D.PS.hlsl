#include "Object3d.hlsli"

// ------------------------------------------------------------
// Material / Light
// ------------------------------------------------------------
struct Material
{
    float32_t4 color;
    int32_t enableLighting;
    float32_t4x4 uvTransform;
    float shininess;
    float3 pad_; // 16byte alignment
};

struct DirectionalLight
{
    float4 color; // light color
    float3 direction; // light direction (unit vector)
    float intensity; // brightness
};

struct PointLight
{
    float4 color; // ライトの色
    float3 position; // ライト位置
    float intensity; // 輝度
    float radius; // 影響半径（最大距離）
    float decay; // 減衰の指数（大きいほど急激）
    float2 pad_; // 16byte alignment（重要）
};

struct SpotLight
{
    float4 color;
    float3 position;
    float intensity;
    float3 direction; // スポットが向く方向（ワールド、単位ベクトル）
    float distance; // 最大到達距離
    float decay; // 減衰指数
    float cosAngle; // 内側円錐のcos（例: cos(pi/3)）
    float pad_; // 16byte alignment
};

ConstantBuffer<SpotLight> gSpotLight : register(b4);



cbuffer Camera : register(b2)
{
    float3 cameraWorldPosition;
    float padCam;
}

ConstantBuffer<Material> gMaterial : register(b0);
ConstantBuffer<DirectionalLight> gDirectionalLight : register(b1);

// ★追加：PointLight は b3（資料どおり）
ConstantBuffer<PointLight> gPointLight : register(b3);

Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

// kept for your project (not used here)
Texture2D<float4> gReceivedTex : register(t4);

// motion mask (optional)
Texture2D<float4> motionMaskTex : register(t2);

// ------------------------------------------------------------
// Output
// ------------------------------------------------------------
struct PixelShaderOutput
{
    float4 color : SV_TARGET0;
};

// ------------------------------------------------------------
// Switch (optional)
// 1 = Blinn-Phong (HalfVector)
// 0 = Phong (Reflect)
// ------------------------------------------------------------
#define USE_BLINN_PHONG 1

// ------------------------------------------------------------
// Safe normalize (prevents NaN when vector length is near zero)
// ------------------------------------------------------------
static float3 SafeNormalize(float3 v)
{
    float len2 = dot(v, v);
    if (len2 < 1e-8f)
    {
        return float3(0.0f, 0.0f, 1.0f);
    }
    return v * rsqrt(len2);
}

// ------------------------------------------------------------
// PS Main
// ------------------------------------------------------------
PixelShaderOutput main(VertexShaderOutput input)
{
    PixelShaderOutput output;

    float2 uv = input.texcoord;

    float4 texColor = gTexture.Sample(gSampler, uv);
    float4 maskColor = motionMaskTex.Sample(gSampler, uv);

    float3 baseRgb = texColor.rgb * gMaterial.color.rgb;
    float baseA = texColor.a * gMaterial.color.a;

    float3 outRgb = baseRgb;

    if (gMaterial.enableLighting != 0)
    {
        float3 N = SafeNormalize(input.normal);

        // V = pixel -> camera
        float3 V = SafeNormalize(cameraWorldPosition - input.worldPosition);

        //========================================================
        // 1) DirectionalLight 分
        //========================================================
        float3 Ld = SafeNormalize(-gDirectionalLight.direction); // pixel -> light
        float3 dirLightColor = gDirectionalLight.color.rgb * gDirectionalLight.intensity;

        // Diffuse (Half-Lambert)
        float NdotLd = saturate(dot(N, Ld));
        float halfLambertD = pow(NdotLd * 0.5f + 0.5f, 2.0f);

        float3 diffuseD =
            baseRgb *
            dirLightColor *
            halfLambertD;

        // Specular
        float specPowD = 0.0f;
#if USE_BLINN_PHONG
        float3 Hd = SafeNormalize(Ld + V);
        float NdotHd = saturate(dot(N, Hd));
        specPowD = pow(NdotHd, gMaterial.shininess);
#else
        float3 Rd = reflect(-Ld, N);
        specPowD = pow(saturate(dot(Rd, V)), gMaterial.shininess);
#endif

        float3 specularD =
            dirLightColor *
            specPowD;

       //========================================================
// 2) PointLight 分 ★距離減衰あり（逆二乗）
//========================================================
        float3 toPL = gPointLight.position - input.worldPosition;

        float dist = length(toPL);

// 半径が0だと割れちゃうので保険
        float r = max(gPointLight.radius, 0.001f);

// 資料の形：pow(saturate(-dist/r + 1), decay) = pow(saturate(1 - dist/r), decay)
        float factor = pow(saturate(1.0f - dist / r), gPointLight.decay);


// Lp = pixel -> pointLight（入射光方向）
        float3 Lp = SafeNormalize(toPL);

// 色 = pointLight の色 * intensity * 減衰
        float3 pointLightColor = gPointLight.color.rgb * gPointLight.intensity * factor;


// Diffuse (Half-Lambert)
        float NdotLp = saturate(dot(N, Lp));
        float halfLambertP = pow(NdotLp * 0.5f + 0.5f, 2.0f);

        float3 diffuseP =
    baseRgb *
    pointLightColor *
    halfLambertP;

// Specular
        float specPowP = 0.0f;

#if USE_BLINN_PHONG
        float3 Hp = SafeNormalize(Lp + V);
        float NdotHp = saturate(dot(N, Hp));
        specPowP = pow(NdotHp, max(gMaterial.shininess, 1.0f));
#else
float3 Rp = reflect(-Lp, N);
specPowP = pow(saturate(dot(Rp, V)), max(gMaterial.shininess, 1.0f));
#endif

        float3 specularP =
    pointLightColor *
    specPowP;

       //========================================================
// 2.5) SpotLight 分（距離減衰 + Falloff）※向き整理版
//========================================================
        float3 toSurface = input.worldPosition - gSpotLight.position; // light -> pixel
        float distS = length(toSurface);

// light->pixel（フォールオフ用）
        float3 Ls_out = SafeNormalize(toSurface);

// pixel->light（拡散・鏡面用）
        float3 Ls = -Ls_out;

// 距離減衰：0で最大、distanceで0
        float maxDist = max(gSpotLight.distance, 0.001f);
        float atten = pow(saturate(1.0f - distS / maxDist), gSpotLight.decay);

// Falloff（角度減衰）
// direction は「スポットが向く方向」（lightから照らす向き）とする
        float3 Sd = SafeNormalize(gSpotLight.direction);
        float cosA = dot(Ls_out, Sd); // 中心軸だと1

// cosAngleが1に近いと分母が0になるので保険
        float denom = max(1.0f - gSpotLight.cosAngle, 1e-4f);
        float falloff = saturate((cosA - gSpotLight.cosAngle) / denom);

// ライト色（距離減衰＋角度減衰込み）
        float3 spotColor = gSpotLight.color.rgb * gSpotLight.intensity * atten * falloff;

// Diffuse（Half-Lambert）
        float NdotLs = saturate(dot(N, Ls));
        float halfLambertS = pow(NdotLs * 0.5f + 0.5f, 2.0f);
        float3 diffuseS = baseRgb * spotColor * halfLambertS;

// Specular（Blinn）
        float3 Hs = SafeNormalize(Ls + V);
        float specPowS = pow(saturate(dot(N, Hs)), max(gMaterial.shininess, 1.0f));
        float3 specularS = spotColor * specPowS;



        //========================================================
        // 3) 最終合成：全部足す（資料どおり）
        //========================================================
        outRgb = (diffuseD + specularD) + (diffuseP + specularP) + (diffuseS + specularS);


    }

    output.color = float4(outRgb, baseA);
    return output;
}
