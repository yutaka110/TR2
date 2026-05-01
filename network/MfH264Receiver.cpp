// MfH264Receiver.cpp --- Aルート（RGB32）版
// H.264(Annex-B) → MFTでRGB32まで展開（CPU）→ D3D12アップロード（行ピッチ考慮）

#include "MfH264Receiver.h"

#include <condition_variable>
#include <utility>
#define HR(expr)  do { HRESULT _hr=(expr); if(FAILED(_hr)){ assert(false); return _hr; } } while(0)
#define HRV(expr) do { HRESULT _hr=(expr); if(FAILED(_hr)){ assert(false); return;   } } while(0)
#define HRB(expr) do { HRESULT _hr=(expr); if(FAILED(_hr)){ assert(false); return false; } } while(0)

static void LogHR(const char* tag, HRESULT hr) {
	char buf[256]; sprintf_s(buf, "[MF][%s] hr=0x%08X\n", tag, (unsigned)hr);
	OutputDebugStringA(buf);
}

// MJPEG -> YUY2 を「ソフトウェアMFT限定」で列挙
static HRESULT CreateMjpegToYUY2Decoder_SOFT(
	UINT width, UINT height, Microsoft::WRL::ComPtr<IMFTransform>& out)
{
	IMFActivate** acts = nullptr; UINT32 count = 0;

	MFT_REGISTER_TYPE_INFO inInfo = { MFMediaType_Video, MFVideoFormat_MJPG };
	MFT_REGISTER_TYPE_INFO outInfo = { MFMediaType_Video, MFVideoFormat_YUY2 };

	// ★ポイント：HWを含めない
	DWORD flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;

	HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inInfo, &outInfo, &acts, &count);
	if (FAILED(hr) || count == 0) {
		if (acts) CoTaskMemFree(acts);
		// どうしても見つからない時だけ HW も試す（最後の保険）
		flags = MFT_ENUM_FLAG_SORTANDFILTER;
		hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inInfo, &outInfo, &acts, &count);
	}
	if (FAILED(hr) || count == 0) { if (acts) CoTaskMemFree(acts); return MF_E_NOT_FOUND; }

	Microsoft::WRL::ComPtr<IMFTransform> dec;
	HR(acts[0]->ActivateObject(__uuidof(IMFTransform), (void**)dec.GetAddressOf()));

	// 入力 MJPG
	Microsoft::WRL::ComPtr<IMFMediaType> inType;  HR(MFCreateMediaType(&inType));
	HR(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	HR(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_MJPG));
	HR(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height));
	MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 30, 1);
	inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	HR(dec->SetInputType(0, inType.Get(), 0));

	// 出力 YUY2
	Microsoft::WRL::ComPtr<IMFMediaType> outType; HR(MFCreateMediaType(&outType));
	HR(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	HR(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2));
	HR(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width, height));
	MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, 30, 1);
	outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	HR(dec->SetOutputType(0, outType.Get(), 0));

	for (UINT32 i = 0; i < count; ++i) acts[i]->Release();
	CoTaskMemFree(acts);
	out = dec;
	return S_OK;
}


