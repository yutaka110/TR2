#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

struct PostProcessPass {
    struct Parameters {
        float bloomThresholdMin = 0.45f;
        float bloomThresholdMax = 1.1f;
        float bloomSoftKnee = 0.2f;
        float bloomUpsampleBlend = 1.0f;
        float bloomUpsampleSoftKnee = 0.35f;
        float blurRadius = 4.0f;
        float distortionScale = 0.02f;
        float toneExposure = 1.0f;
        float glowWeight = 1.0f;
        float glowTintR = 1.0f;
        float glowTintG = 1.0f;
        float glowTintB = 1.0f;
    };

    std::string name;
    std::string inputResource = "SceneColor";
    std::string outputResource = "PostColor";
    std::string pipeline = "FullscreenComposite";
    std::string secondaryInputResource = "VfxAccumulation";
    std::string tertiaryInputResource = "SceneColor";
    bool enabled = true;
    float intensity = 1.0f;
    float resolutionScale = 1.0f;
    Parameters parameters{};
};

struct PostProcessExecutionPass {
    PostProcessPass pass;
};

struct PostProcessExecutionPlan {
    std::vector<PostProcessExecutionPass> passes;
    std::string finalOutputResource = "SceneColor";
};

class PostProcessStack {
public:
    void ResetToVfxDefaults();
    void SetEnabled(const std::string& name, bool enabled);
    void SetIntensity(const std::string& name, float intensity);
    bool IsEnabled(const std::string& name) const;
    float Intensity(const std::string& name) const;
    std::string FinalOutputResource() const;
    PostProcessExecutionPlan BuildExecutionPlan() const;
    const std::vector<PostProcessPass>& Passes() const { return passes_; }
    std::vector<PostProcessPass>& MutablePasses() { return passes_; }

private:
    std::vector<PostProcessPass> passes_;
};
