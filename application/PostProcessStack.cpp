#include "PostProcessStack.h"

#include <utility>

void PostProcessStack::ResetToVfxDefaults() {
    passes_.clear();
    PostProcessPass bloomExtract{};
    bloomExtract.name = "BloomExtract";
    bloomExtract.inputResource = "VfxAccumulation";
    bloomExtract.outputResource = "BloomExtractHalf";
    bloomExtract.pipeline = "BloomExtract";
    bloomExtract.secondaryInputResource = "VfxAccumulation";
    bloomExtract.tertiaryInputResource = "VfxAccumulation";
    bloomExtract.enabled = true;
    bloomExtract.intensity = 1.0f;
    bloomExtract.resolutionScale = 0.5f;
    bloomExtract.parameters.bloomThresholdMin = 0.45f;
    bloomExtract.parameters.bloomThresholdMax = 1.1f;
    bloomExtract.parameters.bloomSoftKnee = 0.2f;
    passes_.push_back(bloomExtract);

    PostProcessPass bloomDownsampleQuarter{};
    bloomDownsampleQuarter.name = "BloomDownsampleQuarter";
    bloomDownsampleQuarter.inputResource = "BloomExtractHalf";
    bloomDownsampleQuarter.outputResource = "BloomQuarterA";
    bloomDownsampleQuarter.pipeline = "BloomDownsample";
    bloomDownsampleQuarter.secondaryInputResource = "BloomExtractHalf";
    bloomDownsampleQuarter.tertiaryInputResource = "BloomExtractHalf";
    bloomDownsampleQuarter.enabled = true;
    bloomDownsampleQuarter.intensity = 1.0f;
    bloomDownsampleQuarter.resolutionScale = 0.25f;
    passes_.push_back(bloomDownsampleQuarter);

    PostProcessPass bloomBlurHorizontalQuarter{};
    bloomBlurHorizontalQuarter.name = "BloomBlurHorizontalQuarter";
    bloomBlurHorizontalQuarter.inputResource = "BloomQuarterA";
    bloomBlurHorizontalQuarter.outputResource = "BloomQuarterB";
    bloomBlurHorizontalQuarter.pipeline = "BlurHorizontal";
    bloomBlurHorizontalQuarter.secondaryInputResource = "BloomQuarterA";
    bloomBlurHorizontalQuarter.tertiaryInputResource = "BloomQuarterA";
    bloomBlurHorizontalQuarter.enabled = true;
    bloomBlurHorizontalQuarter.intensity = 1.0f;
    bloomBlurHorizontalQuarter.resolutionScale = 0.25f;
    bloomBlurHorizontalQuarter.parameters.blurRadius = 6.0f;
    passes_.push_back(bloomBlurHorizontalQuarter);

    PostProcessPass bloomBlurVerticalQuarter{};
    bloomBlurVerticalQuarter.name = "BloomBlurVerticalQuarter";
    bloomBlurVerticalQuarter.inputResource = "BloomQuarterB";
    bloomBlurVerticalQuarter.outputResource = "BloomQuarterA";
    bloomBlurVerticalQuarter.pipeline = "BlurVertical";
    bloomBlurVerticalQuarter.secondaryInputResource = "BloomQuarterB";
    bloomBlurVerticalQuarter.tertiaryInputResource = "BloomQuarterB";
    bloomBlurVerticalQuarter.enabled = true;
    bloomBlurVerticalQuarter.intensity = 1.0f;
    bloomBlurVerticalQuarter.resolutionScale = 0.25f;
    bloomBlurVerticalQuarter.parameters.blurRadius = 6.0f;
    passes_.push_back(bloomBlurVerticalQuarter);

    PostProcessPass bloomDownsampleEighth{};
    bloomDownsampleEighth.name = "BloomDownsampleEighth";
    bloomDownsampleEighth.inputResource = "BloomQuarterA";
    bloomDownsampleEighth.outputResource = "BloomEighthA";
    bloomDownsampleEighth.pipeline = "BloomDownsample";
    bloomDownsampleEighth.secondaryInputResource = "BloomQuarterA";
    bloomDownsampleEighth.tertiaryInputResource = "BloomQuarterA";
    bloomDownsampleEighth.enabled = true;
    bloomDownsampleEighth.intensity = 1.0f;
    bloomDownsampleEighth.resolutionScale = 0.125f;
    passes_.push_back(bloomDownsampleEighth);

    PostProcessPass bloomBlurHorizontalEighth{};
    bloomBlurHorizontalEighth.name = "BloomBlurHorizontalEighth";
    bloomBlurHorizontalEighth.inputResource = "BloomEighthA";
    bloomBlurHorizontalEighth.outputResource = "BloomEighthB";
    bloomBlurHorizontalEighth.pipeline = "BlurHorizontal";
    bloomBlurHorizontalEighth.secondaryInputResource = "BloomEighthA";
    bloomBlurHorizontalEighth.tertiaryInputResource = "BloomEighthA";
    bloomBlurHorizontalEighth.enabled = true;
    bloomBlurHorizontalEighth.intensity = 1.0f;
    bloomBlurHorizontalEighth.resolutionScale = 0.125f;
    bloomBlurHorizontalEighth.parameters.blurRadius = 4.0f;
    passes_.push_back(bloomBlurHorizontalEighth);

    PostProcessPass bloomBlurVerticalEighth{};
    bloomBlurVerticalEighth.name = "BloomBlurVerticalEighth";
    bloomBlurVerticalEighth.inputResource = "BloomEighthB";
    bloomBlurVerticalEighth.outputResource = "BloomEighthA";
    bloomBlurVerticalEighth.pipeline = "BlurVertical";
    bloomBlurVerticalEighth.secondaryInputResource = "BloomEighthB";
    bloomBlurVerticalEighth.tertiaryInputResource = "BloomEighthB";
    bloomBlurVerticalEighth.enabled = true;
    bloomBlurVerticalEighth.intensity = 1.0f;
    bloomBlurVerticalEighth.resolutionScale = 0.125f;
    bloomBlurVerticalEighth.parameters.blurRadius = 4.0f;
    passes_.push_back(bloomBlurVerticalEighth);

    PostProcessPass bloomUpsampleQuarter{};
    bloomUpsampleQuarter.name = "BloomUpsampleQuarter";
    bloomUpsampleQuarter.inputResource = "BloomEighthA";
    bloomUpsampleQuarter.outputResource = "BloomQuarterComposite";
    bloomUpsampleQuarter.pipeline = "BloomUpsample";
    bloomUpsampleQuarter.secondaryInputResource = "BloomQuarterA";
    bloomUpsampleQuarter.tertiaryInputResource = "BloomQuarterA";
    bloomUpsampleQuarter.enabled = true;
    bloomUpsampleQuarter.intensity = 1.0f;
    bloomUpsampleQuarter.resolutionScale = 0.25f;
    bloomUpsampleQuarter.parameters.bloomUpsampleBlend = 0.85f;
    bloomUpsampleQuarter.parameters.bloomUpsampleSoftKnee = 0.35f;
    passes_.push_back(bloomUpsampleQuarter);

    PostProcessPass bloomUpsampleHalf{};
    bloomUpsampleHalf.name = "BloomUpsampleHalf";
    bloomUpsampleHalf.inputResource = "BloomQuarterComposite";
    bloomUpsampleHalf.outputResource = "BloomComposite";
    bloomUpsampleHalf.pipeline = "BloomUpsample";
    bloomUpsampleHalf.secondaryInputResource = "BloomExtractHalf";
    bloomUpsampleHalf.tertiaryInputResource = "BloomExtractHalf";
    bloomUpsampleHalf.enabled = true;
    bloomUpsampleHalf.intensity = 1.0f;
    bloomUpsampleHalf.resolutionScale = 0.5f;
    bloomUpsampleHalf.parameters.bloomUpsampleBlend = 0.7f;
    bloomUpsampleHalf.parameters.bloomUpsampleSoftKnee = 0.25f;
    passes_.push_back(bloomUpsampleHalf);

    PostProcessPass distortionComposite{};
    distortionComposite.name = "DistortionComposite";
    distortionComposite.inputResource = "VfxAccumulation";
    distortionComposite.outputResource = "PostColorA";
    distortionComposite.pipeline = "DistortionComposite";
    distortionComposite.secondaryInputResource = "SceneColor";
    distortionComposite.tertiaryInputResource = "VfxAccumulation";
    distortionComposite.enabled = true;
    distortionComposite.intensity = 1.0f;
    distortionComposite.resolutionScale = 1.0f;
    distortionComposite.parameters.distortionScale = 0.02f;
    passes_.push_back(distortionComposite);

    PostProcessPass toneMapping{};
    toneMapping.name = "ToneMapping";
    toneMapping.inputResource = "PostColorA";
    toneMapping.outputResource = "PostColorB";
    toneMapping.pipeline = "ToneMapping";
    toneMapping.secondaryInputResource = "PostColorA";
    toneMapping.tertiaryInputResource = "PostColorA";
    toneMapping.enabled = true;
    toneMapping.intensity = 1.0f;
    toneMapping.resolutionScale = 1.0f;
    toneMapping.parameters.toneExposure = 1.0f;
    passes_.push_back(toneMapping);

    PostProcessPass glowComposite{};
    glowComposite.name = "GlowComposite";
    glowComposite.inputResource = "BloomComposite";
    glowComposite.outputResource = "PostColorA";
    glowComposite.pipeline = "GlowComposite";
    glowComposite.secondaryInputResource = "VfxAccumulation";
    glowComposite.tertiaryInputResource = "PostColorB";
    glowComposite.enabled = true;
    glowComposite.intensity = 1.0f;
    glowComposite.resolutionScale = 1.0f;
    glowComposite.parameters.glowWeight = 1.0f;
    glowComposite.parameters.glowTintR = 1.0f;
    glowComposite.parameters.glowTintG = 0.95f;
    glowComposite.parameters.glowTintB = 1.2f;
    passes_.push_back(glowComposite);
}