static HRESULT CreateYUY2toRGB32Converter(
	UINT width, UINT height, Microsoft::WRL::ComPtr<IMFTransform>& out)
{
	Microsoft::WRL::ComPtr<IMFTransform> conv;
	HR(CoCreateInstance(CLSID_CColorConvertDMO, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&conv)));

	// 入力: YUY2
	Microsoft::WRL::ComPtr<IMFMediaType> inType;  HR(MFCreateMediaType(&inType));
	HR(inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	HR(inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_YUY2));
	HR(MFSetAttributeSize(inType.Get(), MF_MT_FRAME_SIZE, width, height));
	MFSetAttributeRatio(inType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	MFSetAttributeRatio(inType.Get(), MF_MT_FRAME_RATE, 30, 1);
	inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	HR(conv->SetInputType(0, inType.Get(), 0));

	// 出力: RGB32
	Microsoft::WRL::ComPtr<IMFMediaType> outType; HR(MFCreateMediaType(&outType));
	HR(outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video));
	HR(outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
	HR(MFSetAttributeSize(outType.Get(), MF_MT_FRAME_SIZE, width, height));
	MFSetAttributeRatio(outType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
	MFSetAttributeRatio(outType.Get(), MF_MT_FRAME_RATE, 30, 1);
	outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	HR(conv->SetOutputType(0, outType.Get(), 0));

	out = conv;
	return S_OK;
}


//======================
// SamplePool (RGB32)
//======================
HRESULT SamplePool::Initialize(UINT width, UINT height, UINT poolSize) {
	width_ = width; height_ = height; pool_.clear();
	const DWORD stride = width * 4; // RGB32
	const DWORD size = stride * height;
	for (UINT i = 0; i < poolSize; ++i) {
		ComPtr<IMFSample> s; HR(MFCreateSample(&s));
		ComPtr<IMFMediaBuffer> buf; HR(MFCreateMemoryBuffer(size, &buf));
		HR(s->AddBuffer(buf.Get()));
		pool_.push_back(s);
	}
	return S_OK;
}
HRESULT SamplePool::Acquire(ComPtr<IMFSample>& out) {
	if (pool_.empty()) return HRESULT_FROM_WIN32(ERROR_BUSY);
	out = pool_.back();
	pool_.pop_back();
	return S_OK;
}
void SamplePool::Release(ComPtr<IMFSample> s) {
	if (s) pool_.push_back(s);
}



HRESULT MfH264Receiver::FeedAccessUnit(const BYTE* data, DWORD size, LONGLONG ts100ns, BOOL isIDR) {
	ComPtr<IMFSample> sample; HR(MFCreateSample(&sample));
	ComPtr<IMFMediaBuffer> buf; HR(MFCreateMemoryBuffer(size, &buf));

	BYTE* p = nullptr; DWORD max = 0; HR(buf->Lock(&p, &max, nullptr));
	memcpy(p, data, size);
	HR(buf->Unlock());
	HR(buf->SetCurrentLength(size));

	HR(sample->AddBuffer(buf.Get()));
	HR(sample->SetSampleTime(ts100ns));
	HR(sample->SetSampleDuration(166666)); // 約1/60秒

	if (isIDR) {
		ComPtr<IMFAttributes> attr; HR(sample->QueryInterface(IID_PPV_ARGS(&attr)));
		HR(attr->SetUINT32(MFSampleExtension_CleanPoint, TRUE));
		static bool first = true;
		if (first) { HR(attr->SetUINT32(MFSampleExtension_Discontinuity, TRUE)); first = false; }
	}
	return decoder_->ProcessInput(0, sample.Get(), 0);
}


//======================
// MfH264Receiver (NV12)
//======================

HRESULT MfH264Receiver::InitializeDecoder(UINT width, UINT height)
{
    width_ = width; height_ = height;
    HR(MFStartup(MF_VERSION));

    // ★HWを避けた MJPG->YUY2
    HR(CreateMjpegToYUY2Decoder_SOFT(width_, height_, decoderMJPG_));
    // YUY2 -> RGB32
    HR(CreateYUY2toRGB32Converter(width_, height_, colorConv_));

    // ストリーミング開始
    HR(decoderMJPG_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    HR(decoderMJPG_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));
    HR(colorConv_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0));
    HR(colorConv_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0));

    // 出力サンプル供給要否
    MFT_OUTPUT_STREAM_INFO osi = {};
    HR(decoderMJPG_->GetOutputStreamInfo(0, &osi));
    needAppSamples_ = (0 == (osi.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES));
    if (needAppSamples_) HR(samplePool_.Initialize(width_, height_, 4));

    // 既存のRGBピッチ計算をそのまま利用
    RGBA8Desc d; HR(ComputeRGBA8Desc(width_, height_, d));
    rgba_ = d;
    return S_OK;
}

