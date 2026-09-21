// G0 diagnostic: exercise the existing encoder with known NV12 input.
// AU output order is recorded; it is NOT assumed to equal the input-call index.
#include "../network/H264Encoder.h"
#include <dxgi1_6.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "dxgi.lib")

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "usage: codec_probe auto|software\n"; return 2; }
    const std::string mode = argv[1];
    if (mode != "auto" && mode != "software") return 2;
    SetEnvironmentVariableA("RNVP_H264_ENCODER", mode.c_str());
    SetEnvironmentVariableA("RNVP_H264_GOP", "30");
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 3;
    if (FAILED(MFStartup(MF_VERSION))) { CoUninitialize(); return 4; }
    std::filesystem::create_directories("aus");
    std::ofstream system("adapters.txt");
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT index = 0;; ++index) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            if (!adapter) break;
            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))) {
                char name[512]{};
                WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
                LARGE_INTEGER version{};
                const HRESULT versionHr = adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &version);
                system << index << ',' << name << ',' << desc.DedicatedVideoMemory
                       << ',' << desc.Flags << ',' << std::hex << versionHr << ','
                       << version.QuadPart << std::dec << '\n';
            }
        }
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) system << "physical_memory_bytes," << memory.ullTotalPhys << '\n';
    H264Encoder encoder;
    if (!encoder.Initialize(640, 360, 1500000, 30)) {
        MFShutdown(); CoUninitialize(); return 5;
    }
    std::ofstream trace("encoder_outputs.csv");
    trace << "output_index,input_call_index,phase,bytes,hardware,async,call_ms,elapsed_ms\n";
    std::ofstream requests("idr_requests.csv");
    requests << "before_input_index,outputs_already_observed,elapsed_ms\n";
    const auto start = std::chrono::steady_clock::now();
    const auto elapsed = [&]() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count(); };
    unsigned outputCount = 0, failedCalls = 0, pollsWithoutEventOrWithError = 0;
    const auto save = [&](const std::vector<BYTE>& au, unsigned input, const char* phase) {
        if (au.empty()) return;
        char filename[64]{};
        sprintf_s(filename, "aus/%04u.h264", outputCount);
        std::ofstream file(filename, std::ios::binary);
        file.write(reinterpret_cast<const char*>(au.data()), static_cast<std::streamsize>(au.size()));
        const auto timing = encoder.GetLastFrameTiming();
        trace << outputCount++ << ',' << input << ',' << phase << ',' << au.size() << ','
              << timing.hardware << ',' << timing.async << ',' << timing.callMs << ',' << elapsed() << '\n';
    };
    std::vector<BYTE> nv12(640 * 360 * 3 / 2, 128);
    for (unsigned input = 0; input < 90; ++input) {
        std::this_thread::sleep_until(start + std::chrono::microseconds(input * 1000000ull / 30));
        if (input == 45) {
            requests << input << ',' << outputCount << ',' << elapsed() << '\n';
            encoder.RequestKeyFrame();
        }
        for (unsigned y = 0; y < 360; ++y)
            for (unsigned x = 0; x < 640; ++x)
                nv12[y * 640 + x] = static_cast<BYTE>(16 + ((x + input * 3 + y / 2) % 220));
        std::vector<BYTE> au;
        if (!encoder.EncodeFrame(nv12.data(), static_cast<UINT>(nv12.size()), au)) ++failedCalls;
        save(au, input, "encode");
        // Collect delayed output explicitly, without assigning it to this input's identity.
        if (encoder.IsAsyncHardware()) {
            for (unsigned attempt = 0; attempt < 5; ++attempt) {
                au.clear();
                // The existing API returns false on a no-event timeout as well
                // as an error; keep that observation separate from input failure.
                if (!encoder.DrainOutput(au, 10)) ++pollsWithoutEventOrWithError;
                save(au, input, "drain");
                if (outputCount >= input + 1) break;
            }
        }
    }
    std::vector<std::vector<BYTE>> tail;
    if (!encoder.FlushDelayedFrames(tail)) ++failedCalls;
    for (const auto& au : tail) save(au, 90, "flush");
    encoder.Shutdown();
    MFShutdown();
    CoUninitialize();
    std::cout << "outputs=" << outputCount << " failed_calls=" << failedCalls
              << " empty_or_failed_polls=" << pollsWithoutEventOrWithError << '\n';
    return outputCount == 90 && failedCalls == 0 ? 0 : 6;
}
