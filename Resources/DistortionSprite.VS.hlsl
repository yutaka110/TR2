#include "Particle.hlsli"

VSOutput main(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;

    ParticleForGPU particle = gParticle[instanceId];
    float2 centeredUv = input.texcoord - 0.5f;
    float pulse = 1.0f + dot(centeredUv, centeredUv) * 0.35f;

    float4 local = input.position;
    local.xy *= pulse;

    output.position = mul(local, particle.WVP);
    output.texcoord = input.texcoord;
    output.color = particle.color;
    return output;
}
