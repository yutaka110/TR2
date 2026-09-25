#define NOMINMAX
#include "../network/UdpReceiver.h"
#include "../network/NetworkManager.h"
#include "../network/NetworkVideoReceiver.h"
#include "../network/H264Encoder.h"
#include "../network/FrameIdentityLedger.h"
#include "../network/NalUtils.h"
#include "ReachRobotVideo.h"
#include "ReachJson.h"
#include "ReachBufferedLog.h"
#include "ReachDatagramLink.h"
#include "ReachBudgetTransport.h"
#include "ReachDecodeJournal.h"
#include "ReachBaseline.h"
#include "ReachActionExecution.h"
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
    std::unique_ptr<DatagramLink> link;
    std::shared_ptr<BudgetTransport> budget;uint16_t feedbackIngress=0;
    std::thread worker;
    std::mutex mutex;
    std::deque<CaptureFrame> queue;
    std::map<uint32_t,uint64_t> captureTimes;
    std::set<uint32_t> seen;
    RobotVideoView view;
    std::unique_ptr<MarkerRecognizer> recognizer;
    BufferedLog observations;
    std::unique_ptr<DecodeJournal> decodeJournal;
    std::unique_ptr<Baseline> baseline;
    std::unique_ptr<ActionExecution> actions;
    std::string error;
    std::atomic<bool> closing=false;
    bool finished=false,passed=false,hardware=false;
    uint32_t stream=0,dropFrame=0;
    uint64_t encoderTail=0,decoderTail=0;
    uint64_t maxQueue=0,maxQueueWait=0,maxSubmit=0,maxPoll=0,maxRecognition=0,maxPublishWait=0;
    uint64_t encoderStopUs=0,decoderStopUs=0;
    net::AsyncInputCredits inputCredits;
    uint32_t recognitionDelayMs=0;
    std::atomic<int> encoderPhase=0;
    std::atomic<uint64_t> encoderPhaseStart=0;
    BufferedLog captures,encodedLog,audit,encoderTiming,decodeTiming;
    Impl(const FoundationConfig& c,ResearchSession& s,std::shared_ptr<BudgetTransport> b):config(c),session(s),budget(std::move(b)) {
        stream=static_cast<uint32_t>(std::stoul(s.Id().substr(0,8),nullptr,16)); if(!stream) stream=1;
        view.streamId=stream; dropFrame=DiagnosticDrop();
        decodeJournal=std::make_unique<DecodeJournal>(s.Directory());
        udp.SetCompletedRejectionObserver([this](uint32_t id,uint32_t streamId,uint32_t witness){try{
            const auto reason="older_than_jitter_release:"+std::to_string(witness);
            decodeJournal->Event({"completed_rejected",id,streamId,MonotonicUs(),0,0,0,false,false,reason.c_str()});
        }catch(const std::exception& ex){Fail(ex.what());}});
        decoder.SetDecodeTraceObserver([this](const net::DecodeTraceEvent& e){try{decodeJournal->Event(e);}catch(const std::exception& ex){Fail(ex.what());}});
        udp.SetReassemblyObserver([this](const net::FrameAckInfo& a,const char* event,const char* outcome,uint64_t now){try{decodeJournal->Chunks(a,event,outcome,now);}catch(const std::exception& ex){Fail(ex.what());}});
        udp.SetCompletedObserver([this](const net::CompletedFrame& f){try{
            net::H264AccessUnitPayloadHeader h{};net::DecodeH264AccessUnitPayloadHeader(f.data.data(),f.data.size(),h);
            decodeJournal->Event({"reassembled",f.frameId,f.streamId,MonotonicUs(),h.cameraCaptureCompletedTimeUs,0,0,false,false,"before_au_validation"});
        }catch(const std::exception& ex){Fail(ex.what());}});
        char delay[16]{};
        if(GetEnvironmentVariableA("TR2_REACH_DIAGNOSTIC_RECOGNITION_DELAY_MS",delay,16)){
            const auto value=std::string(delay);size_t end=0;const auto n=std::stoul(value,&end);
            Require(end==value.size()&&n<=500,"invalid recognition diagnostic delay");recognitionDelayMs=static_cast<uint32_t>(n);
        }
        encoderTiming.open(s.Directory()/"encoder_timing.csv");encoderTiming.exceptions(std::ios::badbit|std::ios::failbit);
        decodeTiming.open(s.Directory()/"decode_timing.csv");decodeTiming.exceptions(std::ios::badbit|std::ios::failbit);
        encoderTiming<<"frame_id,dequeued_us,queue_wait_us,remaining_queue,submit_us,sample_create_ms,process_input_ms,max_poll_us\n";
        decodeTiming<<"frame_id,callback_start_us,recognition_us,publish_wait_us,callback_us\n";
        if(c.HasVisualControl()) {
            recognizer=std::make_unique<MarkerRecognizer>(c.task);
            observations.open(s.Directory()/"observations.csv");observations.exceptions(std::ios::badbit|std::ios::failbit);
            observations.precision(12);
            observations<<"frame_id,stream_id,capture_us,received_us,valid,reason,x_m,y_m,yaw_rad,speed_m_s,speed_valid,position_error_m,yaw_error_rad,reprojection_rms_px,min_side_px,tl_u,tl_v,tr_u,tr_v,br_u,br_v,bl_u,bl_v\n";
            auto model=MarkerRecognizer::ModelJson();
            if(c.stage=="command_udp"){
                auto at=model.find("local_diagnostic_adapter");model.replace(at,std::string("local_diagnostic_adapter").size(),"RCMD_v1_UDP");
                at=model.find("\"command_udp\": false");model.replace(at,std::string("\"command_udp\": false").size(),"\"command_udp\": true");
            }
            std::ofstream(s.Directory()/"visual_model.json")<<model;
        }
        for(auto pair:{std::pair{&captures,"captures.csv"},std::pair{&encodedLog,"encoded.csv"},std::pair{&audit,"decoded_audit.csv"}}) {
            pair.first->open(s.Directory()/pair.second,std::ios::binary); pair.first->exceptions(std::ios::badbit|std::ios::failbit);
        }
        captures<<"frame_id,stream_id,capture_us,encoder_input_pts_100ns,render_us,queue_depth\n";
        encodedLog<<"frame_id,stream_id,capture_us,output_pts_100ns,encoder_output_us,bytes,idr,diagnostic_drop,drain\n";
        audit<<"frame_id,stream_id,pixel_id,pixel_stream,pixel_valid,capture_us,expected_capture_us,output_pts_100ns,matched_source_pts_us,decoded_us,age_ms,identity_match,duplicate\n";
        std::ofstream(s.Directory()/"world_model.json")<<RobotWorld::ModelJson(c.HasVisualControl(),c.stage=="command_udp");
        if(budget)udp.SetDatagramSendHook([this](SOCKET socket,std::span<const uint8_t> data,const sockaddr_in& address){
            auto target=address;target.sin_port=htons(feedbackIngress);return budget->Send(false,socket,data,target);});
        Require(udp.Start(0,true,true),"loopback UDP receiver start failed");
        udp.SetJitterBufferAutoModeEnabled(false); udp.SetJitterBufferTargetDelayMs(0);
        if(c.boundedLink)link=std::make_unique<DatagramLink>(c.uplink,s.Directory(),"uplink",udp.BoundPort());
        sender=std::make_unique<NetworkManager>("127.0.0.1",link?link->Port():udp.BoundPort());
        if(c.baseline=="G4-01"||c.baseline=="G4-02"||c.baseline=="G4-03"||c.baseline=="G4-04")actions=std::make_unique<ActionExecution>(*budget,s.Directory(),c.baseline!="G4-01"?sender.get():nullptr,c.task=="T1"?1:2,c.baseline=="G4-03"||c.baseline=="G4-04",c.baseline=="G4-04",c.baselineLambda);
        else if(!c.baseline.empty())baseline=std::make_unique<Baseline>(c.baseline,c.baselineLambda,static_cast<uint32_t>(std::min(c.ipBudget.total.rateBps,c.ipBudget.up.rateBps)),s.Directory());
        sender->SetPacingEnabled(false); sender->SetFecEnabled(false); sender->SetAdaptiveFecEnabled(false);
        if(budget){
            Require(sender->ConfigureResearchControlSocket(),"research feedback socket configuration failed");
            sender->SetDatagramSendHook([this](SOCKET socket,std::span<const uint8_t> data,const sockaddr_in& address){return actions?actions->Send(socket,data,address):budget->Send(true,socket,data,address);});
            sender->SetControlReceiveObserver([this](std::span<const uint8_t> data){budget->Feedback(data);if(baseline)baseline->Feedback(data);if(actions)actions->Feedback(data);});
            sender->SetFecEnabled(c.ipBudget.fecGroup!=0);if(c.ipBudget.fecGroup)sender->SetFecGroupChunkCount(c.ipBudget.fecGroup);
        }
        if(baseline){sender->SetPacingEnabled(true);sender->SetPacingTargetBitrateKbps(static_cast<uint32_t>(std::min(c.ipBudget.total.rateBps,c.ipBudget.up.rateBps)/1000));
            sender->SetAdaptiveFecEnabled(c.baseline=="B0");
            sender->SetResearchRepairGate([this](uint32_t id,std::span<const uint16_t> chunks){return baseline->Repair(id,chunks,*sender);});}
        if(actions){sender->SetPacingEnabled(true);sender->SetPacingTargetBitrateKbps(static_cast<uint32_t>(std::min(c.ipBudget.total.rateBps,c.ipBudget.up.rateBps)/1000));
            sender->SetAdaptiveFecEnabled(false);sender->SetFecEnabled(false);
            sender->SetResearchRepairGate([this](uint32_t id,std::span<const uint16_t> chunks){return actions->Repair(id,chunks);});}
        // Without the opt-in budget model, preserve the isolated G1/G2-02 path.
        Require(decoder.Start(&udp,[]{return true;},[this](const net::DecodedVideoFrame& f){Observe(f);},true),"video decoder start failed");
        std::promise<void> ready; auto future=ready.get_future();
        worker=std::thread([this,p=std::move(ready)]()mutable{Encode(std::move(p));});
        try { future.get(); } catch(...) { closing=true; worker.join(); decoder.Stop(); throw; }
        session.Event("robot_video_started","loopback port="+std::to_string(udp.BoundPort())+"; stream="+std::to_string(stream)+"; diagnostic_drop="+std::to_string(dropFrame));
    }
    ~Impl() {
        closing=true; if(worker.joinable()) worker.join();if(actions)actions->StopScheduler(); decoder.Stop(); udp.Stop();if(sender){sender->StopRNVPControlReceiver();if(actions)sender->StopResearchPacer();}
        if(actions)actions->Close();
    }
    void Fail(const std::string& reason) { std::lock_guard lock(mutex); if(error.empty()) error=reason; }
    void Phase(int phase){encoderPhaseStart=MonotonicUs();encoderPhase=phase;}
    void Checkpoint(const char* stage){
        std::ofstream out(session.Directory()/(std::string(stage)=="capture_queue_overflow"?"pipeline_failure.json":"pipeline_checkpoint.json"));
        out<<"{\"stage\":"<<JsonString(stage)<<",\"monotonic_us\":"<<MonotonicUs()
           <<",\"encoder_phase\":"<<encoderPhase.load()<<",\"encoder_phase_start_us\":"<<encoderPhaseStart.load()
           <<",\"max_capture_queue\":"<<maxQueue<<"}\n";out.flush();
    }
    void Observe(const net::DecodedVideoFrame& f) noexcept {
        try {
            const auto callbackStart=MonotonicUs();
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
            uint64_t expected=0;
            { std::lock_guard lock(mutex);const auto found=captureTimes.find(f.frameId);if(found!=captureTimes.end())expected=found->second; }
            // seen, recognizer and these logs are exclusively owned by the decoder thread.
            // Neither image processing nor IO may hold the capture/control state mutex.
            const bool duplicate=!seen.insert(f.frameId).second;
            const bool match=pixel.valid&&pixel.frameId==f.frameId&&pixel.streamId==f.streamId&&f.streamId==stream&&
                expected&&expected==f.cameraCaptureCompletedTimeUs&&expected==f.h264MatchedSourcePtsUs&&
                f.h264OutputSampleTimeValid&&f.h264OutputSampleTime100ns==static_cast<int64_t>(expected*10)&&f.h264IdentityMatched&&!duplicate;
            const double age=f.decodedTimeUs>=f.cameraCaptureCompletedTimeUs?(f.decodedTimeUs-f.cameraCaptureCompletedTimeUs)/1000.0:-1;
            audit<<f.frameId<<','<<f.streamId<<','<<pixel.frameId<<','<<pixel.streamId<<','<<pixel.valid<<','
                <<f.cameraCaptureCompletedTimeUs<<','<<expected<<','<<f.h264OutputSampleTime100ns<<','<<f.h264MatchedSourcePtsUs<<','
                <<f.decodedTimeUs<<','<<age<<','<<match<<','<<duplicate<<'\n';
            VisualObservation observation;
            const auto recognitionStart=MonotonicUs();
            if(recognizer){
                if(f.frameId==60&&recognitionDelayMs)std::this_thread::sleep_for(std::chrono::milliseconds(recognitionDelayMs));
                // The receiver supplies pixels and verified capture metadata, never RobotTruth.
                observation=recognizer->Process({luma,size_t(pitch)*360,640,360,pitch,f.frameId,f.streamId,
                    f.cameraCaptureCompletedTimeUs,MonotonicUs(),match&&f.referenceTrusted});
                const auto& o=observation;
                observations<<o.frameId<<','<<o.streamId<<','<<o.captureUs<<','<<o.receivedUs<<','<<o.valid<<','<<o.reason<<','
                    <<o.x<<','<<o.y<<','<<o.yaw<<','<<o.speed<<','<<o.speedValid<<','<<o.positionErrorM<<','<<o.yawErrorRad<<','<<o.reprojectionRms<<','<<o.minSidePx;
                for(auto corner:o.corners)observations<<','<<corner.u<<','<<corner.v;observations<<'\n';
            }
            const auto recognitionUs=MonotonicUs()-recognitionStart;
            const auto adoptionUs=MonotonicUs();
            const bool displayReady=match&&f.referenceTrusted&&adoptionUs>=f.cameraCaptureCompletedTimeUs&&adoptionUs-f.cameraCaptureCompletedTimeUs<=200000;
            decodeJournal->Event({displayReady?"display_adopted":"display_rejected",f.frameId,f.streamId,adoptionUs,f.cameraCaptureCompletedTimeUs,f.referenceGeneration,f.decoderInputUs,false,f.referenceTrusted,displayReady?"verified_timely_image":"identity_reference_or_deadline"});
            if(recognizer){decodeJournal->Event({observation.valid?"recognition_accepted":"recognition_rejected",f.frameId,f.streamId,adoptionUs,f.cameraCaptureCompletedTimeUs,f.referenceGeneration,f.decoderInputUs,false,f.referenceTrusted,observation.reason.c_str()});decodeJournal->Observation(observation);}
            const auto publishStart=MonotonicUs();uint64_t publishWait=0;
            { std::lock_guard lock(mutex);publishWait=MonotonicUs()-publishStart;
                ++view.decoded;if(match)++view.matched;else++view.errors;
                if(recognizer){view.observation=std::move(observation);if(view.observation.valid)++view.recognized;else++view.rejected;}
                if(match)view.lastDecodedFrameId=f.frameId;
                if(displayReady){view.frameId=f.frameId;view.ageMs=age;view.bgra=std::move(bgra);}
            }
            maxRecognition=std::max(maxRecognition,recognitionUs);maxPublishWait=std::max(maxPublishWait,publishWait);
            decodeTiming<<f.frameId<<','<<callbackStart<<','<<recognitionUs<<','<<publishWait<<','<<MonotonicUs()-callbackStart<<'\n';
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
                Phase(1);const auto pollStart=MonotonicUs();const bool pollOk=encoder.PollTrackedOutput(output);Phase(0);
                maxPoll=std::max(maxPoll,MonotonicUs()-pollStart);
                Require(pollOk,"encoder output failed");
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
                if(dropped){
                    std::ofstream proof(session.Directory()/"diagnostic_dropped_au.h264",std::ios::binary);
                    proof.exceptions(std::ios::badbit|std::ios::failbit);
                    proof.write(reinterpret_cast<const char*>(output.bytes.data()),output.bytes.size());
                }
                NetworkManager::RnvpFrameProtectionOptions protection;
                if(actions&&actions->Scheduled())actions->Queue(identity->frameId,stream,identity->captureUs,std::move(payload),idr,dropped);
                else{const bool offer=actions?actions->Plan(identity->frameId,stream,identity->captureUs,payload.size(),idr,protection):!baseline||baseline->Plan(identity->frameId,identity->captureUs,payload.size(),idr,*sender,protection);
                    if(!dropped&&offer) sender->SendRNVPFragmented(payload,identity->frameId,net::CodecType::H264,stream,idr,protection);}
                encodedLog<<identity->frameId<<','<<stream<<','<<identity->captureUs<<','<<output.pts100ns<<','<<now<<','<<output.bytes.size()<<','<<idr<<','<<dropped<<','<<drain<<'\n';
                std::lock_guard lock(mutex); ++view.encoded; if(!dropped)++view.sent; if(drain)++encoderTail;
                return true;
            };
            uint64_t closeDeadline=0,lastKeyframeRequest=0;
            for(;;) {
                if(closing&&!closeDeadline)closeDeadline=MonotonicUs()+3000000;
                Require(!closeDeadline||MonotonicUs()<closeDeadline,"encoder input shutdown timed out");
                poll(false);
                CaptureFrame frame; bool hasFrame=false;size_t remaining=0;
                if(encoder.CanAcceptInput()) {
                    std::lock_guard lock(mutex);
                    if(!queue.empty()){frame=std::move(queue.front());queue.pop_front();remaining=queue.size();hasFrame=true;}
                    else if(closing) break;
                }
                if(hasFrame) {
                    const auto dequeued=MonotonicUs(),queueWait=dequeued-frame.identity.captureUs;
                    maxQueueWait=std::max(maxQueueWait,queueWait);
                    const bool requestDue=frame.identity.frameId==45||dequeued>=lastKeyframeRequest+500000;
                    const bool requested=baseline&&requestDue&&sender->ConsumeKeyFrameRequest();
                    const bool actionRequested=actions&&actions->Request(frame.identity.frameId,frame.identity.frameId==45||sender->ConsumeKeyFrameRequest());
                    if(actionRequested||(!actions&&(frame.identity.frameId==45||requested))){
                        decodeJournal->Event({"encoder_keyframe_requested",frame.identity.frameId,stream,MonotonicUs(),frame.identity.captureUs,0,0,false,false,actions?"action_refresh_arbiter":requested?"received_RNVP_request":"forced_at_input_45"});
                        encoder.RequestKeyFrame();
                        lastKeyframeRequest=dequeued;
                    }
                    Phase(2);const auto submitStart=MonotonicUs();const bool submitOk=encoder.SubmitTrackedFrame(frame.nv12.data(),static_cast<UINT>(frame.nv12.size()),static_cast<int64_t>(frame.identity.ptsUs*10));Phase(0);
                    const auto submitUs=MonotonicUs()-submitStart;maxSubmit=std::max(maxSubmit,submitUs);
                    const auto timing=encoder.GetLastFrameTiming();
                    encoderTiming<<frame.identity.frameId<<','<<dequeued<<','<<queueWait<<','<<remaining<<','<<submitUs<<','<<timing.sampleCreateMs<<','<<timing.processInputMs<<','<<maxPoll<<'\n';
                    Require(submitOk,"encoder rejected available input");
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
        inputCredits=encoder.TrackedInputCredits();Phase(3);encoder.Shutdown(); if(mf)MFShutdown();if(com)CoUninitialize();Phase(4);
    }
};
RobotVideo::RobotVideo(const FoundationConfig& c,ResearchSession& s,std::shared_ptr<BudgetTransport> b):impl_(std::make_unique<Impl>(c,s,std::move(b))){}
uint16_t RobotVideo::SenderPort()const{return impl_->sender->BoundPort();}
void RobotVideo::SetFeedbackIngress(uint16_t port){Require(port!=0,"invalid feedback ingress");impl_->feedbackIngress=port;}
void RobotVideo::FinishFeedback(uint64_t expected){auto& p=*impl_;if(!p.budget)return;
    const auto deadline=MonotonicUs()+1000000;while(p.budget->Received()<expected&&MonotonicUs()<deadline)Pause();
    p.sender->StopRNVPControlReceiver();if(p.baseline)p.baseline->Close();if(p.actions){p.sender->StopResearchPacer();p.actions->Close();}Require(p.budget->Received()==expected,"feedback UDP delivery mismatch");p.budget->Check();}
RobotVideo::~RobotVideo(){try{if(impl_&&!impl_->finished)Finish();}catch(...){}}
void RobotVideo::StartLink(uint64_t origin){impl_->decodeJournal->Start(origin,impl_->config.durationUs);if(impl_->link)impl_->link->Start(origin);if(impl_->budget){Require(impl_->feedbackIngress!=0,"feedback route missing");Require(impl_->sender->StartRNVPControlReceiver(),"feedback receiver start failed");}if(impl_->actions)impl_->actions->StartScheduler(origin,impl_->config.durationUs);}
void RobotVideo::RecordControl(const MotionCommand& command){impl_->decodeJournal->Control(command);}
StateReport RobotVideo::ReceiverNotification(){return impl_->decodeJournal->NotificationSnapshot(impl_->stream);}
void RobotVideo::Capture(const RobotWorld& world) {
    Check(); auto& p=*impl_; uint32_t id;
    { std::lock_guard lock(p.mutex); id=static_cast<uint32_t>(p.view.captured+1); }
    const auto renderStart=MonotonicUs();
    auto pixels=world.CaptureNv12(id,p.stream); const auto now=MonotonicUs();
    size_t depth=0;
    { std::lock_guard lock(p.mutex);
        if(p.queue.size()>=8){p.Checkpoint("capture_queue_overflow");throw std::runtime_error("capture queue overflow: real-time run invalid");}
        p.captureTimes.emplace(id,now);
        p.queue.push_back({{id,p.stream,640,360,now,now,0,0,0},std::move(pixels)});++p.view.captured;
        depth=p.queue.size();p.maxQueue=std::max(p.maxQueue,static_cast<uint64_t>(depth));
    }
    p.captures<<id<<','<<p.stream<<','<<now<<','<<now*10<<','<<now-renderStart<<','<<depth<<'\n';
}
RobotVideoView RobotVideo::View() {
    auto& p=*impl_; net::DecodedVideoFrame discarded;
    while(p.decoder.TryGetLatestFrame(discarded)) p.udp.NotifyDisplayFrame();
    std::lock_guard lock(p.mutex); return p.view;
}
VisualObservation RobotVideo::Observation(){std::lock_guard lock(impl_->mutex);return impl_->view.observation;}
void RobotVideo::Check(){if(impl_->actions)impl_->actions->CheckScheduler();if(impl_->budget)impl_->budget->Check();if(impl_->link)impl_->link->Check();std::lock_guard lock(impl_->mutex);if(!impl_->error.empty())throw std::runtime_error(impl_->error);}
bool RobotVideo::Finish() {
    auto& p=*impl_; if(p.finished)return p.passed;
    p.Checkpoint("encoder_join");auto stopStart=MonotonicUs();p.closing=true;if(p.worker.joinable())p.worker.join();p.encoderStopUs=MonotonicUs()-stopStart;
    if(p.actions){p.actions->StopScheduler();p.actions->CheckScheduler();}
    if(p.baseline||p.actions){const auto deadline=MonotonicUs()+1000000;while(p.sender->GetPacingStats().queuedPackets&&MonotonicUs()<deadline)Pause();Require(p.sender->GetPacingStats().queuedPackets==0,"research pacer did not drain");}
    if(p.link)p.link->Finish();
    p.Checkpoint("decoder_drain");
    uint64_t expected;{std::lock_guard lock(p.mutex);expected=p.view.sent;}
    if(p.link)expected=p.udp.GetStats().completedFrames;
    const uint64_t deadline=MonotonicUs()+2500000;
    while(p.decoder.GetStats().decodePopSuccesses<expected&&MonotonicUs()<deadline){View();Pause();}
    const auto before=View().decoded;
    p.decoder.RequestDecoderDrain(); const auto drainDeadline=MonotonicUs()+1000000;
    while(!p.decoder.DecoderDrainComplete()&&MonotonicUs()<drainDeadline)Pause();
    const bool decoderDrained=p.decoder.DecoderDrainComplete();
    const auto decoderStats=p.decoder.GetStats();
    const auto transportStats=p.udp.GetStats();
    p.Checkpoint("decoder_stop");stopStart=MonotonicUs();p.decoder.Stop();p.udp.Stop();p.decoderStopUs=MonotonicUs()-stopStart;
    p.Checkpoint("stopped");
    std::lock_guard lock(p.mutex); p.decoderTail=p.view.decoded-before;
    const bool complete=p.view.encoded==p.view.captured&&p.view.sent==p.view.captured&&p.view.decoded==p.view.sent;
    p.passed=p.error.empty()&&p.view.errors==0&&(p.link||p.view.decoded>0)&&decoderDrained&&
        p.view.encoded==p.view.captured&&(p.link||(p.dropFrame?(p.view.sent+1==p.view.captured&&p.view.lastDecodedFrameId==p.view.captured):complete))&&decoderStats.decodeFailures==0;
    std::ofstream out(p.session.Directory()/"robot_video_summary.json");out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<"{\"validation_passed\":"<<(p.passed?"true":"false")<<",\"complete_no_loss_delivery\":"<<(complete?"true":"false")
       <<",\"capacity_queue_model\":"<<(p.link?"true":"false")<<",\"validation_scope\":"<<JsonString(p.link?"transport_integrity_under_modeled_loss; not task success":"complete_ideal_delivery_or_explicit_G1_diagnostic")
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
       <<",\"recognized\":"<<p.view.recognized<<",\"recognition_rejected\":"<<p.view.rejected
       <<",\"pipeline_metrics\":{\"capture_queue_limit\":8,\"max_capture_queue\":"<<p.maxQueue<<",\"max_queue_wait_us\":"<<p.maxQueueWait
       <<",\"max_submit_us\":"<<p.maxSubmit<<",\"max_poll_us\":"<<p.maxPoll<<",\"max_recognition_us\":"<<p.maxRecognition<<",\"max_publish_wait_us\":"<<p.maxPublishWait
       <<",\"encoder_stop_us\":"<<p.encoderStopUs<<",\"decoder_stop_us\":"<<p.decoderStopUs<<",\"diagnostic_recognition_delay_ms\":"<<p.recognitionDelayMs
       <<",\"input_need_events\":"<<p.inputCredits.events<<",\"input_submissions\":"<<p.inputCredits.submitted<<",\"maximum_input_credits\":"<<p.inputCredits.maximum<<"}"
       <<",\"command_source\":"<<JsonString(p.recognizer?"received_image_v1":"time_script_v1")<<",\"closed_loop_validated\":false}\n";
    if(p.observations.is_open())p.observations.close();
    p.captures.close();p.encodedLog.close();p.audit.close();p.encoderTiming.close();p.decodeTiming.close();SaveBmp(p.session.Directory()/"last_received.bmp",p.view.bgra);
    p.decodeJournal->Close();
    p.finished=true;return p.passed;
}
void RobotVideo::PublishBaselineState(const SenderStateEstimate& estimate){
    auto& p=*impl_;if(p.baseline)p.baseline->PublishState(ProjectBaselineState(p.config.baseline,estimate));
    if(p.actions)p.actions->PublishState(estimate);
}
}
