#include "H264Encoder.h"
#include <mfapi.h>
#include <mferror.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wmcodecdsp.h>
#include <codecapi.h>
#include <comdef.h>
#include <vector>
#include <cassert>

#include <initguid.h> 

// SPS/PPS を挿入するかどうか（1 = 各IDRの先頭に追加）
DEFINE_GUID(CODECAPI_AVEncH264SPSPPSInsertion,
    0x6827f7f2, 0x52e5, 0x4d41, 0xb5, 0x1c, 0x42, 0xc2, 0xb6, 0x93, 0x23, 0x46);

// Media Foundation の SPS/PPS 挿入モード属性（IDRごと or 最初だけ）
DEFINE_GUID(MF_VIDEO_ENCODER_HEADER_INSERTION_MODE,
    0x5295e444, 0x1f7a, 0x4f44, 0xa0, 0xa3, 0x7f, 0x7e, 0x0b, 0x66, 0x95, 0x88);

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")     // CLSIDなどの定義
#pragma comment(lib, "wmcodecdspuuid.lib") // H.264 encoder MFT

using Microsoft::WRL::ComPtr;

bool H264Encoder::Initialize(UINT32 width, UINT32 height, UINT32 bitrate, UINT32 fps) {
    width_ = width;
    height_ = height;
    fps_ = fps;
    frameCount_ = 0;

    

    // MFT インスタンス生成
    HRESULT hr = CoCreateInstance(
        CLSID_CMSH264EncoderMFT, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&encoder_));
    if (FAILED(hr)) return false;

    
    // --- 出力タイプを H.264 に設定 ---
    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) return false;

    hr = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return false;
    hr = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (FAILED(hr)) return false;
   /* hr = outputType->SetUINT32(MF_MT_MPEG_SEQUENCE_HEADER, TRUE);
    if (FAILED(hr)) return false;*/
    hr = outputType->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
    if (FAILED(hr)) return false;
    hr = outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return false;
    hr = outputType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);
    if (FAILED(hr)) return false;


    // 🔽 ★追加：SPS/PPS を出力に含める
    hr = outputType->SetUINT32(CODECAPI_AVEncH264SPSPPSInsertion, TRUE);
    if (FAILED(hr)) return false;

    // 🔽 ★追加：全キーフレームに SPS/PPS を入れる
    hr = outputType->SetUINT32(MF_VIDEO_ENCODER_HEADER_INSERTION_MODE, 1);
    if (FAILED(hr)) return false;
   
    hr = encoder_->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) return false;

    // --- 入力タイプを NV12 に設定 ---
    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;

    // 1) Major Type
    hr = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr)) return false;

    // 2) Subtype (YUV)
    hr = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    if (FAILED(hr)) return false;

    // 3) 解像度・フレームレート
    hr = MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
    if (FAILED(hr)) return false;
    hr = MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, fps, 1);
    if (FAILED(hr)) return false;

    // 4) インタレースモード
    hr = inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (FAILED(hr)) return false;

    // 5) デフォルトストライド（行バイト長）
    //    NV12 のストライドは width そのまま
    hr = inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    if (FAILED(hr)) return false;

    // 6) 固定サイズサンプル & 独立サンプル
    hr = inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    if (FAILED(hr)) return false;
    hr = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    if (FAILED(hr)) return false;

    // 7) サンプルあたりのバッファサイズ
    hr = inputType->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 3 / 2);
    if (FAILED(hr)) return false;

   

    GUID subtype = { 0 };
    inputType->GetGUID(MF_MT_SUBTYPE, &subtype);

    LPOLESTR guidStr = nullptr;
    HRESULT hr2 = StringFromCLSID(subtype, &guidStr);
    if (SUCCEEDED(hr2)) {
        OutputDebugStringW(L"[DEBUG] Encoder InputType Subtype: ");
        OutputDebugStringW(guidStr);
        OutputDebugStringW(L"\n");
        CoTaskMemFree(guidStr);  // メモリ解放
    }

    // 8) 入力タイプを MFT に設定
    hr = encoder_->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        OutputDebugStringA("SetInputType failed. 対応フォーマットを列挙します...\n");
        return false;
    }

    // ストリーミング開始通知
   // encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    return true;
}


