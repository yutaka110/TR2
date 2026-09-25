#define NOMINMAX
#include "ReachBaseline.h"
#include "ReachJson.h"
#include <iomanip>
namespace reach {
namespace {std::string Hex(std::span<const uint8_t> b){std::string s;for(auto c:b){s+="0123456789abcdef"[c>>4];s+="0123456789abcdef"[c&15];}return s;}}
Baseline::Baseline(std::string mode,double lambda,uint32_t pacingCeiling,const std::filesystem::path& path):mode_(std::move(mode)),lambda_(lambda),ceiling_(pacingCeiling){
    feedback_.open(path/"baseline_feedback.csv");decisions_.open(path/"baseline_decisions.csv");candidates_.open(path/"baseline_candidates.csv");repairs_.open(path/"baseline_repairs.csv");
    states_.open(path/"baseline_state.csv");
    for(auto* log:{&feedback_,&decisions_,&candidates_,&repairs_,&states_}){log->exceptions(std::ios::badbit|std::ios::failbit);log->precision(17);}
    states_<<"state_id,published_us,estimate_id,sampled_us,received_us,generated_us,sequence,mask,reference,generation,decode_wait_us,stage,observation_capture_us,position_error_micro,deadline_us\n";
    feedback_<<"event_id,received_us,wire_hex,loss,burst,last_sequence,nacks,rtt_us\n";
    decisions_<<"frame_id,event_us,capture_us,payload_bytes,feedback_id,last_feedback_us,loss,burst,rtt_us,queue_us,rate_bps,lambda,selected,group,rounds,defer,decision_us,queued_payload_bytes,historical_queue_us,effective_lambda,probe,previous_offer_us,decision_start_us,state_id,state_value,state_live,idr,b1_selected\n";
    candidates_<<"frame_id,candidate,group,rounds,initial_ip,expected_ip,success,cost,feasible,defer\n";
    repairs_<<"event_us,frame_id,feedback_id,capture_us,chunks,repair_request,rounds,rtt_us,queue_us,allowed,queued_payload_bytes,queue_rate_bps\n";
    std::ofstream(path/"baseline_model.json")<<"{\"version\":\"G3-02-v1\",\"idle_probe_us\":500000,\"probe_pacing_ceiling\":true,\"probe_cost_lambda\":0,\"queue_model\":\"live queued UDP payload bytes / current pacing rate; no historical residence time\",\"mode\":"<<JsonString(mode_)<<",\"lambda\":"<<lambda_<<",\"pacing_ceiling_bps\":"<<ceiling_
      <<R"(,"cache_frames":24,"deadline_us":200000,"guard_us":8000,"candidate_count":13,"fec_groups":[0,2,4,8],"repair_request_limits":[0,1,2],"loss_model":"EWMA received transport statuses plus burst inflation; XOR group independence approximation","unavailable_B0_adaptive_inputs":"receiver deadline expiry and FEC recovery counters are zero: unavailable remotely","reference_task_inputs":true,"state_input_contract":"B1 none; B2 decoder-only; B3 task-only; masked received RSTA, 250 ms lifetime","state_value_model":"bounded urgency/dependency heuristic v1; probe overrides value to one","network_trace_input":false,"codec_bitrate_adaptation":false,"keyframe_request":"common received RNVP control plus fixed input 45","repair_backend":"shared RNVP cache, TTL, priority and FEC rescue filters; B1 additionally caps requests by plan","comparison_scope":"RNVP/XOR deadline baseline; not a Hairpin or Tooth reproduction"})";
}
void Baseline::Feedback(std::span<const uint8_t> bytes){
    std::lock_guard lock(mutex_);const auto now=MonotonicUs();++feedbackId_;lastFeedbackUs_=now;
    net::RnvpHeaderV1 h{};
    if(net::DecodeRnvpHeaderV1(bytes.data(),bytes.size(),h)&&h.headerSize+h.payloadSize==bytes.size()){
        const auto* payload=bytes.data()+h.headerSize;
        if(h.packetType==static_cast<uint8_t>(net::PacketType::Pong)&&h.payloadSize==16){
            uint64_t sent=0;for(unsigned i=0;i<8;++i)sent=(sent<<8)|payload[i];
            if(sent&&sent<=now)rttUs_=.875*rttUs_+.125*static_cast<double>(now-sent);
        }
        if(h.packetType==static_cast<uint8_t>(net::PacketType::Ack)){net::AckPayload a;if(net::DecodeAckPayload(payload,h.payloadSize,a)&&a.missingChunkCount)++nacks_;}
        if(h.packetType==static_cast<uint8_t>(net::PacketType::TransportFeedback)){
            net::TransportFeedbackPayload f;if(net::DecodeTransportFeedbackPayload(payload,h.payloadSize,f))for(auto e:f.entries){
                const uint64_t seq=uint64_t(f.baseSequence)+e.sequenceDelta;
                if(seq<=lastSequence_)continue;
                const bool missing=(e.flags&net::TransportFeedbackFlag_Missing)!=0;
                if(!(e.flags&(net::TransportFeedbackFlag_Missing|net::TransportFeedbackFlag_Received)))continue;
                loss_=.95*loss_+.05*missing;
                burst_=.95*burst_+.05*(missing&&lastMissing_);
                lastMissing_=missing;lastSequence_=seq;
            }
        }
    }
    feedback_<<feedbackId_<<','<<now<<','<<Hex(bytes)<<','<<loss_<<','<<burst_<<','<<lastSequence_<<','<<nacks_<<','<<rttUs_<<'\n';
}
void Baseline::PublishState(const BaselineState& state){
    if(mode_!="B2"&&mode_!="B3")return;
    if(state.mask!=(mode_=="B2"?1:2))throw std::runtime_error("wrong baseline state mask");
    std::lock_guard lock(mutex_);state_=state;
    states_<<++stateId_<<','<<MonotonicUs()<<','<<state.estimateId<<','<<state.sampledUs<<','<<state.receivedUs<<','<<state.generatedUs<<','<<state.sequence<<','<<state.mask<<','<<state.reference<<','<<state.generation<<','<<state.decodeWaitUs<<','<<state.stage<<','<<state.observationCaptureUs<<','<<state.positionErrorMicro<<','<<state.deadlineUs<<'\n';
}
bool Baseline::Plan(uint32_t id,uint64_t capture,uint64_t bytes,bool idr,NetworkManager& sender,NetworkManager::RnvpFrameProtectionOptions& protection){
    const auto start=MonotonicUs();if(start>=nextPing_){sender.SendRNVPPing();nextPing_=start+250000;}
    const auto bw=sender.GetBandwidthEstimatorStats();const auto pace=sender.GetPacingStats();
    const auto previousOffer=lastOfferUs_;const bool probe=mode_!="B0"&&BaselineProbeDue(start,previousOffer);
    const uint32_t rate=(probe?ceiling_:std::clamp(bw.estimatedBandwidthBps,100000u,ceiling_))/1000*1000;sender.SetPacingTargetBitrateKbps(rate/1000);
    std::lock_guard lock(mutex_);const uint64_t now=MonotonicUs();
    BaselineInput input{now,capture,bytes,BaselineQueueUs(pace.queuedPayloadBytes,rate),static_cast<uint64_t>(rttUs_),rate,loss_,loss_>1e-6?std::clamp(burst_/loss_,0.,1.):0.,probe?0.:lambda_};
    const double value=probe?1.:BaselineStateValue(state_,idr,now);
    BaselineCandidate chosen;size_t selected=0,b1selected=0;
    if(mode_!="B0"){
        const auto plans=StateBaselineCandidates(input,value);selected=ChooseBaseline(plans);chosen=plans[selected];
        b1selected=ChooseBaseline(BaselineCandidates(input));
        for(size_t i=0;i<plans.size();++i){const auto& c=plans[i];candidates_<<id<<','<<i<<','<<c.group<<','<<c.rounds<<','<<c.initialIp<<','<<c.expectedIp<<','<<c.success<<','<<c.cost<<','<<c.feasible<<','<<c.defer<<'\n';}
        protection.forceFec=chosen.group!=0;protection.disableFec=chosen.group==0;protection.fecGroupChunkCountOverride=chosen.group;
    }else{
        sender.UpdateAdaptiveFec(loss_,sender.GetLastAckMissingRate(),nacks_,0,0,0,rate,1500,input.queueUs/1000.);
        chosen.group=sender.IsFecEnabled()?sender.GetFecGroupChunkCount():0;chosen.rounds=3;
    }
    frames_[id]={capture,bytes,chosen.rounds};order_.push_back(id);while(order_.size()>24){frames_.erase(order_.front());order_.pop_front();}
    decisions_<<id<<','<<now<<','<<capture<<','<<bytes<<','<<feedbackId_<<','<<lastFeedbackUs_<<','<<input.loss<<','<<input.burst<<','<<input.rttUs<<','<<input.queueUs<<','<<rate<<','<<lambda_<<','<<selected<<','<<chosen.group<<','<<chosen.rounds<<','<<chosen.defer<<','<<MonotonicUs()-start<<','<<pace.queuedPayloadBytes<<','<<static_cast<uint64_t>(std::max(0.,pace.currentQueueDelayMs)*1000)<<','<<input.lambda<<','<<probe<<','<<previousOffer<<','<<start<<','<<stateId_<<','<<value<<','<<BaselineStateLive(state_,now)<<','<<idr<<','<<b1selected<<'\n';
    if(!chosen.defer)lastOfferUs_=now;
    return !chosen.defer;
}
bool Baseline::Repair(uint32_t id,std::span<const uint16_t> chunks,NetworkManager& sender){
    const auto pace=sender.GetPacingStats();std::lock_guard lock(mutex_);
    const auto now=MonotonicUs();auto it=frames_.find(id);if(it==frames_.end())return mode_=="B0";
    auto& f=it->second;const auto q=BaselineQueueUs(pace.queuedPayloadBytes,pace.targetBitrateBps),r=static_cast<uint64_t>(rttUs_);
    ++f.repairs;const bool allow=mode_=="B0"||(f.repairs<=f.rounds&&now+q+r/2+8000<=f.capture+200000);
    repairs_<<now<<','<<id<<','<<feedbackId_<<','<<f.capture<<','<<chunks.size()<<','<<f.repairs<<','<<f.rounds<<','<<r<<','<<q<<','<<allow<<','<<pace.queuedPayloadBytes<<','<<pace.targetBitrateBps<<'\n';return allow;
}
void Baseline::Close(){std::lock_guard lock(mutex_);feedback_.close();decisions_.close();candidates_.close();repairs_.close();states_.close();}
}
