#include "input/Input.h"
#include <cassert>

bool KeyInputDI::Initialize(HINSTANCE hInst, HWND hwnd) {
    hwnd_ = hwnd;

    HRESULT hr = DirectInput8Create(
        hInst, DIRECTINPUT_VERSION, IID_IDirectInput8,
        reinterpret_cast<void**>(&di_), nullptr);
    if (FAILED(hr) || !di_) return false;

    hr = di_->CreateDevice(GUID_SysKeyboard, &kb_, nullptr);
    if (FAILED(hr) || !kb_) return false;

    // 標準キーボードデータフォーマット
    hr = kb_->SetDataFormat(&c_dfDIKeyboard);
    if (FAILED(hr)) return false;

    // 協調レベル：フォアグラウンド / 非独占 / WinKey無効（Alt+Tab等は可）
    hr = kb_->SetCooperativeLevel(
        hwnd_, DISCL_FOREGROUND | DISCL_NONEXCLUSIVE | DISCL_NOWINKEY);
    if (FAILED(hr)) return false;

    // 初回 Acquire
    AcquireIfNeeded();

    // 初期状態クリア
    curr_.fill(0);
    prev_.fill(0);
    return true;
}

void KeyInputDI::Finalize() {
    if (kb_) {
        kb_->Unacquire();
        kb_->Release();
        kb_ = nullptr;
    }
    if (di_) {
        di_->Release();
        di_ = nullptr;
    }
}

bool KeyInputDI::AcquireIfNeeded() {
    if (!kb_) return false;
    HRESULT hr = kb_->Acquire();
    if (SUCCEEDED(hr)) return true;

    // フォーカス喪失等で失敗しても、次回以降のUpdateで再挑戦
    return false;
}

void KeyInputDI::Update() {
    if (!kb_) return;

    // 前フレームを保存
    prev_ = curr_;

    // 取得（ロストしていたら再Acquire）
    HRESULT hr = kb_->GetDeviceState(static_cast<DWORD>(curr_.size()), curr_.data());
    if (FAILED(hr)) {
        // デバイスロスト/未取得なら取り直し
        if (hr == DIERR_INPUTLOST || hr == DIERR_NOTACQUIRED) {
            if (AcquireIfNeeded()) {
                hr = kb_->GetDeviceState(static_cast<DWORD>(curr_.size()), curr_.data());
                if (FAILED(hr)) {
                    // まだ取れない場合はすべて0とみなす
                    curr_.fill(0);
                }
            }
            else {
                curr_.fill(0);
            }
        }
        else {
            // それ以外の失敗も安全側で0
            curr_.fill(0);
        }
    }
}
