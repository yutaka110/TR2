#include "ColorConverter.h"
#include <mfidl.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>
#include <mferror.h>
#include <initguid.h> // GUID 定義の展開に必要

#include <mfplay.h>
#include <mfreadwrite.h>
#include <comdef.h> // _com_error
#include <string>

using Microsoft::WRL::ComPtr;

ColorConverter::~ColorConverter() {
#if _DEBUG
    OutputDebugStringA("[DEBUG] ~ColorConverter called\n");
#endif

    if (converter_) {
#if _DEBUG
        OutputDebugStringA("[DEBUG] Resetting converter_ (IMFTransform)\n");
#endif
        // 明示的にリセット（Release を呼ぶ）
        converter_.Reset();
    }
}

// Wide文字列（wchar_t*）→ UTF-8文字列（std::string）変換関数
std::string ConvertWideToUtf8(const wchar_t* wideStr) {
    int size = WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, nullptr, 0, nullptr, nullptr);
    std::string result(size, 0);
    WideCharToMultiByte(CP_UTF8, 0, wideStr, -1, &result[0], size, nullptr, nullptr);
    return result;
}

bool ColorConverter::Initialize(UINT width, UINT height) {
    width_ = width;
    height_ = height;

    IMFActivate** ppActivate = nullptr;
    UINT32 count = 0;

    MFT_REGISTER_TYPE_INFO input = { MFMediaType_Video, MFVideoFormat_RGB32 };
    MFT_REGISTER_TYPE_INFO output = { MFMediaType_Video, MFVideoFormat_NV12 };

    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_PROCESSOR,
        MFT_ENUM_FLAG_ALL,
        &input,
        &output,
        &ppActivate,
        &count
    );
    if (FAILED(hr) || count == 0) {
        OutputDebugStringA("[ERROR] MFTEnumEx failed or no matching MFT found.\n");
        return false;
    }

    hr = ppActivate[0]->ActivateObject(__uuidof(IMFTransform), (void**)&converter_);
    if (FAILED(hr)) {
        _com_error err(hr);
        std::string msg = "[ERROR] ActivateObject failed: " + ConvertWideToUtf8(err.ErrorMessage()) + "\n";
        OutputDebugStringA(msg.c_str());

        for (UINT32 i = 0; i < count; i++) {
            ppActivate[i]->Release();
        }
        CoTaskMemFree(ppActivate);
        return false;
    }

    for (UINT32 i = 0; i < count; i++) {
        ppActivate[i]->Release();
    }
    CoTaskMemFree(ppActivate);

    // 入力タイプの設定
    ComPtr<IMFMediaType> inputType;
    hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFCreateMediaType for input failed.\n");
        return false;
    }

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, 30, 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = converter_->SetInputType(0, inputType.Get(), 0);
    if (FAILED(hr)) {
        _com_error err(hr);
        std::string msg = "[ERROR] SetInputType failed: " + ConvertWideToUtf8(err.ErrorMessage()) + "\n";
        OutputDebugStringA(msg.c_str());
        return false;
    }

    // 出力タイプの設定
    ComPtr<IMFMediaType> outputType;
    hr = MFCreateMediaType(&outputType);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFCreateMediaType for output failed.\n");
        return false;
    }

    outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(outputType.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(outputType.Get(), MF_MT_FRAME_RATE, 30, 1);
    outputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = converter_->SetOutputType(0, outputType.Get(), 0);
    if (FAILED(hr)) {
        _com_error err(hr);
        std::string msg = "[ERROR] SetOutputType failed: " + ConvertWideToUtf8(err.ErrorMessage()) + "\n";
        OutputDebugStringA(msg.c_str());
        return false;
    }

    // ストリーミング開始通知
    hr = converter_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFT_MESSAGE_NOTIFY_BEGIN_STREAMING failed.\n");
        return false;
    }

    hr = converter_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] MFT_MESSAGE_NOTIFY_START_OF_STREAM failed.\n");
        return false;
    }

    OutputDebugStringA("[INFO] ColorConverter initialized successfully.\n");
    return true;
}

