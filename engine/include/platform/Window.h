#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <windows.h>

namespace eng::platform {

    struct WindowDesc {
        std::u16string title = u"Engine";
        uint32_t width = 1280;
        uint32_t height = 720;
        bool     center = true;
    };

    class Window {
    public:
        using ResizeCallback = std::function<void(uint32_t w, uint32_t h)>;
        using DropFilesCallback = std::function<void(const std::wstring&)>;

        bool Create(const WindowDesc& desc);
        void Destroy();

        // メッセージを1フレーム分処理。true=継続 / false=終了（WM_QUIT）
        bool PumpMessages();

        HWND Handle() const noexcept { return hwnd_; }
        uint32_t Width() const noexcept { return width_; }
        uint32_t Height() const noexcept { return height_; }

        void SetResizeCallback(ResizeCallback cb) { onResize_ = std::move(cb); }
        void SetDropCallback(DropFilesCallback cb) { onDrop_ = std::move(cb); }

       

    private:
        static LRESULT CALLBACK WndProcStatic(HWND, UINT, WPARAM, LPARAM);
        LRESULT WndProc(HWND, UINT, WPARAM, LPARAM);

        bool RegisterClass();
        bool CreateHwnd(const WindowDesc&);

    private:
        HWND hwnd_ = nullptr;
        HINSTANCE hinst_ = nullptr;
        uint32_t width_ = 0, height_ = 0;
        ResizeCallback onResize_;
        DropFilesCallback onDrop_;
    };

} // namespace eng::platform