bool MfH264Receiver::DrainOneFrameToD3D12(
    ID3D12Resource* rgbaTex,
    ID3D12Resource* rgbaUpload,
    ID3D12GraphicsCommandList* cmd,
    const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp,
    bool bgraToRgba,
    const std::shared_ptr<std::queue<std::vector<uint8_t>>>& frameQueue,
    const std::shared_ptr<std::mutex>& frameQueueMtx
) {
    // --- 0) 供給 ---
    {
        static bool started = false;
        if (!started) {
            decoderMJPG_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            decoderMJPG_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            decoderMJPG_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

            colorConv_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
            colorConv_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
            colorConv_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
            started = true;
        }

        static LONGLONG t = 0;
        const LONGLONG dur = 10'000'000LL / 30; // 30fps

        const int kMaxFeed = 2;
        for (int i = 0; i < kMaxFeed; ++i) {
            std::vector<uint8_t> jpeg;
            {
                std::lock_guard<std::mutex> lk(*frameQueueMtx);
                if (frameQueue->empty()) break;
                jpeg = std::move(frameQueue->front());
                frameQueue->pop();
            }

            Microsoft::WRL::ComPtr<IMFMediaBuffer> mb;
            if (FAILED(MFCreateMemoryBuffer((DWORD)jpeg.size(), &mb))) break;

            BYTE* dst = nullptr; DWORD maxLen = 0;
            if (FAILED(mb->Lock(&dst, &maxLen, nullptr))) break;
            memcpy(dst, jpeg.data(), jpeg.size());
            mb->Unlock();
            mb->SetCurrentLength((DWORD)jpeg.size());

            Microsoft::WRL::ComPtr<IMFSample> s;
            if (FAILED(MFCreateSample(&s))) break;
            s->AddBuffer(mb.Get());
            s->SetSampleTime(t);
            s->SetSampleDuration(dur);
            t += dur;

            HRESULT hrPI = decoderMJPG_->ProcessInput(0, s.Get(), 0);
            if (hrPI == MF_E_NOTACCEPTING) {
                // 出力が詰まってる→ここでは吐かせずに抜ける（Drain側で出力）
                break;
            }
            if (FAILED(hrPI)) {
                char buf[128]; sprintf_s(buf, "[FEED] ProcessInput hr=0x%08X\n", (unsigned)hrPI);
                OutputDebugStringA(buf);
                break;
            }
        }
    }

    // ===== 以降はあなたの現行ロジック（取り出し→変換→アップロード） =====

    if (!rgbaTex || !rgbaUpload || !cmd) {
        OutputDebugStringA("[Drain] invalid args: null resource/cmd\n");
        return false;
    }
    if (fp.Footprint.Width == 0 || fp.Footprint.Height == 0 || fp.Footprint.RowPitch == 0) {
        OutputDebugStringA("[Drain] invalid footprint (zero fields)\n");
        return false;
    }

    HRESULT hr = S_OK;
    bool ok = false;

    Microsoft::WRL::ComPtr<IMFSample> appA;
    MFT_OUTPUT_DATA_BUFFER outA = {}; DWORD statusA = 0;
    MFT_OUTPUT_DATA_BUFFER outB = {}; DWORD statusB = 0;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> firstBuf;
    Microsoft::WRL::ComPtr<IMF2DBuffer> buf2D;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> contig;

    BYTE* srcBase = nullptr;
    DWORD curLen = 0;
    LONG  srcPitch = rgba_.pitch;

    BYTE* mappedUploadBase = nullptr;

    auto log_hr = [](HRESULT ehr, const char* msg) {
        char buf[256];
        sprintf_s(buf, "[Drain][HR=0x%08X] %s\n", (unsigned)ehr, msg);
        OutputDebugStringA(buf);
        };

    auto unlockResources = [&]() {
        if (mappedUploadBase) { rgbaUpload->Unmap(0, nullptr); mappedUploadBase = nullptr; }
        if (buf2D) buf2D->Unlock2D();
        if (contig) contig->Unlock();
        if (appA) samplePool_.Release(appA);
        if (outB.pSample) { outB.pSample->Release(); outB.pSample = nullptr; }
        // ★needAppSamples_==false の場合、outA.pSample は MFT が供給したもの → 解放が必要
        if (!needAppSamples_ && outA.pSample) { outA.pSample->Release(); outA.pSample = nullptr; }
        };

    // A) MJPEG → YUY2 取り出し
    if (needAppSamples_) {
        if (FAILED(samplePool_.Acquire(appA))) {
            OutputDebugStringA("[Drain] samplePool Acquire failed\n");
            unlockResources();
            return false;
        }
        outA.pSample = appA.Get();
    }
    hr = decoderMJPG_->ProcessOutput(0, 1, &outA, &statusA);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        OutputDebugStringA("[Drain] NEED_MORE_INPUT (decoderMJPG)\n");
        unlockResources();
        return false;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        log_hr(hr, "[Drain] STREAM_CHANGE: renegotiate output type required");
        unlockResources();
        return false;
    }
    if (FAILED(hr) || !outA.pSample) {
        log_hr(hr, "[Drain] decoderMJPG_->ProcessOutput failed or null sample");
        unlockResources();
        return false;
    }

    // B) Color Converter
    hr = colorConv_->ProcessInput(0, outA.pSample, 0);
    if (FAILED(hr)) { log_hr(hr, "[Drain] colorConv_->ProcessInput failed"); unlockResources(); return false; }
    hr = colorConv_->ProcessOutput(0, 1, &outB, &statusB);
    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        OutputDebugStringA("[Drain] NEED_MORE_INPUT (colorConv)\n");
        unlockResources();
        return false;
    }
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        log_hr(hr, "[Drain] STREAM_CHANGE from colorConv");
        unlockResources();
        return false;
    }
    if (FAILED(hr) || !outB.pSample) {
        log_hr(hr, "[Drain] colorConv_->ProcessOutput failed or null sample");
        unlockResources();
        return false;
    }

    // C) RGB32 バッファ取得
    if (SUCCEEDED(outB.pSample->GetBufferByIndex(0, &firstBuf))) {
        if (SUCCEEDED(firstBuf.As(&buf2D)) && buf2D) {
            LONG p = 0; BYTE* s0 = nullptr;
            if (SUCCEEDED(buf2D->Lock2D(&s0, &p))) {
                srcBase = s0;
                srcPitch = p;
                curLen = (DWORD)(std::abs(p) * rgba_.height);
            }
        }
    }
    if (!srcBase) {
        if (FAILED(outB.pSample->ConvertToContiguousBuffer(&contig)) ||
            FAILED(contig->Lock(&srcBase, nullptr, &curLen))) {
            OutputDebugStringA("[Drain] failed to lock contiguous buffer\n");
            unlockResources();
            return false;
        }
        srcPitch = (LONG)(rgba_.width * 4);
    }

    const UINT w = (UINT)rgba_.width;
    const UINT h = (UINT)rgba_.height;
    const UINT rowBytes = w * 4;
    if (fp.Footprint.RowPitch < rowBytes) {
        OutputDebugStringA("[Drain] Footprint.RowPitch < width*4\n");
        unlockResources();
        return false;
    }

    // D) Upload BUFFER にコピー
    if (FAILED(rgbaUpload->Map(0, nullptr, reinterpret_cast<void**>(&mappedUploadBase))) || !mappedUploadBase) {
        OutputDebugStringA("[Drain] Map(upload) failed\n");
        unlockResources();
        return false;
    }
    {
        BYTE* dst = mappedUploadBase + fp.Offset;
        const UINT dstPitch = fp.Footprint.RowPitch;
        const bool topDown = (srcPitch < 0);
        const UINT srcStep = (UINT)(topDown ? (-srcPitch) : srcPitch);
        const BYTE* srcTop = topDown ? (srcBase + (h - 1) * srcStep) : srcBase;

        if (!bgraToRgba) {
            for (UINT y = 0; y < h; ++y) {
                memcpy(dst + y * dstPitch, srcTop + y * srcStep, rowBytes);
            }
        }
        else {
            for (UINT y = 0; y < h; ++y) {
                const BYTE* s = srcTop + y * srcStep;
                BYTE* d = dst + y * dstPitch;
                for (UINT x = 0; x < w; ++x) {
                    d[4 * x + 0] = s[4 * x + 2];
                    d[4 * x + 1] = s[4 * x + 1];
                    d[4 * x + 2] = s[4 * x + 0];
                    d[4 * x + 3] = s[4 * x + 3];
                }
            }
        }
    }
    rgbaUpload->Unmap(0, nullptr);
    mappedUploadBase = nullptr;

    // E) CopyTextureRegion
    {
        D3D12_TEXTURE_COPY_LOCATION dstLoc = { rgbaTex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dstLoc.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION srcLoc = { rgbaUpload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        srcLoc.PlacedFootprint = fp;
        cmd->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);
    }

    // F) バリア
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = rgbaTex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1, &b);
    }

    ok = true;
    unlockResources();
    return ok;
}







