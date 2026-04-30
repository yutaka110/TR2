//float4 main(float4 pos : POSITION) : SV_POSITION
//{
//    return pos;
//}
#include "object3d.hlsli"



struct TransformationMatrix
{
   float4x4  WVP;
    float4x4 World;
    float4x4 WorldInverseTranspose;
};

ConstantBuffer<TransformationMatrix> gTransformationMatrix : register(b0);

//struct VertexShaderOutput
//{
//    float4 position : SV_POSITION;
//    float2 texcoord : TEXCOORD0;
//};

struct VertexShaderInput
{
    float4 position : POSITION0;
    float2 texcoord : TEXCOORD0;
    float3 normal : NORMAL0;
};



VertexShaderOutput main(VertexShaderInput input)
{
    VertexShaderOutput output;
    output.position = mul(input.position, gTransformationMatrix.WVP);
    output.texcoord = input.texcoord;
   // output.normal = normalize(mul(input.normal, (float3x3) gTransformationMatrix.World));

    // 追加
   // output.worldPosition = mul(input.position, gTransformationMatrix.World).xyz;
    
    // ワールド座標（PSでV計算に使ってる）
    output.worldPosition = mul(input.position, gTransformationMatrix.World).xyz;

     // ★ 法線をワールド空間へ変換 ★
    //float3 worldNormal =
    //    mul(input.normal, (float3x3) gTransformationMatrix.World);

    float3 worldNormal = mul(input.normal, (float3x3) gTransformationMatrix.WorldInverseTranspose);

    
    output.normal = normalize(worldNormal);

    output.texcoord = input.texcoord;
    
    return output;
}


