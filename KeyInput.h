#pragma once
#define DIRECTINPUT_VERSION  0x0800  // DirectInputのバージョン指定
#include <dinput.h>

#pragma comment(lib, "dinput8.lib")
#pragma comment(lib, "dxguid.lib")

class KeyInput {
public:
    void Initialize( HWND hwnd, WNDCLASS wc);
    void Update();

    bool IsKeyPressed(BYTE dikCode) const;

private:
    IDirectInputDevice8* keyboard_ = nullptr;
    BYTE key_[256] = {};
};
