#pragma once

class BeamRenderer;
class DistortionRenderer;
class ParticleRenderer;
class TrailRenderer;

struct AppVfxRendererSet {
    ParticleRenderer* particle = nullptr;
    TrailRenderer* trail = nullptr;
    BeamRenderer* beam = nullptr;
    DistortionRenderer* distortion = nullptr;

    bool IsValid() const {
        return particle != nullptr &&
            trail != nullptr &&
            beam != nullptr &&
            distortion != nullptr;
    }
};