//bool H264Encoder::EncodeFrame(const BYTE* rgbData, UINT dataSize, std::vector<BYTE>& outH264Data) {
//    if (!encoder_) return false;
//
//    // 入力サンプル作成
//    ComPtr<IMFSample> sample;
//    MFCreateSample(&sample);
//
//   /* ComPtr<IMFMediaBuffer> buffer;
//    MFT_OUTPUT_STREAM_INFO info;
//    encoder_->GetOutputStreamInfo(0, &info);
//    MFCreateMemoryBuffer(info.cbSize, &buffer);*/
//
//
//    /*ComPtr<IMFMediaBuffer> buffer;
//    MFCreateMemoryBuffer(dataSize, &buffer);*/
//
//    ComPtr<IMFMediaBuffer> buffer;
//        // ここを GetInputStreamInfo に変更
//    MFT_INPUT_STREAM_INFO inInfo;
//    HRESULT hr = encoder_->GetInputStreamInfo(0, &inInfo);
//    if (FAILED(hr)) return false;
//    hr = MFCreateMemoryBuffer(inInfo.cbSize, &buffer);
//    if (FAILED(hr)) return false;
//
//    BYTE* dest = nullptr;
//    DWORD maxLen = 0;
//    buffer->Lock(&dest, &maxLen, nullptr);
//    memcpy(dest, rgbData, dataSize);
//    buffer->Unlock();
//    buffer->SetCurrentLength(dataSize);
//
//    sample->AddBuffer(buffer.Get());
//    sample->SetSampleTime(frameCount_ * 10000000 / fps_);
//    sample->SetSampleDuration(10000000 / fps_);
//    frameCount_++;
//
//    hr = encoder_->ProcessInput(0, sample.Get(), 0);
//    if (FAILED(hr)) return false;
//
//    // 出力の取得
//    MFT_OUTPUT_DATA_BUFFER outputData = {};
//    DWORD status = 0;
//    ComPtr<IMFSample> outputSample;
//    MFCreateSample(&outputSample);
//
//    //ComPtr<IMFMediaBuffer> outBuffer;
//    //MFCreateMemoryBuffer(1024 * 1024, &outBuffer); // 1MB
//
//    ComPtr<IMFMediaBuffer> outBuffer;
//        // ここで出力ストリーム情報から適切なサイズを取得
//    MFT_OUTPUT_STREAM_INFO outInfo;
//    hr = encoder_->GetOutputStreamInfo(0, &outInfo);
//    if (FAILED(hr)) return false;
//    hr = MFCreateMemoryBuffer(outInfo.cbSize, &outBuffer);
//    if (FAILED(hr)) return false;
//
//    outputSample->AddBuffer(outBuffer.Get());
//    outputData.pSample = outputSample.Get();
//    outputData.dwStreamID = 0;
//
//    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);
//    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
//        return true; // データが出力されないが、エラーではない
//    }
//    else if (FAILED(hr)) {
//        return false;
//    }
//
//    // バッファからデータを取り出す
//    BYTE* data = nullptr;
//    DWORD len = 0;
//    outBuffer->Lock(&data, nullptr, &len);
//    outH264Data.assign(data, data + len);
//    outBuffer->Unlock();
//
//    // 最初の数フレームでSPS/PPSを抽出
//    if (spsPpsBuffer_.empty()) {
//        const uint8_t* ptr = data;
//        const uint8_t* end = data + len;
//        while (ptr + 4 < end) {
//            if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x00 && ptr[3] == 0x01) {
//                uint8_t nalType = ptr[4] & 0x1F;
//                if (nalType == 7 || nalType == 8) {  // SPSまたはPPS
//                    const uint8_t* next = ptr + 4;
//                    while (next + 4 < end && !(next[0] == 0x00 && next[1] == 0x00 && next[2] == 0x00 && next[3] == 0x01)) {
//                        ++next;
//                    }
//                    spsPpsBuffer_.insert(spsPpsBuffer_.end(), ptr, next);
//                    ptr = next;
//                }
//                else {
//                    ptr += 4;
//                }
//            }
//            else {
//                ++ptr;
//            }
//        }
//    }
//
//
//    return true;
//}

