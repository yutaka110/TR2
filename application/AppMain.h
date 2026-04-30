#pragma once
#include <Windows.h>

class AppMain {
public:
    bool Initialize(HINSTANCE hInstance);
    int Run();
    void Finalize();

private:
    HINSTANCE hInstance_ = nullptr;
};
