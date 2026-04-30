#pragma once
#include <Windows.h>

// 失敗なら例外（std::runtime_error）を投げる
void ThrowIfFailed(HRESULT hr);