bool H264Encoder::EncodeFrame(const BYTE* rgbData, UINT dataSize, std::vector<BYTE>& outH264Data) {
    if (!encoder_) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] Encoder is not initialized.\n");
#endif
        return false;
    }

    HRESULT hr;

    // 入力サンプルの作成
    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] MFCreateSample failed\n");
#endif
        return false;
    }

    MFT_INPUT_STREAM_INFO inInfo;
    hr = encoder_->GetInputStreamInfo(0, &inInfo);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] GetInputStreamInfo failed\n");
#endif
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(inInfo.cbSize, &buffer);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] MFCreateMemoryBuffer (input) failed\n");
#endif
        return false;
    }

    BYTE* dest = nullptr;
    DWORD maxLen = 0;
    hr = buffer->Lock(&dest, &maxLen, nullptr);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] Lock input buffer failed\n");
#endif
        return false;
    }

    memcpy(dest, rgbData, dataSize);
    buffer->Unlock();
    buffer->SetCurrentLength(dataSize);

    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] AddBuffer to input sample failed\n");
#endif
        return false;
    }

    sample->SetSampleTime(frameCount_ * 10000000 / fps_);
    sample->SetSampleDuration(10000000 / fps_);
    frameCount_++;

    hr = encoder_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] ProcessInput failed\n");
#endif
        return false;
    }

    // 出力取得
    ComPtr<IMFSample> outputSample;
    hr = MFCreateSample(&outputSample);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] MFCreateSample (output) failed\n");
#endif
        return false;
    }

    MFT_OUTPUT_STREAM_INFO outInfo;
    hr = encoder_->GetOutputStreamInfo(0, &outInfo);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] GetOutputStreamInfo failed\n");
#endif
        return false;
    }

    ComPtr<IMFMediaBuffer> outBuffer;
    hr = MFCreateMemoryBuffer(outInfo.cbSize, &outBuffer);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] MFCreateMemoryBuffer (output) failed\n");
#endif
        return false;
    }

    hr = outputSample->AddBuffer(outBuffer.Get());
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] AddBuffer to output sample failed\n");
#endif
        return false;
    }

    MFT_OUTPUT_DATA_BUFFER outputData = {};
    outputData.pSample = outputSample.Get();
   // outputData.dwStreamID = 0;
    DWORD status = 0;

    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
#ifdef _DEBUG
        OutputDebugStringA("[INFO] ProcessOutput: NEED_MORE_INPUT (no frame output yet)\n");
#endif
        return true;
    }
    else if (FAILED(hr)) {
#ifdef _DEBUG
        char buf[128];
        sprintf_s(buf, "[ERROR] ProcessOutput failed: 0x%08X\n", hr);
        OutputDebugStringA(buf);
#endif
        return false;
    }

    BYTE* data = nullptr;
    DWORD len = 0;
    hr = outBuffer->Lock(&data, nullptr, &len);
    if (FAILED(hr)) {
#ifdef _DEBUG
        OutputDebugStringA("[ERROR] Lock output buffer failed\n");
#endif
        return false;
    }

    outH264Data.assign(data, data + len);
    outBuffer->Unlock();

    // SPS/PPS 抽出
    if (spsPpsBuffer_.empty()) {
        const uint8_t* ptr = data;
        const uint8_t* end = data + len;
        while (ptr + 4 < end) {
            if (ptr[0] == 0x00 && ptr[1] == 0x00 && ptr[2] == 0x00 && ptr[3] == 0x01) {
                uint8_t nalType = ptr[4] & 0x1F;
                if (nalType == 7 || nalType == 8) {  // SPSまたはPPS
                    const uint8_t* next = ptr + 4;
                    while (next + 4 < end && !(next[0] == 0x00 && next[1] == 0x00 && next[2] == 0x00 && next[3] == 0x01)) {
                        ++next;
                    }
                    spsPpsBuffer_.insert(spsPpsBuffer_.end(), ptr, next);
                    ptr = next;
                }
                else {
                    ptr += 4;
                }
            }
            else {
                ++ptr;
            }
        }
#ifdef _DEBUG
        OutputDebugStringA("[INFO] SPS/PPS extracted\n");
#endif
    }

    return true;
}