//bool ColorConverter::Convert(IMFSample* inputSample, IMFSample** outputSample) {
//    if (!converter_) return false;
//
//    // 入力があるときだけ入力を渡す（nullptrなら出力試行だけ）
//    if (inputSample) {
//        HRESULT hrIn = converter_->ProcessInput(0, inputSample, 0);
//        if (FAILED(hrIn)) {
//#if _DEBUG
//            OutputDebugStringA("[ERROR] Convert: ProcessInput failed\n");
//#endif
//            return false;
//        }
//    }
//
//   
//
//    MFT_OUTPUT_STREAM_INFO streamInfo = {};
//    HRESULT hr = converter_->GetOutputStreamInfo(0, &streamInfo);
//    if (FAILED(hr)) {
//#if _DEBUG
//        OutputDebugStringA("[ERROR] Convert: GetOutputStreamInfo failed\n");
//#endif
//        return false;
//    }
//
//    ComPtr<IMFMediaBuffer> buffer;
//    hr = MFCreateMemoryBuffer(streamInfo.cbSize, &buffer);
//    if (FAILED(hr)) {
//#if _DEBUG
//        OutputDebugStringA("[ERROR] Convert: MFCreateMemoryBuffer failed\n");
//#endif
//        return false;
//    }
//
//    ComPtr<IMFSample> sample;
//    hr = MFCreateSample(&sample);
//    if (FAILED(hr)) {
//#if _DEBUG
//        OutputDebugStringA("[ERROR] Convert: MFCreateSample failed\n");
//#endif
//        return false;
//    }
//
//    hr = sample->AddBuffer(buffer.Get());
//    if (FAILED(hr)) {
//#if _DEBUG
//        OutputDebugStringA("[ERROR] Convert: AddBuffer failed\n");
//#endif
//        return false;
//    }
//
//    MFT_OUTPUT_DATA_BUFFER outputData = {};
//    outputData.dwStreamID = 0;
//    outputData.pSample = sample.Get();
//
//    DWORD status = 0;
//    hr = converter_->ProcessOutput(0, 1, &outputData, &status);
//
//    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
//#if _DEBUG
//        OutputDebugStringA("[INFO] Convert: NEED_MORE_INPUT - not producing output yet\n");
//#endif
//        * outputSample = nullptr;  // 出力なしでも正常
//        return true;
//    }
//    else if (FAILED(hr)) {
//#if _DEBUG
//        OutputDebugStringA("[ERROR] Convert: ProcessOutput failed\n");
//#endif
//        return false;
//    }
//
//    *outputSample = sample.Detach();
//    return true;
//}

bool ColorConverter::Convert(IMFSample* inputSample, IMFSample** outputSample) {
    if (!converter_) return false;

    HRESULT hr;

    // 【1】古い出力を空打ちで回収（必要な限り）
    while (true) {
        MFT_OUTPUT_STREAM_INFO streamInfo = {};
        hr = converter_->GetOutputStreamInfo(0, &streamInfo);
        if (FAILED(hr)) {
            OutputDebugStringA("[ERROR] Convert: GetOutputStreamInfo failed\n");
            return false;
        }

        ComPtr<IMFMediaBuffer> dummyBuffer;
        hr = MFCreateMemoryBuffer(streamInfo.cbSize, &dummyBuffer);
        if (FAILED(hr)) {
            OutputDebugStringA("[ERROR] Convert: Create dummy buffer failed\n");
            return false;
        }

        ComPtr<IMFSample> dummySample;
        hr = MFCreateSample(&dummySample);
        if (FAILED(hr)) {
            OutputDebugStringA("[ERROR] Convert: Create dummy sample failed\n");
            return false;
        }

        dummySample->AddBuffer(dummyBuffer.Get());

        MFT_OUTPUT_DATA_BUFFER outputData = {};
        outputData.dwStreamID = 0;
        outputData.pSample = dummySample.Get();
        DWORD status = 0;

        hr = converter_->ProcessOutput(0, 1, &outputData, &status);
        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            break;  // 入力を受け付け可能になったので次へ
        } else if (FAILED(hr)) {
            break;  // エラーなら無理せず終了
        }

        // 成功したけど今回の出力は捨てる（空打ち）
        OutputDebugStringA("[INFO] Convert: dummy output cleared\n");
    }

    // 【2】入力を送る
    if (inputSample) {
        hr = converter_->ProcessInput(0, inputSample, 0);
        if (FAILED(hr)) {
            OutputDebugStringA("[ERROR] Convert: ProcessInput failed\n");
            return false;
        }
    }

    // 【3】出力を受け取る
    MFT_OUTPUT_STREAM_INFO streamInfo = {};
    hr = converter_->GetOutputStreamInfo(0, &streamInfo);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] Convert: GetOutputStreamInfo (2) failed\n");
        return false;
    }

    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(streamInfo.cbSize, &buffer);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] Convert: Create output buffer failed\n");
        return false;
    }

    ComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] Convert: Create output sample failed\n");
        return false;
    }

    sample->AddBuffer(buffer.Get());

    MFT_OUTPUT_DATA_BUFFER outputData = {};
    outputData.dwStreamID = 0;
    outputData.pSample = sample.Get();
    DWORD status = 0;

    hr = converter_->ProcessOutput(0, 1, &outputData, &status);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        OutputDebugStringA("[INFO] Convert: NEED_MORE_INPUT - output not ready\n");
        *outputSample = nullptr;
        return true;
    } else if (FAILED(hr)) {
        OutputDebugStringA("[ERROR] Convert: ProcessOutput failed\n");
        return false;
    }

    *outputSample = sample.Detach();
    return true;
}

bool ColorConverter::ProcessMessage(DWORD msg, ULONG_PTR param) {
    if (!converter_) return false;
    HRESULT hr = converter_->ProcessMessage(static_cast<MFT_MESSAGE_TYPE>(msg), param);
    return SUCCEEDED(hr);
}


