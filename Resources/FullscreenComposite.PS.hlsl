Texture2D<float4> gSceneColor : register(t0);
Texture2D<float4> gVfxAccumulation : register(t1);
Texture2D<float4> gPostColor : register(t2);
SamplerState gSampler : register(s0);

cbuffer CompositeParams : register(b0)
{
    float gBloomIntensity;
    float gDistortionIntensity;
    float gGlowIntensity;
    float gPostIntensity;
    float gAux4;
    float gAux5;
    float gAux6;
    float gAux7;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float4 main(PSInput input) : SV_TARGET
{
    float2 uv = saturate(input.uv);
    float4 scene = gSceneColor.Sample(gSampler, uv);
    float4 post = gPostColor.Sample(gSampler, uv);
    float postBlend = saturate(gPostIntensity);
    float postLuminance = dot(post.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    float effectiveBlend = postLuminance > 0.0001f ? postBlend : 0.0f;
    float3 color = lerp(scene.rgb, post.rgb, effectiveBlend);
    return float4(saturate(color), 1.0f);
}
