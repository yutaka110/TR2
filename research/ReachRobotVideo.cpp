#define NOMINMAX
#include "../network/UdpReceiver.h"
#include "../network/NetworkManager.h"
#include "../network/NetworkVideoReceiver.h"
#include "../network/H264Encoder.h"
#include "../network/FrameIdentityLedger.h"
#include "../network/NalUtils.h"
#include "ReachRobotVideo.h"
#include "ReachJson.h"
#include <mfapi.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <future>
#include <map>
#include <set>
#include <thread>

namespace reach {
namespace {
void Require(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
void Pause() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
uint32_t DiagnosticDrop() {
    wchar_t text[32]{}; const auto n=GetEnvironmentVariableW(L"TR2_REACH_DIAGNOSTIC_DROP_FRAME",text,32);
    if(!n) return 0;
    Require(n<32,"invalid diagnostic drop frame");
    wchar_t* end=nullptr; const auto value=wcstoul(text,&end,10);
    Require(end!=text&&*end==0&&value>0&&value<=1800,"invalid diagnostic drop frame");
    return value;
}
void SaveBmp(const std::filesystem::path& path,const std::vector<uint8_t>& bgra) {
    if(bgra.size()!=640*360*4) return;
    BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+static_cast<DWORD>(bgra.size());
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=640; info.biHeight=-360;
    info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    std::ofstream out(path,std::ios::binary); out.exceptions(std::ios::badbit|std::ios::failbit);
    out.write(reinterpret_cast<char*>(&file),sizeof(file)); out.write(reinterpret_cast<char*>(&info),sizeof(info));
    out.write(reinterpret_cast<const char*>(bgra.data()),bgra.size());
}
}
struct RobotVideo::Impl {
    struct CaptureFrame { net::FrameIdentity identity; std::vector<uint8_t> nv12; };
    FoundationConfig config;
    ResearchSession& session;
    net::UdpReceiver udp;
    net::NetworkVideoReceiver decoder;
    std::unique_ptr<NetworkManager> sender;
    std::thread worker;
    std::mutex mutex;
    std::deque<CaptureFrame> queue;
    std::map<uint32_t,uint64_t> captureTimes;
    std::set<uint32_t> seen;
    RobotVideoView view;
    std::string error;
    std::atomic<bool> closing=false;
    bool finished=false,passed=false,hardware=false;
    uint32_t stream=0,dropFrame=0;
    uint64_t encoderTail=0,decoderTail=0;
    std::ofstream captures,encodedLog,audit;
    Impl(const FoundationConfig& c,ResearchSession& s):config(c),session(s) {
        stream=static_cast<uint32_t>(std::stoul(s.Id().substr(0,8),nullptr,16)); if(!stream) stream=1;
        view.streamId=stream; dropFrame=DiagnosticDrop();
        for(auto pair:{std::pair{&captures,"captures.csv"},std::pair{&encodedLog,"encoded.csv"},std::pair{&audit,"decoded_audit.csv"}}) {
            pair.first->open(s.Directory()/pair.second,std::ios::binary); pair.first->exceptions(std::ios::badbit|std::ios::failbit);
        }
        captures<<"frame_id,stream_id,capture_us,encoder_input_pts_100ns\n";
        encodedLog<<"frame_id,stream_id,capture_us,output_pts_100ns,encoder_output_us,bytes,idr,diagnostic_drop,drain\n";
        audit<<"frame_id,stream_id,pixel_id,pixel_stream,pixel_valid,capture_us,expected_capture_us,output_pts_100ns,matched_source_pts_us,decoded_us,age_ms,identity_match,duplicate\n";
        std::ofstream(s.Directory()/"world_model.json")<<RobotWorld::ModelJson();
        Require(udp.Start(0,true,true),"loopback UDP receiver start failed");
        udp.SetJitterBufferAutoModeEnabled(false); udp.SetJitterBufferTargetDelayMs(0);
        sender=std::make_unique<NetworkManager>("127.0.0.1",udp.BoundPort());
        sender->SetPacingEnabled(false); sender->SetFecEnabled(false); sender->SetAdaptiveFecEnabled(false);
        // Isolated transport validation: ACK receiver intentionally disabled, no repair/adaptation.
        Require(decoder.Start(&udp,[]{return true;},[this](const net::DecodedVideoFrame& f){Observe(f);},true),"video decoder start failed");
        std::promise<void> ready; auto future=ready.get_future();
        worker=std::thread([this,p=std::move(ready)]()mutable{Encode(std::move(p));});
        try { future.get(); } catch(...) { closing=true; worker.join(); decoder.Stop(); throw; }
        session.Event("robot_video_started","loopback port="+std::to_string(udp.BoundPort())+"; stream="+std::to_string(stream)+"; diagnostic_drop="+std::to_string(dropFrame));
    }
    ~Impl() {
        closing=true; if(worker.joinable()) worker.join(); decoder.Stop(); udp.Stop();
    }
    void Fail(const std::string& reason) { std::lock_guard lock(mutex); if(error.empty()) error=reason; }
    void Observe(const net::DecodedVideoFrame& f) noexcept {
        try {
            std::vector<uint8_t> y, bgra(640*360*4);
            const uint8_t* luma=nullptr; uint32_t pitch=0;
            Require(f.width==640&&f.height==360,"unexpected decoded dimensions");
            if(f.format==net::DecodedVideoFrameFormat::Nv12) {
                Require(f.nv12YPitch>=640&&f.nv12Y.size()>=size_t(f.nv12YPitch)*360,"short decoded NV12");
                luma=f.nv12Y.data(); pitch=f.nv12YPitch;
            } else {
                Require(f.rgba.size()>=640*360*4,"short decoded RGBA"); y.resize(640*360);
                for(size_t i=0;i<y.size();++i) y[i]=f.rgba[i*4]; luma=y.data(); pitch=640;
            }
            const auto pixel=ReadPixelIdentity(luma,pitch,640,360);
            for(size_t v=0;v<360;++v) for(size_t u=0;u<640;++u) {
                const auto i=(v*640+u)*4; const auto gray=static_cast<uint8_t>(std::clamp((int(luma[v*pitch+u])-16)*255/219,0,255));
                bgra[i]=bgra[i+1]=bgra[i+2]=gray; bgra[i+3]=255;
            }
            std::lock_guard lock(mutex);
            const auto found=captureTimes.find(f.frameId);
            const uint64_t expected=found==captureTimes.end()?0:found->second;
            const bool duplicate=!seen.insert(f.frameId).second;
            const bool match=pixel.valid&&pixel.frameId==f.frameId&&pixel.streamId==f.streamId&&f.streamId==stream&&
                expected&&expected==f.cameraCaptureCompletedTimeUs&&expected==f.h264MatchedSourcePtsUs&&
                f.h264OutputSampleTimeValid&&f.h264OutputSampleTime100ns==static_cast<int64_t>(expected*10)&&f.h264IdentityMatched&&!duplicate;
            const double age=f.decodedTimeUs>=f.cameraCaptureCompletedTimeUs?(f.decodedTimeUs-f.cameraCaptureCompletedTimeUs)/1000.0:-1;
            audit<<f.frameId<<','<<f.streamId<<','<<pixel.frameId<<','<<pixel.streamId<<','<<pixel.valid<<','
                <<f.cameraCaptureCompletedTimeUs<<','<<expected<<','<<f.h264OutputSampleTime100ns<<','<<f.h264MatchedSourcePtsUs<<','
                <<f.decodedTimeUs<<','<<age<<','<<match<<','<<duplicate<<'\n';
            ++view.decoded; if(match)++view.matched;else++view.errors;
            if(match){view.frameId=f.frameId; view.ageMs=age; view.bgra=std::move(bgra);}
        } catch(const std::exception& e) { Fail(e.what()); }
    }
    void Encode(std::promise<void> ready) noexcept {
        H264Encoder encoder; bool com=false,mf=false,announced=false;
        try {
            Require(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"encoder COM initialization failed"); com=true;
            Require(SUCCEEDED(MFStartup(MF_VERSION)),"MF startup failed"); mf=true;
            SetEnvironmentVariableA("RNVP_H264_ENCODER",config.encoder.c_str());
            Require(encoder.Initialize(640,360,1500000,30),"research encoder initialization failed");
            hardware=encoder.IsAsyncHardware();
            net::FrameIdentityLedger ledger;
            ready.set_value(); announced=true;
            auto poll=[&](bool drain) {
                H264Encoder::TrackedAccessUnit output;
                Require(encoder.PollTrackedOutput(output),"encoder output failed");
                if(output.bytes.empty()) return false;
                Require(output.ptsValid,"encoder returned no output PTS");
                const auto identity=ledger.Take(output.pts100ns);
                Require(identity.has_value(),"encoder output PTS has no accepted capture");
                auto nals=ScanAnnexB(output.bytes); bool idr=false,sps=false,pps=false;
                for(const auto& n:nals){idr|=n.type==5;sps|=n.type==7;pps|=n.type==8;}
                if(idr&&(!sps||!pps)) {
                    const auto headers=encoder.GetSpsPps(); output.bytes.insert(output.bytes.begin(),headers.begin(),headers.end());
                    nals=ScanAnnexB(output.bytes); for(const auto& n:nals){sps|=n.type==7;pps|=n.type==8;}
                }
                Require(!nals.empty()&&(!idr||(sps&&pps)),"invalid research H264 access unit");
                const uint64_t now=MonotonicUs();
                net::H264AccessUnitPayloadHeader header{};
                header.frameId=identity->frameId; header.ptsUs=identity->ptsUs; header.width=640;header.height=360;
                header.flags=(idr?(net::H264AccessUnitFlag_Idr|net::H264AccessUnitFlag_DecoderSync):0)|((sps&&pps)?net::H264AccessUnitFlag_ContainsSpsPps:0);
                header.codecConfigId=(sps&&pps)?identity->frameId:0; header.nalUnitCount=static_cast<uint16_t>(nals.size());
                header.accessUnitBytes=static_cast<uint32_t>(output.bytes.size()); header.headerBytes=net::kH264AccessUnitPayloadHeaderV3Size;
                header.cameraCaptureCompletedTimeUs=identity->captureUs; header.encoderOutputTimeUs=now;
                header.accessUnitCrc32=net::ComputeCrc32(output.bytes.data(),output.bytes.size());
                std::vector<uint8_t> payload(header.headerBytes+output.bytes.size());
                net::EncodeH264AccessUnitPayloadHeaderV3(payload.data(),header);
                std::memcpy(payload.data()+header.headerBytes,output.bytes.data(),output.bytes.size());
                const bool dropped=identity->frameId==dropFrame;
                if(!dropped) sender->SendRNVPFragmented(payload,identity->frameId,net::CodecType::H264,stream,idr);
                encodedLog<<identity->frameId<<','<<stream<<','<<identity->captureUs<<','<<output.pts100ns<<','<<now<<','<<output.bytes.size()<<','<<idr<<','<<dropped<<','<<drain<<'\n';
                std::lock_guard lock(mutex); ++view.encoded; if(!dropped)++view.sent; if(drain)++encoderTail;
                return true;
            };
            uint64_t closeDeadline=0;
            for(;;) {
                if(closing&&!closeDeadline)closeDeadline=MonotonicUs()+3000000;
                Require(!closeDeadline||MonotonicUs()<closeDeadline,"encoder input shutdown timed out");
                poll(false);
                CaptureFrame frame; bool hasFrame=false;
                if(encoder.CanAcceptInput()) {
                    std::lock_guard lock(mutex);
                    if(!queue.empty()){frame=std::move(queue.front());queue.pop_front();hasFrame=true;}
                    else if(closing) break;
                }
                if(hasFrame) {
                    if(frame.identity.frameId==45)encoder.RequestKeyFrame();
                    Require(encoder.SubmitTrackedFrame(frame.nv12.data(),static_cast<UINT>(frame.nv12.size()),static_cast<int64_t>(frame.identity.ptsUs*10)),"encoder rejected available input");
                    Require(ledger.Insert(static_cast<int64_t>(frame.identity.ptsUs*10),frame.identity),"duplicate/full encoder identity ledger");
                }
                Pause();
            }
            Require(encoder.BeginTrackedDrain(),"encoder drain start failed");
            const uint64_t timeout=MonotonicUs()+3000000;
            while(!encoder.TrackedDrainComplete()&&MonotonicUs()<timeout){poll(true);Pause();}
            while(poll(true)){}
            Require(encoder.TrackedDrainComplete()&&ledger.Size()==0,"encoder drain incomplete");
        } catch(...) {
            if(!announced)ready.set_exception(std::current_exception());
            try { throw; } catch(const std::exception& e){Fail(e.what());} catch(...){Fail("encoder unknown error");}
        }
        encoder.Shutdown(); if(mf)MFShutdown();if(com)CoUninitialize();
    }
};
RobotVideo::RobotVideo(const FoundationConfig& c,ResearchSession& s):impl_(std::make_unique<Impl>(c,s)){}
RobotVideo::~RobotVideo(){try{if(impl_&&!impl_->finished)Finish();}catch(...){}}
void RobotVideo::Capture(const RobotWorld& world) {
    Check(); auto& p=*impl_; uint32_t id;
    { std::lock_guard lock(p.mutex); id=static_cast<uint32_t>(p.view.captured+1); }
    auto pixels=world.CaptureNv12(id,p.stream); const auto now=MonotonicUs();
    std::lock_guard lock(p.mutex);
    Require(p.queue.size()<8,"capture queue overflow: real-time run invalid");
    p.captureTimes.emplace(id,now);
    p.captures<<id<<','<<p.stream<<','<<now<<','<<now*10<<'\n';
    p.queue.push_back({{id,p.stream,640,360,now,now,0,0,0},std::move(pixels)}); ++p.view.captured;
}
RobotVideoView RobotVideo::View() {
    auto& p=*impl_; net::DecodedVideoFrame discarded;
    while(p.decoder.TryGetLatestFrame(discarded)) p.udp.NotifyDisplayFrame();
    std::lock_guard lock(p.mutex); return p.view;
}
void RobotVideo::Check(){std::lock_guard lock(impl_->mutex);if(!impl_->error.empty())throw std::runtime_error(impl_->error);}
bool RobotVideo::Finish() {
    auto& p=*impl_; if(p.finished)return p.passed;
    p.closing=true;if(p.worker.joinable())p.worker.join();
    uint64_t expected;{std::lock_guard lock(p.mutex);expected=p.view.sent;}
    const uint64_t deadline=MonotonicUs()+2500000;
    while(p.decoder.GetStats().decodePopSuccesses<expected&&MonotonicUs()<deadline){View();Pause();}
    const auto before=View().decoded;
    p.decoder.RequestDecoderDrain(); const auto drainDeadline=MonotonicUs()+1000000;
    while(!p.decoder.DecoderDrainComplete()&&MonotonicUs()<drainDeadline)Pause();
    const bool decoderDrained=p.decoder.DecoderDrainComplete();
    const auto decoderStats=p.decoder.GetStats();
    const auto transportStats=p.udp.GetStats();
    p.decoder.Stop();p.udp.Stop();
    std::lock_guard lock(p.mutex); p.decoderTail=p.view.decoded-before;
    const bool complete=p.view.encoded==p.view.captured&&p.view.sent==p.view.captured&&p.view.decoded==p.view.sent;
    p.passed=p.error.empty()&&p.view.errors==0&&p.view.decoded>0&&decoderDrained&&
        p.view.encoded==p.view.captured&&(p.dropFrame?(p.view.sent+1==p.view.captured&&p.view.frameId==p.view.captured):complete)&&decoderStats.decodeFailures==0;
    std::ofstream out(p.session.Directory()/"robot_video_summary.json");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"{\"validation_passed\":"<<(p.passed?"true":"false")<<",\"complete_no_loss_delivery\":"<<(complete?"true":"false")
       <<",\"encoder_mode\":"<<JsonString(p.config.encoder)<<",\"async_hardware\":"<<(p.hardware?"true":"false")
       <<",\"captured\":"<<p.view.captured<<",\"encoded\":"<<p.view.encoded<<",\"sent\":"<<p.view.sent<<",\"decoded\":"<<p.view.decoded
       <<",\"matched\":"<<p.view.matched<<",\"identity_errors\":"<<p.view.errors<<",\"encoder_drain_outputs\":"<<p.encoderTail
       <<",\"decoder_drain_outputs\":"<<p.decoderTail<<",\"decoder_drained\":"<<(decoderDrained?"true":"false")
       <<",\"decode_failures\":"<<decoderStats.decodeFailures<<",\"freshness_drops\":"<<decoderStats.freshnessDroppedFrames
       <<",\"udp_receive_buffer_bytes\":"<<p.udp.ReceiveBufferBytes()<<",\"udp_completed_frames\":"<<transportStats.completedFrames
       <<",\"udp_missing_packets\":"<<transportStats.missingPackets<<",\"udp_reordered_packets\":"<<transportStats.reorderedPackets
       <<",\"udp_deadline_drops\":"<<transportStats.deadlineDroppedFrames<<",\"udp_queue_drops\":"<<transportStats.outputQueueDroppedFrames
       <<",\"decode_pop_count\":"<<decoderStats.decodePopSuccesses
       <<",\"diagnostic_drop_frame\":"<<p.dropFrame<<",\"force_idr_at_input\":45,\"error\":"<<JsonString(p.error)
       <<",\"command_source\":\"time_script_v1\",\"closed_loop_validated\":false}\n";
    p.captures.close();p.encodedLog.close();p.audit.close();SaveBmp(p.session.Directory()/"last_received.bmp",p.view.bgra);
    p.finished=true;return p.passed;
}
}
