#include "Particle.hlsli"




VSOutput main(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;

    // position などは今まで通り作る（あなたの既存ロジックに合わせて）
    // 例：o.position = mul(gParticle[instanceId].WVP, localPos);
    float4x4 wvp = gParticle[instanceId].WVP;
    output.position = mul(input.position, wvp);
    output.texcoord = input.texcoord;
    output.color = gParticle[instanceId].color; // ★追加
    return output;
}

//VSOutput main(VSInput input, uint instanceId : SV_InstanceID)
//{
//    VSOutput output;

//    float4x4 wvp = gTransformationMatrices[instanceId].WVP;

//    output.position = mul(input.position, wvp);
//    output.texcoord = input.texcoord;

//    return output;
//}
