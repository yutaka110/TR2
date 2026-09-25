#pragma once
#include "ReachFoundation.h"
#include "ReachVisualControl.h"
#include "ReachStateProtocol.h"
#include "../network/DecodeTrace.h"
#include "../network/FrameReassembler.h"
#include <map>

namespace reach {
// Receiver-local state and audit sink. Only a serialized notification may expose
// its snapshot to a sender. All producers share this lock, not the control lock.
// Sequence denotes journal order; event_us remains the actual source clock.
class DecodeJournal {
public:
    explicit DecodeJournal(const std::filesystem::path& path){
        events_.open(path/"decode_events.csv");events_.exceptions(std::ios::badbit|std::ios::failbit);
        chunks_.open(path/"reassembly_events.csv");chunks_.exceptions(std::ios::badbit|std::ios::failbit);
        events_<<"sequence,event_us,event,frame_id,stream_id,capture_us,generation,idr,trusted,reason,state,last_complete_frame,last_decoded_frame,last_control_frame,display_deadline_us,display_timely,decoder_input_us,decoder_wait_us,task_deadline_us,command_deadline_us,command_sequence\n";
        chunks_<<"event_us,event,outcome,frame_id,stream_id,total_chunks,received_chunks,missing_chunks,missing_indices,indices_complete,first_receive_us,send_us,recovery_deadline_us\n";
        std::ofstream(path/"decode_model.json")<<R"({"version":"G2-04-v1","reference_model":"conservative_actual_IDR_generation","display_age_limit_us":200000,"control_observation_age_limit_us":200000,"command_lifetime_us":100000,"late_replay_supported":false,"reference_deadline":"actual reassembler recovery deadline plus decoder sequence/flush eligibility; 0 means not assigned or unknown, never unlimited","decoder_wait":"accepted ProcessInput to real pixel output","error_concealment_detection":"reported corruption and conservative continuity; unreported concealment is not proven absent","sender_access_to_journal":false})";
    }
    void Start(uint64_t origin,uint64_t duration){std::lock_guard lock(mutex_);taskDeadline_=origin+duration;}
    void Event(const net::DecodeTraceEvent& e,uint64_t commandDeadline=0,uint64_t commandSequence=0){
        std::lock_guard lock(mutex_);const std::string kind=e.event;
        if(kind=="reassembled")lastComplete_=e.frameId;
        if(kind=="reference_uncertain")state_="ReferenceUncertain";
        if(kind=="recovery_pending")state_="RecoveryPending";
        if(kind=="input_accepted"){
            generations_[e.frameId]=e.generation;
            if(e.idr){generation_=e.generation;state_="RecoveryPending";}
        }
        if(kind=="decoded_output"){
            lastDecoded_=e.frameId;
            if(e.trusted&&e.generation==generation_)state_="Synchronized";
            else if(!e.trusted)state_="ReferenceUncertain";
        }
        if(kind=="control_used")lastControl_=e.frameId;
        if(kind=="reference_uncertain"||kind=="recovery_pending"||kind=="input_accepted"||kind=="decoded_output")report_.stateEventUs=e.timeUs;
        if(kind=="decoded_output"){report_.decodedCaptureUs=e.captureUs;report_.decodeWaitUs=e.inputUs&&e.timeUs>=e.inputUs?e.timeUs-e.inputUs:0;}
        const auto display=e.captureUs?e.captureUs+200000:0;
        events_<<++sequence_<<','<<e.timeUs<<','<<e.event<<','<<e.frameId<<','<<e.streamId<<','<<e.captureUs<<','
            <<e.generation<<','<<e.idr<<','<<e.trusted<<','<<e.reason<<','<<state_<<','<<lastComplete_<<','<<lastDecoded_<<','<<lastControl_<<','
            <<display<<','<<(display&&e.timeUs>=e.captureUs&&e.timeUs<=display)<<','<<e.inputUs<<','
            <<(e.inputUs&&e.timeUs>=e.inputUs?e.timeUs-e.inputUs:0)<<','<<taskDeadline_<<','<<commandDeadline<<','<<commandSequence<<'\n';
    }
    void Chunks(const net::FrameAckInfo& a,const char* event,const char* outcome,uint64_t now){
        std::lock_guard lock(mutex_);
        report_.missingFrame=a.frameId;report_.missingChunks=a.missingChunkCount;report_.missingSnapshotUs=now;report_.recoveryDeadlineUs=a.recoveryDeadlineUs;report_.flags|=4;
        chunks_<<now<<','<<event<<','<<outcome<<','<<a.frameId<<','<<a.streamId<<','<<a.chunkCount<<','<<a.receivedChunkCount<<','<<a.missingChunkCount<<',';
        for(size_t n=0;n<a.missingChunkIndices.size();++n){if(n)chunks_<<';';chunks_<<a.missingChunkIndices[n];}
        chunks_<<','<<(a.missingChunkIndices.size()==a.missingChunkCount)<<','<<a.firstReceiveUs<<','<<a.sendUs<<','<<a.recoveryDeadlineUs<<'\n';
    }
    void Control(const MotionCommand& c){
        uint64_t generation=0;{std::lock_guard lock(mutex_);auto it=generations_.find(c.sourceFrameId);if(it!=generations_.end())generation=it->second;}
        // sourceFrameId alone is not evidence of use: early stop paths also copy it.
        const bool used=c.observationUsed;
        Event({used?"control_used":"control_rejected",c.sourceFrameId,c.sourceStreamId,c.generatedUs,c.sourceCaptureUs,generation,0,false,used,c.reason.c_str()},c.validUntilUs,c.sequence);
        {std::lock_guard lock(mutex_);report_.taskStage=c.state=="HOLD"?3:c.state=="ALIGN"?2:c.state=="APPROACH"?1:0;
            report_.controlSequence=c.sequence;report_.controlGeneratedUs=c.generatedUs;report_.commandSourceFrame=c.sourceFrameId;
            if(c.estimatedComplete)report_.flags|=2;else report_.flags&=~2u;}
    }
    void Observation(const VisualObservation& o){
        std::lock_guard lock(mutex_);report_.observationFrame=o.frameId;report_.observationCaptureUs=o.captureUs;report_.observationReceivedUs=o.receivedUs;
        if(o.valid){report_.flags|=1;report_.xMicro=int32_t(std::llround(o.x*1000000));report_.yMicro=int32_t(std::llround(o.y*1000000));report_.yawMicro=int32_t(std::llround(o.yaw*1000000));
            report_.positionErrorMicro=uint32_t(std::ceil(o.positionErrorM*1000000));report_.yawErrorMicro=uint32_t(std::ceil(o.yawErrorRad*1000000));}
        else {report_.flags&=~3u;report_.xMicro=report_.yMicro=report_.yawMicro=0;report_.positionErrorMicro=report_.yawErrorMicro=0;}
    }
    StateReport NotificationSnapshot(uint32_t stream){
        std::lock_guard lock(mutex_);auto r=report_;r.generatedUs=MonotonicUs();r.taskDeadlineUs=taskDeadline_;r.stream=stream;
        r.completeFrame=lastComplete_;r.decodedFrame=lastDecoded_;r.controlFrame=lastControl_;r.generation=generation_;
        r.receiverEventSequence=static_cast<uint32_t>(sequence_);
        r.reference=state_=="Synchronized"?ReceiverReference::Synchronized:state_=="ReferenceUncertain"?ReceiverReference::ReferenceUncertain:state_=="RecoveryPending"?ReceiverReference::RecoveryPending:ReceiverReference::AwaitingRandomAccess;
        r.remainingTaskUs=taskDeadline_>r.generatedUs?taskDeadline_-r.generatedUs:0;
        if(!r.observationCaptureUs||r.generatedUs-r.observationCaptureUs>200000)r.flags&=~3u;
        if(!(r.flags&1))r.flags&=~2u;
        return r;
    }
    void Close(){std::lock_guard lock(mutex_);events_.close();chunks_.close();}
private:
    std::mutex mutex_;BufferedLog events_,chunks_;
    uint64_t sequence_=0,generation_=0,taskDeadline_=0;
    uint32_t lastComplete_=0,lastDecoded_=0,lastControl_=0;
    std::string state_="AwaitingRandomAccess";
    std::map<uint32_t,uint64_t> generations_;
    StateReport report_;
};
}
