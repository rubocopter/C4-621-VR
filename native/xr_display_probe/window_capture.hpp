#pragma once
#include <memory>
#include <vector>
#include <cstdint>
#include <windows.h>
#include <dxgi1_4.h>

// Captures only a window owned by the selected AC6 process, without input.
class WindowCapture {
public:
    WindowCapture(DWORD game_pid, IDXGIAdapter1* adapter);
    ~WindowCapture();
    bool Next(std::vector<std::uint8_t>& rgba, unsigned width, unsigned height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
