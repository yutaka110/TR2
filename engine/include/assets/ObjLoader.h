#pragma once
#include <string>
#include <vector>

#include "utils/math/MathUtils.h"
#include "utils/math/Vector.h"

namespace assets {

// MaterialData構造体
struct MaterialData {
	std::string textureFilePath;
};

// ModelData構造体
struct ModelData {
	std::vector<VertexData> vertices;
	MaterialData material;
};


ModelData LoadObjFile(const std::string& directoryPath, const std::string& filename);
MaterialData LoadMaterialTemplateFile(const std::string& directoryPath, const std::string& filename);

} // namespace assets
