#pragma once
#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <array>
#include <windows.h>

class KeyInputDI {
public:
    // 初期化 / 破棄
    bool Initialize(HINSTANCE hInst, HWND hwnd);
    void Finalize();

    // 毎フレーム更新（押下状態の取得 & ロスト時再取得）
    void Update();

    // クエリ
    bool IsPressed(int dik) const { return curr_[dik] != 0; }
    bool IsTriggered(int dik) const { return curr_[dik] && !prev_[dik]; }
    bool IsReleased(int dik) const { return !curr_[dik] && prev_[dik]; }

    // 生の配列（必要なら参照用）
    const unsigned char* Current() const { return curr_.data(); }
    const unsigned char* Previous() const { return prev_.data(); }

private:
    bool AcquireIfNeeded();

    IDirectInput8* di_ = nullptr;
    IDirectInputDevice8* kb_ = nullptr;
    HWND                 hwnd_ = nullptr;

    std::array<unsigned char, 256> curr_{};
    std::array<unsigned char, 256> prev_{};
};
