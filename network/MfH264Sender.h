#pragma once
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <string>

class MfH264Sender {
public:
    MfH264Sender();
    ~MfH264Sender();

    bool Initialize(const std::string& destinationIp, unsigned short port);
    void Run();         // メインループ（映像取得→送信）
    void Finalize();    // 終了処理・リソース解放

private:
    Microsoft::WRL::ComPtr<IMFSourceReader> sourceReader;
    Microsoft::WRL::ComPtr<IMFSinkWriter> sinkWriter;
    Microsoft::WRL::ComPtr<IMFByteStream> byteStream;

    DWORD streamIndex = 0;
    LONGLONG rtStart = 0;
    bool initialized = false;

    std::string ip_;
    unsigned short port_;
};
