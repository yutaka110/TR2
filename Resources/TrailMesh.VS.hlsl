#include "Particle.hlsli"

VSOutput main(VSInput input, uint instanceId : SV_InstanceID)
{
    VSOutput output;

    ParticleForGPU particle = gParticle[instanceId];
    float trailT = saturate(input.texcoord.y);
    float widthT = abs(input.texcoord.x - 0.5f) * 2.0f;

    float4 local = input.position;
    local.x *= lerp(1.0f, 0.18f, trailT);
    local.y = input.position.y * (1.0f + trailT * 2.4f);
    local.z -= trailT * 1.6f;

    output.position = mul(local, particle.WVP);
    output.texcoord = input.texcoord;

    float fade = pow(1.0f - trailT, 1.6f) * (1.0f - widthT * 0.25f);
    output.color = particle.color * float4(1.0f, 0.82f, 0.45f, fade);
    return output;
}
