#define NOMINMAX
#include "ReachActionExecution.h"
#include <cstdlib>
namespace reach {
namespace {std::string Hex(std::span<const uint8_t> bytes){std::string result;for(auto b:bytes){result+="0123456789abcdef"[b>>4];result+="0123456789abcdef"[b&15];}return result;}}
ActionExecution::ActionExecution(BudgetTransport& budget,const std::filesystem::path& path,NetworkManager* network,int task,bool scheduled,bool comparison,double lambda):budget_(budget),network_(network),task_(task),predict_(network!=nullptr),scheduled_(scheduled){
    if(comparison){
        char variant[32]{};const auto n=GetEnvironmentVariableA("TR2_REACH_SCHEDULER_VARIANT",variant,32);
        if(n>=32)throw std::runtime_error("scheduler variant too long");const std::string mode=n?variant:"joint";
        if(mode=="common")schedulerOptions_.mask=PredictionMask::Common;else if(mode=="decode")schedulerOptions_.mask=PredictionMask::Decode;
        else if(mode=="task")schedulerOptions_.mask=PredictionMask::Task;else if(mode=="scalar")schedulerOptions_.scalar=true;else if(mode!="joint")throw std::runtime_error("unknown scheduler variant");
        schedulerOptions_.lambda=lambda;
        std::ofstream(path/"ablation_model.json")<<"{\"version\":\"G4-04-v1\",\"variant\":\""<<mode<<"\",\"lambda\":"<<lambda<<",\"mask_before_inference\":true,\"model\":\"frozen G4-02 path library, marginalized masked coordinates\",\"candidate_and_notification_cost\":\"common\",\"calibration_used\":false,\"task_success_model\":false}";
    }
    if(predict_){
        std::array<char,32768> modelPath{};const auto length=GetEnvironmentVariableA("TR2_REACH_PREDICTION_MODEL",modelPath.data(),static_cast<DWORD>(modelPath.size()));
        if(length>=modelPath.size())throw std::runtime_error("prediction model path too long");if(length)predictor_.Load(modelPath.data());
        predictions_.open(path/"predictions.csv");predictionFeedback_.open(path/"prediction_feedback.csv");
        predictions_.exceptions(std::ios::badbit|std::ios::failbit);predictionFeedback_.exceptions(std::ios::badbit|std::ios::failbit);
        predictions_.precision(17);predictionFeedback_.precision(17);
        predictions_<<"decision_id,event_us,frame_id,candidate,kind,group,idr,reference,stage,task,estimate_id,sampled_us,received_us,generated_us,notification_sequence,report_stream,decoded_frame,last_idr,feedback_id,feedback_us,queued_bytes,rate_bps,eligible,live,causal";
        for(int i=0;i<16;++i)predictions_<<",x"<<i;
        predictions_<<",status,support,runs,distance,terminal_supported";
        for(int i=0;i<6;++i)predictions_<<",raw"<<i<<",p"<<i;
        for(int i=0;i<3;++i)predictions_<<",p50_"<<i<<",p90_"<<i;
        predictions_<<'\n';predictionFeedback_<<"id,event_us,wire_hex,loss,burst,last_sequence\n";
        std::ofstream(path/"prediction_model.json")<<R"({"version":"G4-02-v2","horizon_us":200000,"policy":"development_cyclic_fresh_g2_g4_g8_with_common_repairs_refresh","inference":"shadow_only","max_neighbors":64,"min_neighbors":8,"min_runs":2,"state_lifetime_us":250000,"labels":["AU_available_by_deadline","AU_decoded_by_deadline","reported_reference_at_horizon","AU_command_applied","later_image_command_applied","new_task_completion"],"interpretation":"conditional forecast under fixed continuation; not isolated causal gain or 60-second success probability","unknown_probability":-1,"censored_delay_ms":999})";
    }
    candidates_.open(path/"action_candidates.csv");events_.open(path/"action_events.csv");wire_.open(path/"action_wire.csv");
    for(auto* l:{&candidates_,&events_,&wire_})l->exceptions(std::ios::badbit|std::ios::failbit);
    candidates_<<"decision_id,event_us,frame_id,stream_id,generation,active_generation,capture_us,payload_bytes,remaining_ip,kind,group,ip_bytes,eligible,reason,selected,offered,missing_count,repair_requests,refresh_wanted,refresh_pending,last_refresh_us,missing_chunks\n";
    events_<<"event_us,event,frame_id,generation,decision_id,related_frame\n";
    wire_<<"event_us,frame_id,stream_id,chunk_index,kind,authorization_id,generation,active_generation,capture_us,ip_bytes,result,status\n";
    std::string actionContract=R"({"version":"G4-01-v1","policy":"wiring_only_protect_group4_else_fresh","max_frame_metadata":4,"candidate_count":7,"deadline_us":200000,"repair_request_limit":2,"idr_cooldown_us":500000,"refresh_timeout":"remain_pending_until_verified_output_or_session_end","candidate_trigger":"encoded_AU_or_native_repair_or_encoder_input","planning_period_ms":null,"success_prediction":false,"state_prediction":false,"budget":"existing_shared_IP_admission_at_actual_sendto","parity":"existing_RNVP_XOR_data_plus_parity_bundle","repair":"existing_received_ACK_cache_and_native_TTL_filters","wire_authorization":"authorization_at_send_not_enqueue_identity"})";
    if(predict_){const std::string old="wiring_only_protect_group4_else_fresh";actionContract.replace(actionContract.find(old),old.size(),"development_cyclic_fresh_g2_g4_g8");}
    std::ofstream(path/"action_model.json")<<actionContract;
    if(scheduled_){
        if(!network_)throw std::runtime_error("scheduler requires network");
        char diagnostic[32]{};const auto n=GetEnvironmentVariableA("TR2_REACH_SCHEDULER_DIAGNOSTIC",diagnostic,32);
        if(n){if(n>=32||std::string(diagnostic)!="timeout")throw std::runtime_error("invalid scheduler diagnostic");forceTimeout_=true;}
        schedulerLog_.open(path/"scheduler.csv");schedulerCandidates_.open(path/"scheduler_candidates.csv");schedulerExecution_.open(path/"scheduler_execution.csv");
        for(auto* l:{&schedulerLog_,&schedulerCandidates_,&schedulerExecution_}){l->exceptions(std::ios::badbit|std::ios::failbit);l->precision(17);}
        schedulerLog_<<"tick,scheduled_us,start_us,snapshot_us,selection_end_us,dispatch_end_us,skipped,candidates,evaluated,selected,frame_id,kind,fallback,reason,revalidation,decision_id,selection_us,dispatch_us,injected_timeout,estimate_id,sampled_us,received_us,generated_us,notification_sequence,feedback_id,feedback_us,last_idr,queued_bytes,rate_bps\n";
        schedulerCandidates_<<"tick,candidate,frame_id,stream,generation,active_generation,capture_us,bytes,offered,remaining_ip,repair_requests,missing,local,kind,group,ip_bytes,eligible,eligibility,live,causal,reference,stage,task,status,support,runs,raw_command,calibrated_command,score";
        for(int i=0;i<16;++i)schedulerCandidates_<<",x"<<i;schedulerCandidates_<<",value\n";
        schedulerExecution_<<"tick,event_us,decision_id,frame_id,kind,status\n";
        std::ofstream(path/"scheduler_model.json")<<R"({"version":"G4-03-v1","period_us":20000,"compute_budget_us":5000,"max_candidates":64,"actual_max_candidates":22,"latest_frames":4,"horizon_us":200000,"predictor_clock_check_rows":64,"calibration_used":false,"score":"raw_target_AU_command_probability - 0.1 * IP_bytes / 65536","interpretation":"experimental short-horizon proxy under changed continuation; not causal or calibrated task success","unknown_policy":"whole decision common recovery","fallback":"earliest requested repair; requested refresh; latest fresh group4 else unprotected; defer","overrun":"cooperative cutoff; OS wait and bounded revalidation can exceed 5ms; recorded","catchup":"skip slots; never replay previous winner"})";
        std::ofstream(path/"action_model.json")<<R"({"version":"G4-03-v1","policy":"ReachScheduler_experimental_raw_proxy_with_common_recovery","planning_period_ms":20,"candidate_count_per_execution":7,"max_frame_metadata":4,"deadline_us":200000,"success_prediction":false,"budget":"existing shared IP admission at actual sendto"})";
        std::ofstream(path/"prediction_model.json")<<R"({"version":"G4-03-v1","source_model":"G4-02-v2 fixed-continuation path library","inference":"raw short-horizon ranking with support checks; changed-policy accuracy unverified","calibration_used_for_selection":false,"horizon_us":200000,"max_neighbors":64,"min_neighbors":8,"min_runs":2,"state_lifetime_us":250000,"unknown_probability":-1})";
    }
}
uint64_t ActionExecution::Record(const DecisionSnapshot& s,const std::vector<ActionTicket>& cs,size_t selected,uint32_t id){
    const auto d=++decision_;
    for(size_t i=0;i<cs.size();++i){const auto& c=cs[i];
        candidates_<<d<<','<<s.now<<','<<id<<','<<s.frame.stream<<','<<s.frame.generation<<','<<s.generation<<','<<s.frame.capture<<','<<s.frame.bytes<<','<<s.remainingIp<<','<<ActionName(c.kind)<<','<<c.group<<','<<c.ip<<','<<c.eligible<<','<<c.reason<<','<<(selected==i)<<','<<s.frame.offered<<','<<s.missing.size()<<','<<s.repairRequests<<','<<s.refreshWanted<<','<<s.refreshPending<<','<<s.lastRefresh<<',';
        for(auto chunk:s.missing)candidates_<<chunk<<';';candidates_<<'\n';
    }if(predict_&&!scheduled_)Predict(s,cs,d,id);return d;
}
bool ActionExecution::Plan(uint32_t id,uint32_t stream,uint64_t capture,uint64_t bytes,bool idr,NetworkManager::RnvpFrameProtectionOptions& protection){
    std::lock_guard lock(mutex_);const auto now=MonotonicUs();
    if(stream_!=stream){stream_=stream;frames_.clear();++generation_;}
    if(idr){++generation_;lastIdr_=id;}
    const auto requestedFrame=refresh_.PendingFrame();
    if(refresh_.Output(idr,id))events_<<now<<",idr_output,"<<id<<','<<generation_<<",0,"<<requestedFrame<<'\n';
    Entry entry;entry.frame={id,stream,generation_,capture,bytes,idr,false};frames_[id]=entry;
    while(frames_.size()>4)frames_.erase(frames_.begin());
    DecisionSnapshot s;s.now=now;s.generation=generation_;s.remainingIp=budget_.RemainingUplinkBytes();s.frame=entry.frame;s.hasFrame=true;
    s.lastRefresh=refresh_.Last();s.refreshPending=refresh_.Pending();
    const auto plans=ActionCandidates(s);size_t chosen=plans[3].eligible?3:plans[1].eligible?1:0; // group4 else unprotected, not a research performance policy
    if(predict_){const size_t explore=1+(id%4);chosen=plans[explore].eligible?explore:plans[1].eligible?1:0;}
    auto& current=frames_.at(id);current.offeredDecision=Record(s,plans,chosen,id);
    current.frame.offered=chosen!=0;current.group=plans[chosen].group;
    protection.disableFec=current.group==0;protection.forceFec=current.group!=0;protection.fecGroupChunkCountOverride=current.group;
    // Native pacing deadlines remain active; the send hook additionally enforces capture+200 ms.
    events_<<now<<','<<(chosen?"offer":"defer")<<','<<id<<','<<generation_<<','<<current.offeredDecision<<",0\n";
    return chosen!=0;
}
bool ActionExecution::Repair(uint32_t id,std::span<const uint16_t> chunks){
    // Called by RNVP only after receiving a missing request and finding actual cached bytes.
    std::lock_guard lock(mutex_);DecisionSnapshot s;s.now=MonotonicUs();s.generation=generation_;s.remainingIp=budget_.RemainingUplinkBytes();
    const auto it=frames_.find(id);s.hasFrame=it!=frames_.end();if(s.hasFrame){s.frame=it->second.frame;s.repairRequests=it->second.repairs;}
    if(scheduled_){if(it!=frames_.end())it->second.pendingMissing.assign(chunks.begin(),chunks.end());
        events_<<s.now<<",repair_queued,"<<id<<','<<s.frame.generation<<",0,0\n";return false;}
    s.missing.assign(chunks.begin(),chunks.end());const auto cs=ActionCandidates(s);const size_t chosen=cs[5].eligible?5:0;
    const auto d=Record(s,cs,chosen,id);
    if(chosen){auto& e=it->second;e.repairDecision=d;++e.repairs;e.missing.insert(chunks.begin(),chunks.end());}
    events_<<s.now<<','<<(chosen?"repair_authorized":"repair_rejected")<<','<<id<<','<<s.frame.generation<<','<<d<<",0\n";
    return chosen!=0;
}
bool ActionExecution::Request(uint32_t input,bool wanted){
    std::lock_guard lock(mutex_);refresh_.Want(wanted);if(!refresh_.Wanted())return false;
    if(scheduled_&&!refreshPermit_)return false;
    DecisionSnapshot s;s.now=MonotonicUs();s.generation=generation_;s.remainingIp=budget_.RemainingUplinkBytes();s.refreshWanted=true;s.refreshPending=refresh_.Pending();s.lastRefresh=refresh_.Last();
    if(scheduled_&&s.now-refreshPermitUs_>=200000){schedulerExecution_<<refreshPermit_<<','<<s.now<<",0,"<<input<<",REFRESH,permit_expired\n";refreshPermit_=0;return false;}
    const auto cs=ActionCandidates(s);const size_t chosen=cs[6].eligible?6:0;const auto d=Record(s,cs,chosen,input);
    if(chosen)refresh_.Issued(s.now,input);
    if(scheduled_){schedulerExecution_<<refreshPermit_<<','<<s.now<<','<<d<<','<<input<<",REFRESH,"<<(chosen?"issued":"rejected")<<'\n';refreshPermit_=0;}
    events_<<s.now<<','<<(chosen?"idr_request":"idr_suppressed")<<','<<input<<','<<generation_<<','<<d<<','<<refresh_.PendingFrame()<<'\n';
    return chosen!=0;
}
int ActionExecution::Send(SOCKET socket,std::span<const uint8_t> bytes,const sockaddr_in& address){
    // Hold the sender generation lock through the budget lock and actual sendto.
    std::lock_guard lock(mutex_);const auto now=MonotonicUs();net::RnvpHeaderV1 h{};
    if(!net::DecodeRnvpHeaderV1(bytes.data(),bytes.size(),h))return budget_.Send(true,socket,bytes,address);
    const bool media=h.packetType==static_cast<uint8_t>(net::PacketType::Data)||h.packetType==static_cast<uint8_t>(net::PacketType::Fec);
    if(!media)return budget_.Send(true,socket,bytes,address);
    const auto kind=IpPacketClass(bytes);uint64_t authorization=0,gen=0,capture=0;std::string status="metadata_unavailable";
    const auto it=frames_.find(h.frameId);
    if(it!=frames_.end()){
        const auto& e=it->second;gen=e.frame.generation;capture=e.frame.capture;authorization=e.offeredDecision;
        DecisionSnapshot s;s.now=now;s.generation=generation_;s.frame=e.frame;s.hasFrame=true;status=ActionFrameCheck(s);
        if(status=="eligible"&&h.streamId!=e.frame.stream)status="stream_changed";
        if(status=="eligible"&&!e.frame.offered)status="not_offered";
        if(net::HasPacketFlag(h.flags,net::PacketFlag_Retransmit)){
            authorization=e.repairDecision;
            if(status=="eligible"&&(!authorization||!e.missing.count(h.chunkIndex)))status="repair_not_authorized";
        }else if(status=="eligible"&&h.packetType==static_cast<uint8_t>(net::PacketType::Fec)&&!e.group)status="fec_not_selected";
    }
    const int result=status=="eligible"?budget_.Send(true,socket,bytes,address,capture+200000):0;
    if(status=="eligible")status=result==static_cast<int>(bytes.size())?"sent":result==BudgetTransport::ExpiredBeforeAdmission?"expired_during_budget_wait":result==0?"budget_rejected":"send_error";
    wire_<<now<<','<<h.frameId<<','<<h.streamId<<','<<h.chunkIndex<<','<<kind<<','<<authorization<<','<<gen<<','<<generation_<<','<<capture<<','<<bytes.size()+28<<','<<result<<','<<status<<'\n';
    return result==BudgetTransport::ExpiredBeforeAdmission?0:result;
}
void ActionExecution::Close(){StopScheduler();std::lock_guard lock(mutex_);if(closed_)return;closed_=true;
    if(refresh_.Pending())events_<<MonotonicUs()<<",idr_unresolved,"<<refresh_.PendingFrame()<<','<<generation_<<",0,0\n";
    candidates_.close();events_.close();wire_.close();if(predict_){predictions_.close();predictionFeedback_.close();}
    if(scheduled_){schedulerLog_.close();schedulerCandidates_.close();schedulerExecution_.close();}}
void ActionExecution::PublishState(const SenderStateEstimate& state){if(!predict_)return;std::lock_guard lock(mutex_);state_=state;}
void ActionExecution::Feedback(std::span<const uint8_t> bytes){
    if(!predict_)return;std::lock_guard lock(mutex_);feedbackUs_=MonotonicUs();++feedbackId_;
    net::RnvpHeaderV1 h{};
    if(net::DecodeRnvpHeaderV1(bytes.data(),bytes.size(),h)&&h.headerSize+h.payloadSize==bytes.size()&&h.packetType==static_cast<uint8_t>(net::PacketType::TransportFeedback)){
        net::TransportFeedbackPayload f;if(net::DecodeTransportFeedbackPayload(bytes.data()+h.headerSize,h.payloadSize,f))for(auto e:f.entries){
            const auto seq=uint64_t(f.baseSequence)+e.sequenceDelta;if(seq<=lastSequence_)continue;
            if(!(e.flags&(net::TransportFeedbackFlag_Missing|net::TransportFeedbackFlag_Received)))continue;
            const bool missing=(e.flags&net::TransportFeedbackFlag_Missing)!=0;loss_=.95*loss_+.05*missing;burst_=.95*burst_+.05*(missing&&lastMissing_);lastMissing_=missing;lastSequence_=seq;
        }
    }predictionFeedback_<<feedbackId_<<','<<feedbackUs_<<','<<Hex(bytes)<<','<<loss_<<','<<burst_<<','<<lastSequence_<<'\n';
}
void ActionExecution::Predict(const DecisionSnapshot& s,const std::vector<ActionTicket>& cs,uint64_t d,uint32_t id){
    const auto pace=network_->GetPacingStats();const auto& r=state_.report;
    const bool causal=state_.sampledUs<=s.now&&state_.receivedUs<=s.now&&r.generatedUs<=s.now&&feedbackUs_<=s.now;
    const auto age=r.generatedUs&&causal?s.now-r.generatedUs:UINT64_MAX;
    const bool live=r.sequence&&causal&&age<250000&&(!s.frame.stream||r.stream==s.frame.stream);
    const int reference=r.decodedFrame<lastIdr_?4:static_cast<int>(r.reference);
    const double rate=std::max<uint32_t>(1,pace.targetBitrateBps);
    for(size_t i=0;i<cs.size();++i){const auto& c=cs[i];PredictionInput q;
        q.kind=static_cast<int>(c.kind);q.group=c.group;q.idr=s.frame.idr;q.reference=reference;q.stage=r.taskStage;q.task=task_;q.live=live;q.causal=causal;q.eligible=c.eligible;
        q.x={s.hasFrame?double(s.now-s.frame.capture)/1000:0,double(c.ip)/1000,double(pace.queuedPayloadBytes)*8000/rate,double(c.ip)*8000/rate,loss_,burst_,feedbackUs_?double(s.now-feedbackUs_)/1000:1000,live?double(age)/1000:1000,
          r.observationCaptureUs&&causal?double(s.now-r.observationCaptureUs)/1000:1000,r.taskDeadlineUs>s.now?double(r.taskDeadlineUs-s.now)/1000000:0,double(r.xMicro)/1000000,double(r.yMicro)/1000000,double(r.yawMicro)/1000000,double(r.positionErrorMicro)/1000000,double(r.decodeWaitUs)/1000,double(s.missing.size())};
        const auto p=predictor_.Predict(q);
        predictions_<<d<<','<<s.now<<','<<id<<','<<i<<','<<q.kind<<','<<q.group<<','<<q.idr<<','<<q.reference<<','<<q.stage<<','<<q.task<<','<<state_.estimateId<<','<<state_.sampledUs<<','<<state_.receivedUs<<','<<r.generatedUs<<','<<r.sequence<<','<<r.stream<<','<<r.decodedFrame<<','<<lastIdr_<<','<<feedbackId_<<','<<feedbackUs_<<','<<pace.queuedPayloadBytes<<','<<pace.targetBitrateBps<<','<<q.eligible<<','<<live<<','<<causal;
        for(auto x:q.x)predictions_<<','<<x;predictions_<<','<<p.status<<','<<p.support<<','<<p.runs<<','<<p.distance<<','<<p.terminalSupported;
        for(size_t j=0;j<6;++j)predictions_<<','<<p.raw[j]<<','<<p.probability[j];
        for(size_t j=0;j<3;++j)predictions_<<','<<p.p50[j]<<','<<p.p90[j];predictions_<<'\n';
    }
}
ActionExecution::~ActionExecution(){StopScheduler();}
void ActionExecution::Queue(uint32_t id,uint32_t stream,uint64_t capture,std::vector<uint8_t> payload,bool idr,bool dropped){
    std::lock_guard lock(mutex_);const auto now=MonotonicUs();
    if(stream_!=stream){stream_=stream;frames_.clear();++generation_;}
    if(idr){++generation_;lastIdr_=id;}
    const auto requested=refresh_.PendingFrame();
    if(refresh_.Output(idr,id))events_<<now<<",idr_output,"<<id<<','<<generation_<<",0,"<<requested<<'\n';
    Entry e;e.frame={id,stream,generation_,capture,payload.size(),idr,false};
    if(!dropped)e.payload=std::make_shared<const std::vector<uint8_t>>(std::move(payload));
    frames_[id]=std::move(e);while(frames_.size()>4)frames_.erase(frames_.begin());
    // Sender generation changes on actual output, even for a deliberately omitted AU.
    events_<<now<<",defer,"<<id<<','<<generation_<<",0,0\n";
}
void ActionExecution::StartScheduler(uint64_t origin,uint64_t duration){
    if(!scheduled_)return;if(schedulerThread_.joinable())throw std::runtime_error("scheduler already running");
    schedulerStop_=false;schedulerThread_=std::thread([this,origin,duration](){try{
        struct Timer {HANDLE h=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);~Timer(){if(h)CloseHandle(h);}} timer;
        if(!timer.h)throw std::runtime_error("high-resolution scheduler timer unavailable");
        uint64_t next=1;
        while(!schedulerStop_){const auto due=origin+next*SchedulerPeriodUs;if(due>=origin+duration)break;
            const auto beforeWait=MonotonicUs();if(beforeWait<due){LARGE_INTEGER relative;relative.QuadPart=-static_cast<LONGLONG>((due-beforeWait)*10);
                if(!SetWaitableTimerEx(timer.h,&relative,0,nullptr,nullptr,nullptr,0)||WaitForSingleObject(timer.h,1000)!=WAIT_OBJECT_0)throw std::runtime_error("scheduler timer wait failed");}
            if(schedulerStop_)break;const auto now=MonotonicUs();if(now>=origin+duration)break;if(now<due)continue;
            const auto slot=SchedulerSlot(origin,now);const auto skipped=slot>next?slot-next:0;
            Tick(slot,origin+slot*SchedulerPeriodUs,skipped);next=slot+1;
        }
    }catch(const std::exception& e){std::lock_guard lock(mutex_);schedulerError_=e.what();}});
}
void ActionExecution::StopScheduler(){schedulerStop_=true;if(schedulerThread_.joinable())schedulerThread_.join();}
void ActionExecution::CheckScheduler(){std::lock_guard lock(mutex_);if(!schedulerError_.empty())throw std::runtime_error("scheduler: "+schedulerError_);}
void ActionExecution::Tick(uint64_t tick,uint64_t due,uint64_t skipped){
    const auto started=MonotonicUs();std::vector<SchedulerCandidate> cs;cs.reserve(22);
    SenderStateEstimate state;uint64_t feedback=0,feedbackUs=0,snapshot=0;uint32_t lastIdr=0;double loss=0,burst=0;
    const auto pace=network_->GetPacingStats();
    {
        std::lock_guard lock(mutex_);snapshot=MonotonicUs();state=state_;feedback=feedbackId_;feedbackUs=feedbackUs_;lastIdr=lastIdr_;loss=loss_;burst=burst_;
        DecisionSnapshot global;global.now=snapshot;global.generation=generation_;global.remainingIp=budget_.RemainingUplinkBytes();
        global.refreshWanted=refresh_.Wanted();global.refreshPending=refresh_.Pending()||refreshPermit_!=0;global.lastRefresh=refresh_.Last();
        const auto base=ActionCandidates(global);SchedulerCandidate defer;defer.snapshot=global;defer.action=base[0];cs.push_back(defer);
        for(const auto& [id,e]:frames_){auto s=global;s.hasFrame=true;s.frame=e.frame;s.repairRequests=e.repairs;s.missing=e.pendingMissing;
            auto plans=ActionCandidates(s);
            for(size_t i=1;i<=5;++i){SchedulerCandidate c;c.snapshot=s;c.local=i;c.action=plans[i];
                if(!e.payload&&i<=4){c.action.eligible=false;c.action.reason="diagnostic_omission";}
                cs.push_back(std::move(c));}
        }
        SchedulerCandidate refresh;refresh.snapshot=global;refresh.local=6;refresh.action=base[6];cs.push_back(refresh);
    }
    const auto& r=state.report;
    const bool causal=state.sampledUs<=snapshot&&state.receivedUs<=snapshot&&r.generatedUs<=snapshot&&feedbackUs<=snapshot&&r.observationCaptureUs<=snapshot;
    const auto age=r.generatedUs&&causal?snapshot-r.generatedUs:UINT64_MAX;
    const double rate=std::max<uint32_t>(1,pace.targetBitrateBps);
    for(auto& c:cs){const auto& s=c.snapshot;auto& q=c.input;
        const bool live=r.sequence&&causal&&age<250000&&(!s.frame.stream||r.stream==s.frame.stream);
        q.kind=static_cast<int>(c.action.kind);q.group=c.action.group;q.idr=s.frame.idr;q.reference=r.decodedFrame<lastIdr?4:static_cast<int>(r.reference);
        q.stage=r.taskStage;q.task=task_;q.live=live;q.causal=causal;q.eligible=c.action.eligible;
        q.x={s.hasFrame&&snapshot>=s.frame.capture?double(snapshot-s.frame.capture)/1000:0,double(c.action.ip)/1000,double(pace.queuedPayloadBytes)*8000/rate,double(c.action.ip)*8000/rate,loss,burst,
            feedbackUs&&feedbackUs<=snapshot?double(snapshot-feedbackUs)/1000:1000,live?double(age)/1000:1000,
            r.observationCaptureUs&&causal?double(snapshot-r.observationCaptureUs)/1000:1000,r.taskDeadlineUs>snapshot?double(r.taskDeadlineUs-snapshot)/1000000:0,
            double(r.xMicro)/1000000,double(r.yMicro)/1000000,double(r.yawMicro)/1000000,double(r.positionErrorMicro)/1000000,double(r.decodeWaitUs)/1000,double(s.missing.size())};
        c.prediction.status=c.action.eligible?"not_evaluated":"ineligible";
    }
    const auto choice=SelectReachAction(cs,predictor_,[&](){return forceTimeout_||MonotonicUs()-started>=SchedulerBudgetUs;},schedulerOptions_);
    const auto& chosen=cs[choice.index];uint64_t actionId=0;std::string validation="eligible";size_t actual=chosen.local;
    std::shared_ptr<const std::vector<uint8_t>> payload;std::vector<uint16_t> missing;auto frame=chosen.snapshot.frame;
    uint64_t selectionEnd=0;
    {
        std::lock_guard lock(mutex_);DecisionSnapshot fresh=chosen.snapshot;fresh.now=MonotonicUs();fresh.generation=generation_;fresh.remainingIp=budget_.RemainingUplinkBytes();
        fresh.refreshWanted=refresh_.Wanted();fresh.refreshPending=refresh_.Pending()||refreshPermit_!=0;fresh.lastRefresh=refresh_.Last();
        auto it=frames_.find(frame.id);fresh.hasFrame=it!=frames_.end();
        if(fresh.hasFrame){fresh.frame=it->second.frame;fresh.repairRequests=it->second.repairs;}
        // Keep exactly the received missing request evaluated at this tick; never replace with future information.
        const auto plans=ActionCandidates(fresh);
        if(!plans[actual].eligible){validation=plans[actual].reason;actual=0;}
        // Lock acquisition can consume the remaining budget after inference finished.
        // Never execute a prediction winner whose computation has already expired.
        if(!choice.fallback&&MonotonicUs()-started>=SchedulerBudgetUs){validation="compute_budget_during_revalidation";actual=0;}
        if(actual>=1&&actual<=4&&(!fresh.hasFrame||!it->second.payload)){validation="payload_unavailable";actual=0;}
        if(actual==6){refreshPermit_=tick;refreshPermitUs_=fresh.now;}
        else{
            actionId=Record(fresh,plans,actual,frame.id);
            if(actual>=1&&actual<=4){auto& e=it->second;e.offeredDecision=actionId;e.frame.offered=true;e.group=plans[actual].group;payload=e.payload;frame=e.frame;
                events_<<fresh.now<<",offer,"<<frame.id<<','<<generation_<<','<<actionId<<",0\n";
            }else if(actual==5){auto& e=it->second;e.repairDecision=actionId;++e.repairs;e.missing.insert(fresh.missing.begin(),fresh.missing.end());
                missing=fresh.missing;if(e.pendingMissing==missing)e.pendingMissing.clear();
                events_<<fresh.now<<",repair_authorized,"<<frame.id<<','<<generation_<<','<<actionId<<",0\n";
            }
        }
        selectionEnd=MonotonicUs();
        schedulerExecution_<<tick<<','<<fresh.now<<','<<actionId<<','<<frame.id<<','<<ActionName(plans[actual].kind)<<','<<(actual==6?"reserved":"authorized")<<'\n';
    }
    if(payload){NetworkManager::RnvpFrameProtectionOptions p;p.disableFec=chosen.action.group==0;p.forceFec=chosen.action.group!=0;p.fecGroupChunkCountOverride=chosen.action.group;
        network_->SendRNVPFragmented(*payload,frame.id,net::CodecType::H264,frame.stream,frame.idr,p);
    }else if(actual==5)network_->SendResearchRepair(frame.stream,frame.id,missing);
    const auto dispatchEnd=MonotonicUs();
    schedulerLog_<<tick<<','<<due<<','<<started<<','<<snapshot<<','<<selectionEnd<<','<<dispatchEnd<<','<<skipped<<','<<cs.size()<<','<<choice.evaluated<<','<<choice.index<<','<<chosen.snapshot.frame.id<<','<<ActionName(chosen.action.kind)<<','<<choice.fallback<<','<<choice.reason<<','<<validation<<','<<actionId<<','<<selectionEnd-started<<','<<dispatchEnd-selectionEnd<<','<<forceTimeout_<<','<<state.estimateId<<','<<state.sampledUs<<','<<state.receivedUs<<','<<r.generatedUs<<','<<r.sequence<<','<<feedback<<','<<feedbackUs<<','<<lastIdr<<','<<pace.queuedPayloadBytes<<','<<pace.targetBitrateBps<<'\n';
    for(size_t i=0;i<cs.size();++i){const auto& c=cs[i];const auto& s=c.snapshot;const auto& q=c.input;const auto& p=c.prediction;
        schedulerCandidates_<<tick<<','<<i<<','<<s.frame.id<<','<<s.frame.stream<<','<<s.frame.generation<<','<<s.generation<<','<<s.frame.capture<<','<<s.frame.bytes<<','<<s.frame.offered<<','<<s.remainingIp<<','<<s.repairRequests<<',';
        for(auto chunk:s.missing)schedulerCandidates_<<chunk<<';';
        schedulerCandidates_<<','<<c.local<<','<<ActionName(c.action.kind)<<','<<c.action.group<<','<<c.action.ip<<','<<c.action.eligible<<','<<c.action.reason<<','<<q.live<<','<<q.causal<<','<<q.reference<<','<<q.stage<<','<<q.task<<','<<p.status<<','<<p.support<<','<<p.runs<<','<<p.raw[3]<<','<<p.probability[3]<<',';
        if(std::isfinite(c.score))schedulerCandidates_<<c.score;else schedulerCandidates_<<"unknown";
        for(auto x:q.x)schedulerCandidates_<<','<<x;schedulerCandidates_<<','<<c.value<<'\n';
    }
}
}
