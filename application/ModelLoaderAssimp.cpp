// ModelLoaderAssimp.cpp
#include "ModelLoaderAssimp.h"

// #2 include の追加（スライド通り）
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include "utils/math/Vector.h"
#include "utils/math/MathUtils.h"
// AppMain.cpp で定義している ModelData / VertexData / Vector2/3/4 を使うなら
// それが見えるヘッダを include する必要がある（プロジェクト側の実体に合わせて差し替えて）
   // Vector2/3/4 がここなら
// #include "YourVertexDataHeader.h"  // VertexData がある場所に合わせて

// 文字列連結（"dir/" が無い場合に備える）
static std::string JoinPath_(const std::string& dir, const std::string& file) {
    if (dir.empty()) return file;
    if (dir.back() == '/' || dir.back() == '\\') return dir + file;
    return dir + "/" + file;
}

Node ReadNode(aiNode* node);

ModelData LoadObjFile_Assimp(const std::string& directoryPath,
    const std::string& filename)
{
    ModelData modelData{};
    modelData.vertices.clear();
    modelData.material.textureFilePath.clear();
    modelData.rootNode.localMatrix = MakeIdentity4x4();

    const std::string filePath = JoinPath_(directoryPath, filename);

    Assimp::Importer importer;

    // スライド方針に合わせた最小セット：
    // - 三角化
    // - UVのV反転
    // - 面の並び順反転（RH→LH相当の対処の一部）
    const unsigned flags =
        aiProcess_Triangulate |
        aiProcess_FlipUVs |
        aiProcess_GenSmoothNormals;

    const aiScene* scene = importer.ReadFile(filePath.c_str(), flags);

    if (!scene || !scene->HasMeshes()) {
        // 課題なら assert でも良い。運用ならログ。
        // assert(false && "Assimp ReadFile failed or no meshes.");
        return modelData;
    }

    // -----------------------------
    // Meshをすべて結合して1つのModelDataに詰める（課題最短）
    // ※複数mesh/複数materialを厳密に分けたい場合は拡張が必要
    // -----------------------------
    for (unsigned meshIndex = 0; meshIndex < scene->mNumMeshes; ++meshIndex) {
        const aiMesh* mesh = scene->mMeshes[meshIndex];
        if (!mesh) continue;

        // スライドの簡易方針：無いものは非対応
        if (!mesh->HasNormals()) continue;
        if (!mesh->HasTextureCoords(0)) continue;

        // -----------------------------
        // face -> indices -> VertexData 展開（非indexed）
        // -----------------------------
        for (unsigned faceIndex = 0; faceIndex < mesh->mNumFaces; ++faceIndex) {
            const aiFace& face = mesh->mFaces[faceIndex];
            if (face.mNumIndices != 3) {
                // Triangulateしてるので基本3のはず。念のためスキップ。
                continue;
            }

            for (unsigned e = 0; e < 3; ++e) {
                const unsigned vi = face.mIndices[e];

                const aiVector3D& p = mesh->mVertices[vi];
                const aiVector3D& n = mesh->mNormals[vi];
                const aiVector3D& uv = mesh->mTextureCoords[0][vi];

                VertexData v{};
                v.position = { p.x, p.y, p.z, 1.0f };
                v.texcoord = { uv.x, uv.y };
                v.normal = { n.x, n.y, n.z };

                // スライド例：左手系化を手動で行うなら X反転
                // （FlipWindingOrderとセット運用）
                /*v.position.x *= -1.0f;
                v.normal.x *= -1.0f;*/

                modelData.vertices.push_back(v);
            }
        }

        // -----------------------------
        // material -> diffuse texture（meshが参照するmaterialを使う）
        // -----------------------------
        if (scene->HasMaterials()) {
            const unsigned matIndex = mesh->mMaterialIndex;
            if (matIndex < scene->mNumMaterials) {
                const aiMaterial* mat = scene->mMaterials[matIndex];
                if (mat) {
                    if (mat->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
                        aiString texPath;
                        if (mat->GetTexture(aiTextureType_DIFFUSE, 0, &texPath) == AI_SUCCESS) {
                            // texPath は相対パスで来ることが多い
                            modelData.material.textureFilePath = JoinPath_(directoryPath, texPath.C_Str());
                        }
                    }
                }
            }
        }
    }
    if (scene->mRootNode != nullptr) {
        modelData.rootNode = ReadNode(scene->mRootNode);
    }

    return modelData;
}

Node ReadNode(aiNode* node) {
    Node result;

    aiMatrix4x4 aiMat = node->mTransformation;
    aiMat.Transpose(); // ★超重要（Assimpは行列レイアウトが違う）

    // aiMatrix → Matrix4x4 にコピー
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            result.localMatrix.m[r][c] = aiMat[r][c];
        }
    }

    result.name = node->mName.C_Str();

    result.children.resize(node->mNumChildren);
    for (uint32_t i = 0; i < node->mNumChildren; i++) {
        result.children[i] = ReadNode(node->mChildren[i]);
    }

    return result;
}
