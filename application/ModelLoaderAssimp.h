// ModelLoaderAssimp.h
#pragma once
#include <string>

// AppMain.cpp に既にある構造体を使う前提なら forward 宣言はできないので、
// ここでは「AppMain.cpp側で include」して使うのが簡単。
// ただし理想は ModelData/MaterialData/VertexData を別ヘッダに移すこと。

struct ModelData; // AppMain.cpp 側の struct を使うなら、最終的には共通ヘッダ化推奨

ModelData LoadObjFile_Assimp(const std::string& directoryPath,
    const std::string& filename);

// ModelLoaderAssimp.h
//Node ReadNode(aiNode* node);
