#pragma once

struct AppFrameGraphBuildContext;

class AppVfxRenderPipeline {
public:
    void RegisterPasses(const AppFrameGraphBuildContext& context) const;
};
