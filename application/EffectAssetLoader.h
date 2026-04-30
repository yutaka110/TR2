#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "EffectSystem.h"

struct LoadedEffectAsset {
    EffectAsset asset;
    std::filesystem::file_time_type lastWriteTime{};
    std::filesystem::path path;
};

class EffectAssetLoader {
public:
    std::vector<LoadedEffectAsset> LoadDirectory(const std::filesystem::path& directory) const;
    bool LoadFile(const std::filesystem::path& path, LoadedEffectAsset& outAsset) const;

private:
    static bool ApplyKeyValue(EffectAsset& asset, const std::string& key, const std::string& value);
};