void H264Encoder::Shutdown() {
    if (encoder_) {
        encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        encoder_.Reset();
    }
    MFShutdown();
    /*CoUninitialize();*/

}

std::vector<uint8_t> H264Encoder::GetSpsPps() const {
    return spsPpsBuffer_;
}

//bool H264Encoder::EncodeSample(IMFSample* inputSample, std::vector<uint8_t>& outData) {
//    if (!encoder_) return false;
//
//    HRESULT hr = encoder_->ProcessInput(0, inputSample, 0);
//    if (FAILED(hr)) {
//        OutputDebugStringA("[ERROR] ProcessInput failed\n");
//        return false;
//    }
//
//    // 出力取得の試行
//    MFT_OUTPUT_STREAM_INFO streamInfo = {};
//    hr = encoder_->GetOutputStreamInfo(0, &streamInfo);
//    if (FAILED(hr)) {
//        OutputDebugStringA("[ERROR] GetOutputStreamInfo failed\n");
//        return false;
//    }
//
//    ComPtr<IMFMediaBuffer> outputBuffer;
//    hr = MFCreateMemoryBuffer(streamInfo.cbSize, &outputBuffer);
//    if (FAILED(hr)) {
//        OutputDebugStringA("[ERROR] MFCreateMemoryBuffer failed\n");
//        return false;
//    }
//
//    ComPtr<IMFSample> outputSample;
//    MFCreateSample(&outputSample);
//    outputSample->AddBuffer(outputBuffer.Get());
//
//    MFT_OUTPUT_DATA_BUFFER outputData = {};
//    outputData.pSample = outputSample.Get();
//    DWORD status = 0;
//
//    //if (!outputSample || !outputBuffer) {
//    //    OutputDebugStringA("[ERROR] outputSample or outputBuffer is null\n");
//    //    return false;
//    //}
//
//
//    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);
//
//    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
//        OutputDebugStringA("[INFO] NEED_MORE_INPUT (正常、出力なし)\n");
//        return true; // 正常だが出力はまだない
//    }
//    else if (FAILED(hr)) {
//        char buf[128];
//        sprintf_s(buf, "[ERROR] ProcessOutput failed: 0x%08X\n", hr);
//        OutputDebugStringA(buf);
//        return false;
//    }
//
//    // 出力がある場合のみ outData に格納
//    BYTE* pData = nullptr;
//    DWORD maxLen = 0, curLen = 0;
//    hr = outputBuffer->Lock(&pData, &maxLen, &curLen);
//    if (SUCCEEDED(hr)) {
//        outData.assign(pData, pData + curLen);
//        outputBuffer->Unlock();
//    }
//    else {
//        OutputDebugStringA("[ERROR] Failed to lock output buffer\n");
//        return false;
//    }
//
//    return true;
//}

