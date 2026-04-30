#pragma once

#include <Windows.h>
#include "platform/Window.h"

class AppBootstrap {
public:
    bool Initialize(HINSTANCE hInstance);
    void Shutdown();

    HINSTANCE GetInstance() const { return hInstance_; }
    HWND Handle() const { return window_.Handle(); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

private:
    HINSTANCE hInstance_ = nullptr;
    eng::platform::Window window_;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool coInitialized_ = false;
};
