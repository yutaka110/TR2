
#include <Windows.h>
#include "application/AppMain.h"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    AppMain app;
    if (!app.Initialize(hInstance)) { return -1; }
    return app.Run();
}
