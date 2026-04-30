struct ParticleForGPU
{
    float4x4 WVP;
    float4x4 World;
    float4 color;
};

struct ParticleState
{
    float3 position;
    float age;
    float3 velocity;
    float lifetime;
    float4 color;
    float3 scale;
    float seed;
};

cbuffer SimConstants : register(b0)
{
    float4x4 gViewProjection;
    float gDeltaTime;
    float gTime;
    uint gMaxParticles;
    float gPad;
    float4 gTint;
    float4 gScaleAndParams;
    float4 gEffectParams;
};

RWStructuredBuffer<ParticleForGPU> gParticleOutput : register(u0);
RWStructuredBuffer<ParticleState> gParticleState : register(u1);

float3 HashSpawn(uint id, float seed, float radius)
{
    float x = frac(sin((float)id * 12.9898 + seed * 78.233) * 43758.5453);
    float y = frac(sin((float)id * 39.3467 + seed * 11.135) * 24634.6345);
    float z = frac(sin((float)id * 73.1569 + seed * 91.753) * 16431.5172);
    return float3((x - 0.5f) * radius * 2.0f, (y - 0.5f) * radius, 2.0f + z * max(radius, 0.1f));
}

float4x4 MakeWorld(float3 position, float3 scale)
{
    return float4x4(
        scale.x, 0.0f, 0.0f, 0.0f,
        0.0f, scale.y, 0.0f, 0.0f,
        0.0f, 0.0f, scale.z, 0.0f,
        position.x, position.y, position.z, 1.0f);
}

[numthreads(256, 1, 1)]
void main(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    uint id = dispatchThreadId.x;
    if (id >= gMaxParticles)
    {
        return;
    }

    ParticleState state = gParticleState[id];
    state.age += gDeltaTime;
    if (state.age >= state.lifetime)
    {
        state.age = 0.0f;
        state.position = HashSpawn(id, state.seed + gTime, max(gEffectParams.y, 0.1f));
        state.velocity = float3(state.seed * 0.8f - 0.4f, 0.6f + state.seed, state.seed * 0.5f - 0.25f);
    }

    float normalizedAge = saturate(state.age / max(state.lifetime, 0.001f));
    float turbulence = gScaleAndParams.w;
    float curl = sin(gTime * (2.0f + gEffectParams.z) + (float)id * 0.03125f) * (0.25f + turbulence);
    state.velocity.x += curl * gDeltaTime;
    state.velocity.y += (0.15f - normalizedAge * 0.2f) * gDeltaTime;
    state.position += state.velocity * gDeltaTime;
    gParticleState[id] = state;

    float alpha = 1.0f - normalizedAge;
    float pulse = 0.65f + 0.35f * sin(gTime * max(gEffectParams.x, 0.01f) + state.seed * 6.28318f);
    float3 scale = state.scale * float3(gScaleAndParams.x, gScaleAndParams.y, 1.0f) * (0.7f + normalizedAge * 1.7f);
    float4x4 world = MakeWorld(state.position, scale);

    ParticleForGPU output;
    output.World = world;
    output.WVP = mul(world, gViewProjection);
    output.color = float4(state.color.rgb * gTint.rgb * pulse * max(gScaleAndParams.z, 0.01f), alpha * gTint.a);
    gParticleOutput[id] = output;
}
