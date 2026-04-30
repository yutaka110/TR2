#include "AppBootstrap.h"
#include "AppRuntimeUtils.h"

#include <filesystem>

bool AppBootstrap::Initialize(HINSTANCE hInstance) {
    hInstance_ = hInstance;
    if (!hInstance_) {
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) {
        coInitialized_ = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    SetUnhandledExceptionFilter(ExportDump);
    std::filesystem::create_directory("logs");

    eng::platform::WindowDesc desc;
    desc.title = u"LE2B_17_タケイ_ユタカ";
    desc.width = 1280;
    desc.height = 720;

    if (!window_.Create(desc)) {
        Shutdown();
        return false;
    }

    width_ = desc.width;
    height_ = desc.height;
    return true;
}

void AppBootstrap::Shutdown() {
    window_.Destroy();
    width_ = 0;
    height_ = 0;
    if (coInitialized_) {
        CoUninitialize();
        coInitialized_ = false;
    }
    hInstance_ = nullptr;
}
