#include "KeyInput.h"
#include <cassert>

void KeyInput::Initialize(HWND hwnd, WNDCLASS wc) {
	// DirectInputの初期化
	IDirectInput8* directInput = nullptr;
	HRESULT result = DirectInput8Create(
		wc.hInstance, DIRECTINPUT_VERSION, IID_IDirectInput8,
		(void**)&directInput, nullptr);
	assert(SUCCEEDED(result));

	//キーボードデバイスの生成
	IDirectInputDevice8* keyboard = nullptr;
	result = directInput->CreateDevice(GUID_SysKeyboard, &keyboard, NULL);
	assert(SUCCEEDED(result));

	//入力データ形式のセット
	result = keyboard->SetDataFormat(&c_dfDIKeyboard); // 標準形式
	assert(SUCCEEDED(result));

	//排他制御レベルのセット
	result = keyboard->SetCooperativeLevel(
		hwnd,
		DISCL_FOREGROUND | DISCL_NONEXCLUSIVE | DISCL_NOWINKEY
	);
	assert(SUCCEEDED(result));
}

void KeyInput::Update() {
    keyboard_->Acquire();
    keyboard_->GetDeviceState(sizeof(key_), key_);
}

bool KeyInput::IsKeyPressed(BYTE dikCode) const {
    return (key_[dikCode] & 0x80) != 0;
}