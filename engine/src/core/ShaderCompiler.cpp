#include "core/ShaderCompiler.h"
#include <windows.h> // OutputDebugStringW
#include <filesystem>

using namespace ge3::core;

static void DebugPrint(const wchar_t* msg) {
#if defined(_DEBUG)
    OutputDebugStringW(msg);
#endif
}

bool ShaderCompiler::Initialize() {
    lastError_ = S_OK;

    if (utils_ || compiler_) {
        // 既に初期化済み
        return true;
    }

    lastError_ = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&utils_));
    if (FAILED(lastError_)) {
        DebugPrint(L"[ShaderCompiler] DxcCreateInstance(DxcUtils) failed\n");
        return false;
    }

    lastError_ = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler_));
    if (FAILED(lastError_)) {
        DebugPrint(L"[ShaderCompiler] DxcCreateInstance(DxcCompiler) failed\n");
        utils_.Reset();
        return false;
    }

    lastError_ = utils_->CreateDefaultIncludeHandler(&includeHandler_);
    if (FAILED(lastError_)) {
        DebugPrint(L"[ShaderCompiler] CreateDefaultIncludeHandler failed\n");
        compiler_.Reset();
        utils_.Reset();
        return false;
    }

    DebugPrint(L"[ShaderCompiler] Initialize OK\n");
    return true;
}

void ShaderCompiler::Finalize() {
    includeHandler_.Reset();
    compiler_.Reset();
    utils_.Reset();
}

Microsoft::WRL::ComPtr<IDxcBlob> ShaderCompiler::CompileFromFile(
    const std::wstring& filePath,
    const std::wstring& entryPoint,
    const std::wstring& targetProfile,
    const std::vector<LPCWSTR>& extraArgs)
{
    ComPtr<IDxcBlob> resultBlob;
    lastError_ = S_OK;

    if (!utils_ || !compiler_) {
        DebugPrint(L"[ShaderCompiler] CompileFromFile called before Initialize\n");
        lastError_ = E_FAIL;
        return resultBlob;
    }

    if (!std::filesystem::exists(filePath)) {
        DebugPrint(L"[ShaderCompiler] file not found: ");
        DebugPrint(filePath.c_str());
        DebugPrint(L"\n");
        lastError_ = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        return resultBlob;
    }

    // ファイル読み込み → DxcBuffer 準備
    ComPtr<IDxcBlobEncoding> sourceBlob;
    lastError_ = utils_->LoadFile(filePath.c_str(), nullptr, &sourceBlob);
    if (FAILED(lastError_)) {
        DebugPrint(L"[ShaderCompiler] LoadFile failed\n");
        return resultBlob;
    }

    DxcBuffer sourceBuffer{};
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_ACP; // 既に UTF-8 / UTF-16 に対応済みの想定

    // 引数を組み立て
    std::vector<LPCWSTR> args;
    args.reserve(8 + extraArgs.size());

    args.push_back(filePath.c_str());       // ソース名
    args.push_back(L"-E");                  // エントリポイント
    args.push_back(entryPoint.c_str());
    args.push_back(L"-T");                  // ターゲット
    args.push_back(targetProfile.c_str());

    // デバッグ情報（必要に応じて調整）
#if _DEBUG
    args.push_back(L"-Zi");                // デバッグ情報
    args.push_back(L"-Qembed_debug");
    args.push_back(L"-Od");                // 最適化オフ
#else
    args.push_back(L"-O3");                // Release 時は最適化
#endif
    // ★ ここを追加：行優先レイアウトを強制
    args.push_back(L"-Zpr");
    for (auto a : extraArgs) {
        args.push_back(a);
    }

    ComPtr<IDxcResult> result;
    lastError_ = compiler_->Compile(
        &sourceBuffer,
        args.data(),
        (UINT)args.size(),
        includeHandler_.Get(),
        IID_PPV_ARGS(&result));

    if (FAILED(lastError_) || !result) {
        DebugPrint(L"[ShaderCompiler] Compile() failed\n");
        return ComPtr<IDxcBlob>();
    }

    // エラー出力があればログに出す
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) {
        DebugPrint(L"[ShaderCompiler] CompileFromFile errors:\n");
        OutputDebugStringA(errors->GetStringPointer());
        DebugPrint(L"\n");
    }

    HRESULT status = S_OK;
    result->GetStatus(&status);
    if (FAILED(status)) {
        DebugPrint(L"[ShaderCompiler] CompileFromFile status FAILED\n");
        lastError_ = status;
        return ComPtr<IDxcBlob>();
    }

    // 実バイナリを取り出す
    ComPtr<IDxcBlob> shader;
    lastError_ = result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shader), nullptr);
    if (FAILED(lastError_) || !shader) {
        DebugPrint(L"[ShaderCompiler] GetOutput(DXC_OUT_OBJECT) failed\n");
        return ComPtr<IDxcBlob>();
    }

    return shader;
}
