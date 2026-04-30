#include "platform/Window.h"
#include <shellscalingapi.h> // SetProcessDpiAwareness
#pragma comment(lib, "Shcore.lib")
#include"utils/ProjectSettings.h"
#if defined(DEVELOP) || defined(_DEBUG)
#include "../../../externals/imgui/imgui.h"
#include "../../../externals/imgui/imgui_impl_win32.h"
#endif
using namespace eng::platform;

//void Window::EnableDpiAwareness() {
//    // Windows 10 以降：システム DPI/Per-monitorV2 に対応
//    // 失敗しても致命ではないので戻り値は無視
//    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
//}

bool Window::RegisterClass() {
    hinst_ = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(WNDCLASSEXW) };
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &Window::WndProcStatic;
    wc.hInstance = hinst_;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"EngWindowClass";
    return RegisterClassExW(&wc) != 0;
}

bool Window::Create(const WindowDesc& desc) {
    if (!RegisterClass()) return false;
    return CreateHwnd(desc);
}

bool Window::CreateHwnd(const WindowDesc& d) {
    // UTF-16 タイトル
    std::wstring title(d.title.begin(), d.title.end());

    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT rc{ 0, 0, LONG(d.width), LONG(d.height) };
    AdjustWindowRect(&rc, style, FALSE);

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
    if (d.center) {
        RECT sr{}; GetClientRect(GetDesktopWindow(), &sr);
        x = (sr.right - w) / 2;
        y = (sr.bottom - h) / 2;
    }

    hwnd_ = CreateWindowExW(
        0, L"EngWindowClass", title.c_str(), style,
        x, y, w, h,
        nullptr, nullptr, hinst_, this /*lpParamにthis*/);

    if (!hwnd_) return false;

    DragAcceptFiles(hwnd_, TRUE); // D&D 受け入れ（任意）
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    width_ = d.width;
    height_ = d.height;
    return true;
}

void Window::Destroy() {
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool Window::PumpMessages() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return true;
}

LRESULT CALLBACK Window::WndProcStatic(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto cs = reinterpret_cast<CREATESTRUCTW*>(l);
        auto that = static_cast<Window*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(that));
        SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&Window::WndProcStatic));
        that->hwnd_ = h;
    }
    auto* that = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (that) return that->WndProc(h, m, w, l);
    return DefWindowProcW(h, m, w, l);
}

LRESULT Window::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {

    // Window::WndProc 冒頭あたり
#if defined(DEVELOP) || defined(_DEBUG)
    extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
#endif


    switch (m) {
    case WM_SIZE: {
        width_ = LOWORD(l);
        height_ = HIWORD(l);
        if (onResize_) onResize_(width_, height_);
        return 0;
    }
    case WM_DROPFILES: {
        if (onDrop_) {
            HDROP drop = (HDROP)w;
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(drop, 0, path, MAX_PATH)) {
                onDrop_(path);
            }
            DragFinish(drop);
        }
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
