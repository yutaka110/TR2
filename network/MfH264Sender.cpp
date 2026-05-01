#include "MfH264Sender.h"
#include "UdpByteStream.h"

#include <thread>
#include <chrono>
#include <iostream>

using namespace Microsoft::WRL;

MfH264Sender::MfH264Sender() {}

MfH264Sender::~MfH264Sender() {
    Finalize();
}

bool MfH264Sender::Initialize(const std::string& destinationIp, unsigned short port) {
    ip_ = destinationIp;
    port_ = port;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "MFStartup failed\n";
        return false;
    }

    // 初期化はフェーズ2で実装
    initialized = true;
    return true;
}

void MfH264Sender::Run() {
    if (!initialized) {
        std::cerr << "MfH264Sender not initialized\n";
        return;
    }

    // 後続のフェーズ2で実装：SourceReader → SinkWriter による送信処理
    std::cout << "Run() called (sending logic to be implemented in phase 2)\n";
}

void MfH264Sender::Finalize() {
    if (sinkWriter) {
        sinkWriter->Finalize();
        sinkWriter.Reset();
    }
    sourceReader.Reset();
    byteStream.Reset();

    MFShutdown();
    std::cout << "MfH264Sender finalized\n";
}
