#include "particle/ParticleSystemCpu.h"

namespace particlecpu {

uint32_t UpdateInstanceMatrices_List(const Matrix4x4& viewProj, float deltaTime)
{
	uint32_t numInstance = 0;
	static constexpr uint32_t kNumInstance = 1024; // ※ main側のインスタンシングバッファ容量と同じにする

	for (auto it = particles.begin(); it != particles.end(); )
	{
		// 寿命切れはリストから消す（ここがポイント）
		if (it->currentTime >= it->lifeTime) {
			it = particles.erase(it);
			continue;
		}

		// Field範囲内なら加速度を速度に加える（v += a * dt）
		if (IsCollision(accelerationField.area, it->transform.translate)) {
			it->velocity.x += accelerationField.acceleration.x * deltaTime;
			it->velocity.y += accelerationField.acceleration.y * deltaTime;
			it->velocity.z += accelerationField.acceleration.z * deltaTime;
		}

		// 最大数を超えたら「更新だけして描画しない」（VRAM破壊防止）
		if (numInstance >= kNumInstance) {
			it->transform.translate.x += it->velocity.x * deltaTime;
			it->transform.translate.y += it->velocity.y * deltaTime;
			it->currentTime += deltaTime;
			++it;
			continue;
		}

		// 更新
		it->transform.translate.x += it->velocity.x * deltaTime;
		it->transform.translate.y += it->velocity.y * deltaTime;
		it->currentTime += deltaTime;

		// 行列
		Matrix4x4 world = MakeAffineMatrix(it->transform.scale, it->transform.rotate, it->transform.translate);
		gInstancingData[numInstance].World = world;
		gInstancingData[numInstance].WVP = Multiply(world, viewProj);

		// フェード（α）
		float alpha = 1.0f - (it->currentTime / it->lifeTime);
		Vector4 c = it->color;
		c.x *= alpha;
		c.y *= alpha;
		c.z *= alpha;
		c.w = alpha;
		gInstancingData[numInstance].color = c;

		++numInstance;
		++it;
	}

	return numInstance;
}
 // UpdateInstanceMatrices_List

} // namespace particlecpu