void PostProcessStack::SetEnabled(const std::string& name, bool enabled) {
    for (PostProcessPass& pass : passes_) {
        if (pass.name == name) {
            pass.enabled = enabled;
            return;
        }
    }
}

void PostProcessStack::SetIntensity(const std::string& name, float intensity) {
    for (PostProcessPass& pass : passes_) {
        if (pass.name == name) {
            pass.intensity = intensity;
            return;
        }
    }
}

bool PostProcessStack::IsEnabled(const std::string& name) const {
    for (const PostProcessPass& pass : passes_) {
        if (pass.name == name) {
            return pass.enabled;
        }
    }
    return false;
}

float PostProcessStack::Intensity(const std::string& name) const {
    for (const PostProcessPass& pass : passes_) {
        if (pass.name == name) {
            return pass.enabled ? pass.intensity : 0.0f;
        }
    }
    return 0.0f;
}

std::string PostProcessStack::FinalOutputResource() const {
    return BuildExecutionPlan().finalOutputResource;
}

PostProcessExecutionPlan PostProcessStack::BuildExecutionPlan() const {
    PostProcessExecutionPlan plan{};
    std::unordered_set<std::string> availableResources = {
        "SceneColor",
        "VfxAccumulation",
    };

    for (const PostProcessPass& pass : passes_) {
        if (!pass.enabled) {
            continue;
        }

        const bool hasPrimaryInput = availableResources.contains(pass.inputResource);
        const bool hasSecondaryInput = availableResources.contains(pass.secondaryInputResource);
        const bool hasTertiaryInput = availableResources.contains(pass.tertiaryInputResource);
        if (!hasPrimaryInput || !hasSecondaryInput || !hasTertiaryInput) {
            continue;
        }

        PostProcessExecutionPass executionPass{};
        executionPass.pass = pass;
        plan.finalOutputResource = pass.outputResource;
        availableResources.insert(pass.outputResource);
        plan.passes.push_back(std::move(executionPass));
    }

    return plan;
}