HRESULT MfH264Receiver::ComputeRGBA8Desc(UINT w, UINT h, RGBA8Desc& out) {
	out.width = w; out.height = h;
	LONG stride = 0; HR(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, w, &stride));
	out.pitch = stride; // 幅×4とは限らない（アライメントのため）
	return S_OK;
}

HRESULT MfH264Receiver::TryDrainAndDiscard() {
	MFT_OUTPUT_DATA_BUFFER out = {};
	DWORD status = 0;
	ComPtr<IMFSample> appSample;
	if (needAppSamples_) {
		if (FAILED(samplePool_.Acquire(appSample))) {
			return MF_E_TRANSFORM_NEED_MORE_INPUT;
		}
		out.pSample = appSample.Get();
	}

	HRESULT hr = decoder_->ProcessOutput(0, 1, &out, &status);
	if (SUCCEEDED(hr) && out.pSample) {
		ComPtr<IMFMediaBuffer> buf;
		if (SUCCEEDED(out.pSample->ConvertToContiguousBuffer(&buf))) {
			BYTE* p = nullptr; DWORD len = 0;
			if (SUCCEEDED(buf->Lock(&p, nullptr, &len))) {
				buf->Unlock(); // 触って即解放
			}
		}
	}
	if (appSample) samplePool_.Release(appSample);
	return hr; // S_OK:1枚吐けた / MF_E_TRANSFORM_NEED_MORE_INPUT:もう無い
}

