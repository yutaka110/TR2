#pragma once

struct AppFrameGraphBuildContext;

class AppPostProcessPipeline {
public:
    void RegisterPasses(const AppFrameGraphBuildContext& context) const;
};
