#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <dxcapi.h> // DXC

namespace ge3::core {

    using Microsoft::WRL::ComPtr;

    // DXC をラップするコンパイラクラス
    class ShaderCompiler {
    public:
        ShaderCompiler() = default;
        ~ShaderCompiler() { Finalize(); }

        // DXC 初期化（成功:true, 失敗:false）
        bool Initialize();
        void Finalize();

        // HLSLファイルをコンパイルするユーティリティ
        // - filePath:    L"Resources/shader/xxx.hlsl"
        // - entryPoint:  L"VSMain" や L"PSMain"
        // - target:      L"vs_6_0" / L"ps_6_0" / L"cs_6_0" など
        ComPtr<IDxcBlob> CompileFromFile(
            const std::wstring& filePath,
            const std::wstring& entryPoint,
            const std::wstring& targetProfile,
            const std::vector<LPCWSTR>& extraArgs = {}); // -O3 などを追加したいとき用

        // 直近の HRESULT を取得（失敗調査用）
        HRESULT LastError() const { return lastError_; }

    private:
        ComPtr<IDxcUtils>         utils_;
        ComPtr<IDxcCompiler3>     compiler_;
        ComPtr<IDxcIncludeHandler> includeHandler_;
        HRESULT lastError_ = S_OK;
    };

} // namespace ge3::core