bool H264Encoder::EncodeSample(IMFSample* inputSample, std::vector<uint8_t>& outData) {
    if (!encoder_) return false;

    HRESULT hr = encoder_->ProcessInput(0, inputSample, 0);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] ProcessInput failed\n");
        return false;
    }

    // 出力取得の試行
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    hr = encoder_->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] GetOutputStreamInfo failed\n");
        return false;
    }

    ComPtr<IMFMediaBuffer> outputBuffer;
    hr = MFCreateMemoryBuffer(streamInfo.cbSize, &outputBuffer);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFCreateMemoryBuffer failed\n");
        return false;
    }

    ComPtr<IMFSample> outputSample;
    MFCreateSample(&outputSample);
    outputSample->AddBuffer(outputBuffer.Get());

    MFT_OUTPUT_DATA_BUFFER outputData = {};
    outputData.pSample = outputSample.Get();
    DWORD status = 0;

    hr = encoder_->ProcessOutput(0, 1, &outputData, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        OutputDebugStringA("[INFO] NEED_MORE_INPUT (正常、出力なし)\n");
        return true; // 正常だが出力はまだない
    }
    else if (FAILED(hr)) {
        char buf[128];
        sprintf_s(buf, "[ERROR] ProcessOutput failed: 0x%08X\n", hr);
        OutputDebugStringA(buf);
        return false;
    }

    // 出力がある場合のみ outData に格納
    BYTE* pData = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = outputBuffer->Lock(&pData, &maxLen, &curLen);
    if (SUCCEEDED(hr)) {
        outData.assign(pData, pData + curLen);
        outputBuffer->Unlock();
    }
    else {
        OutputDebugStringA("[ERROR] Failed to lock output buffer\n");
        return false;
    }

//    // 🔧 毎回 SPS/PPS を先頭に追加
//    if (!spsPpsBuffer_.empty()) {
//        outData.insert(outData.begin(), spsPpsBuffer_.begin(), spsPpsBuffer_.end());
//#ifdef _DEBUG
//        OutputDebugStringA("[DEBUG] SPS/PPS prepended to encoded frame\n");
//
//        {
//            char log[128];
//            sprintf_s(log, "[DEBUG] spsPpsBuffer size: %zu\n", spsPpsBuffer_.size());
//            OutputDebugStringA(log);
//        }
//
//
//#endif
//    }

    return true;
}


bool H264Encoder::FlushDelayedFrames(
    std::vector<std::vector<BYTE>>& flushedFrames)
{
    if (!encoder_) return false;

    HRESULT hr = S_OK;
    // (1) 入力終了を通知
    hr = encoder_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
    if (FAILED(hr)) {
        OutputDebugStringA("[Flush] NotifyEndOfStream failed.\n");
        return false;
    }

    // (2) ドレイン開始を指示
    hr = encoder_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (FAILED(hr)) {
        OutputDebugStringA("[Flush] CommandDrain failed.\n");
        return false;
    }

    // (3) ProcessOutput で残りを取り出す
    while (true) {
        // サンプルとバッファを用意
        ComPtr<IMFSample> sample;
        hr = MFCreateSample(&sample);
        if (FAILED(hr)) break;

        MFT_OUTPUT_STREAM_INFO info = {};
        hr = encoder_->GetOutputStreamInfo(0, &info);
        if (FAILED(hr)) break;

        ComPtr<IMFMediaBuffer> buf;
        hr = MFCreateMemoryBuffer(info.cbSize, &buf);
        if (FAILED(hr)) break;

        sample->AddBuffer(buf.Get());

        MFT_OUTPUT_DATA_BUFFER outBuf = {};
        outBuf.pSample = sample.Get();
        outBuf.dwStreamID = 0;
        DWORD status = 0;

        hr = encoder_->ProcessOutput(0, 1, &outBuf, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            // もう出力なし
            break;
        }
        if (FAILED(hr)) {
            OutputDebugStringA("[Flush] ProcessOutput failed.\n");
            break;
        }

        // 成功したらバッファをロックしてデータ取得
        BYTE* pData = nullptr;
        DWORD maxLen = 0, curLen = 0;
        hr = buf->Lock(&pData, &maxLen, &curLen);
        if (SUCCEEDED(hr) && curLen > 0) {
            flushedFrames.emplace_back(pData, pData + curLen);
            buf->Unlock();
            char log[128];
            sprintf_s(log, "[Flush] Got %u bytes\n", curLen);
            OutputDebugStringA(log);
        }
        // outBuf.pSample は MFCreateSample で作ったものなので Release は不要
    }

    return true;
}

