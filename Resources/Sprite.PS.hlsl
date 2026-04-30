struct Material
{
    float4 color;
    int enableLighting;
    float3 padding;
    float4x4 uvTransform;
    float shininess;
};

ConstantBuffer<Material> gMaterial : register(b0);
Texture2D<float4> gTexture : register(t0);
SamplerState gSampler : register(s0);

struct PixelShaderInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

float4 main(PixelShaderInput input) : SV_TARGET0
{
    float3 transformedUv = mul(float3(input.texcoord, 1.0f), (float3x3)gMaterial.uvTransform);
    float4 texColor = gTexture.Sample(gSampler, transformedUv.xy);
    return texColor * gMaterial.color;
}
