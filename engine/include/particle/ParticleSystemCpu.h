#pragma once
#include <cstdint>
#include <list>
#include <random>

#include "utils/math/Vector.h"
#include "utils/math/MathUtils.h"

namespace particlecpu {

struct Particle {
	Transform transform;
	Vector3 velocity;
	Vector4 color;
	float lifeTime;
	float currentTime;
};

struct ParticleForGPU {
	Matrix4x4 WVP;
	Matrix4x4 World;
	Vector4 color;
};

struct Emitter {
	Transform transform;
	uint32_t count;
	float frequency;      // 発生周期（秒）
	float frequencyTime;  // 経過
};

struct AABB {
	Vector3 min;
	Vector3 max;
};

struct AccelerationField {
	Vector3 acceleration; // 加速度（風/重力）
	AABB area;            // 影響範囲
};

bool IsCollision(const AABB& aabb, const Vector3& p)
{
	if (p.x < aabb.min.x || p.x > aabb.max.x) return false;
	if (p.y < aabb.min.y || p.y > aabb.max.y) return false;
	if (p.z < aabb.min.z || p.z > aabb.max.z) return false;
	return true;
}


//static ParticleForGPU* gInstancingData = nullptr; // Mapした先

//static Particle particle[kNumInstance];
std::list<Particle> particles;
static ParticleForGPU* gInstancingData = nullptr; // instancingResource->Map でセット

std::random_device seedGenerator;
std::mt19937 randomEngine(seedGenerator());

AccelerationField accelerationField{};

Particle MakeNewParticle(std::mt19937& randomEngine)
{
	std::uniform_real_distribution<float> distPos(-1.0f, 1.0f);
	std::uniform_real_distribution<float> distVel(-1.0f, 1.0f);
	std::uniform_real_distribution<float> distCol(0.0f, 1.0f);
	std::uniform_real_distribution<float> distTime(1.0f, 3.0f);

	Particle p{};
	p.transform.scale = { 1.0f, 1.0f, 1.0f };
	p.transform.rotate = { 0.0f, 0.0f, 0.0f };
	p.transform.translate = { distPos(randomEngine), distPos(randomEngine), 2.0f + distPos(randomEngine) };

	p.velocity = { distVel(randomEngine), distVel(randomEngine), distVel(randomEngine) };

	p.color = { distCol(randomEngine), distCol(randomEngine), distCol(randomEngine), 1.0f };
	p.lifeTime = distTime(randomEngine);
	p.currentTime = 0.0f;
	return p;
}

Particle MakeNewParticle(std::mt19937& randomEngine, const Vector3& baseTranslate)
{
	std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
	std::uniform_real_distribution<float> distScale(0.1f, 0.5f);
	Particle p{};
	// scale/rotateは今まで通り
	float s = distScale(randomEngine);
	p.transform.scale = { s, s, 1.0f }; 
	p.transform.rotate = { 0,0,0 };


	Vector3 randomOffset = { dist(randomEngine), dist(randomEngine), dist(randomEngine) };
	p.transform.translate.x = baseTranslate.x + randomOffset.x;   // ★ここが本体
	p.transform.translate.y = baseTranslate.y + randomOffset.y;
	// velocity / color / lifetime も今まで通り
	return p;
}

std::list<Particle> Emit(const Emitter& emitter, std::mt19937& randomEngine)
{
	std::list<Particle> out;
	for (uint32_t i = 0; i < emitter.count; ++i) {
		Particle p = MakeNewParticle(randomEngine);

		// エミッタ位置から出したいならここで上書き
		p.transform.translate = emitter.transform.translate;

		out.push_back(p);
	}
	return out;
}


//void UpdateInstanceMatrices(const Matrix4x4& viewProj, float deltaTime)
//{
//	for (uint32_t i = 0; i < kNumInstance; ++i) {
//
//		//auto& tr = particle[i];
//		//tr.transform.translate.y+= tr.velocity.y * deltaTime;
//		////tr.rotate.y += 0.5f * deltaTime;
//
//		//Matrix4x4 world =
//		//	MakeAffineMatrix(tr.transform.scale, tr.transform.rotate, tr.transform.translate);
//		//
//
//		//gInstancingData[i].World = world;
//		//gInstancingData[i].WVP = Multiply(world, viewProj);
//		//// ★追加：色もGPUへ
//		//gInstancingData[i].color = tr.color;
//		// 寿命超え → そのフレームは描画しない（必要ならここで再生成してもOK）
//		if (particle[i].currentTime >= particle[i].lifeTime) {
//			continue;
//		}
//
//		// 移動
//		particle[i].transform.translate.x += particle[i].velocity.x * deltaTime;
//		particle[i].transform.translate.y += particle[i].velocity.y * deltaTime;
//		//particle[i].transform.translate.z += particle[i].velocity.z * deltaTime;
//
//		// 経過時間
//		particle[i].currentTime += deltaTime;
//
//		// world / wvp
//		Matrix4x4 world = MakeAffineMatrix(
//			particle[i].transform.scale,
//			particle[i].transform.rotate,
//			particle[i].transform.translate);
//
//		gInstancingData[numInstance].World = world;
//		gInstancingData[numInstance].WVP = Multiply(world, viewProj);
//
//		// 徐々に消す（alpha）
//		float alpha = 1.0f - (particle[i].currentTime / particle[i].lifeTime);
//		Vector4 c = particle[i].color;
//		c.w = alpha;
//
//		gInstancingData[numInstance].color = c;
//
//		++numInstance; // ★生きてる数だけ増える
//	}
//	
//}
//uint32_t UpdateInstanceMatrices(const Matrix4x4& viewProj, float deltaTime)
//{
//	uint32_t numInstance = 0; // ★毎フレーム0スタート（超重要）
//
//	for (uint32_t i = 0; i < kNumInstance; ++i) {
//
//		if (particle[i].currentTime >= particle[i].lifeTime) {
//			continue; // 寿命切れは詰めない
//		}
//
//		particle[i].transform.translate.x += particle[i].velocity.x * deltaTime;
//		particle[i].transform.translate.y += particle[i].velocity.y * deltaTime;
//
//		particle[i].currentTime += deltaTime;
//
//		Matrix4x4 world = MakeAffineMatrix(
//			particle[i].transform.scale,
//			particle[i].transform.rotate,
//			particle[i].transform.translate);
//
//		gInstancingData[numInstance].World = world;
//		gInstancingData[numInstance].WVP = Multiply(world, viewProj);
//
//		float alpha = 1.0f - (particle[i].currentTime / particle[i].lifeTime);
//		Vector4 c = particle[i].color;
//		c.w = alpha;
//		gInstancingData[numInstance].color = c;
//
//		++numInstance;
//	}
//
//	return numInstance; // ★描画数を返す
//}

 // structs + globals + helpers up to before UpdateInstanceMatrices_List

uint32_t UpdateInstanceMatrices_List(const Matrix4x4& viewProj, float deltaTime);

} // namespace particlecpu
