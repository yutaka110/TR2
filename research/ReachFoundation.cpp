#define NOMINMAX
#include "ReachFoundation.h"
#include "ReachJson.h"
#include <Windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")

namespace reach {
namespace {
void Keys(const Json& object, std::initializer_list<const char*> expected) {
    std::set<std::string> allowed;
    for(auto key:expected) allowed.insert(key);
    for(const auto& [key,value]:object.ObjectValue())
        if(!allowed.count(key)) throw std::runtime_error("unknown config key: " + key);
    for(const auto& key:allowed) object.At(key);
}
double Number(const Json& obj, const char* key, double lo, double hi, bool integer = false) {
    const double value=obj.At(key).NumberValue();
    if(value<lo||value>hi||(integer&&std::floor(value)!=value))
        throw std::runtime_error(std::string("out of range config key: ")+key);
    return value;
}
void WriteFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary|std::ios::trunc);
    file.exceptions(std::ios::badbit|std::ios::failbit);
    file.write(text.data(),static_cast<std::streamsize>(text.size())); file.close();
}
std::string UtcNow() {
    SYSTEMTIME value{}; GetSystemTime(&value);
    char result[40]{};
    sprintf_s(result,"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",value.wYear,value.wMonth,value.wDay,
              value.wHour,value.wMinute,value.wSecond,value.wMilliseconds);
    return result;
}
std::string NewId() {
    GUID guid{}; if(FAILED(CoCreateGuid(&guid))) throw std::runtime_error("session GUID creation failed");
    wchar_t text[40]{}; StringFromGUID2(guid,text,40);
    std::wstring w(text);
    std::string result;
    for(size_t i=1;i+1<w.size();++i) result+=static_cast<char>(w[i]); // GUID text is ASCII.
    return result;
}
}
std::string PathUtf8(const std::filesystem::path& path) {
    const auto value=path.u8string(); return std::string(value.begin(),value.end());
}
std::string ReadFile(const std::filesystem::path& path, uint64_t maxBytes) {
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) throw std::runtime_error("cannot read file: " + PathUtf8(path));
    const auto size=file.tellg();
    if(size<0||static_cast<uint64_t>(size)>maxBytes) throw std::runtime_error("file size exceeds limit: " + PathUtf8(path));
    std::string result(static_cast<size_t>(size),'\0'); file.seekg(0);
    if(!result.empty()&&!file.read(result.data(),static_cast<std::streamsize>(result.size()))) throw std::runtime_error("file read failed");
    return result;
}
FoundationConfig FoundationConfig::Parse(const std::string& text) {
    const auto root=ParseJson(text);
    const auto stage=root.At("stage").StringValue();
    if(stage=="robot_video") Keys(root,{"schema_version","stage","task","seed","duration_s","tick_rates_hz","max_catchup_steps","world","encoder"});
    else Keys(root,{"schema_version","stage","task","seed","duration_s","tick_rates_hz","max_catchup_steps"});
    if(Number(root,"schema_version",1,1,true)!=1) throw std::runtime_error("unsupported schema");
    if(stage!="foundation"&&stage!="robot_video") throw std::runtime_error("unsupported research stage");
    FoundationConfig config;
    config.stage=stage;
    if(stage=="robot_video") {
        config.encoder=root.At("encoder").StringValue();
        if(config.encoder!="auto"&&config.encoder!="software") throw std::runtime_error("encoder must be auto or software");
        const auto& world=root.At("world");
        Keys(world,{"initial_y_m","initial_yaw_rad","corridor_width_m"});
        config.initialY=Number(world,"initial_y_m",-0.15,0.15);
        config.initialYaw=Number(world,"initial_yaw_rad",-0.175,0.175);
        config.corridorWidth=Number(world,"corridor_width_m",0.65,0.90);
    }
    config.task=root.At("task").StringValue();
    if(config.task!="T1"&&config.task!="T2") throw std::runtime_error("task must be T1 or T2");
    config.seed=static_cast<uint32_t>(Number(root,"seed",0,4294967295.0,true));
    config.durationUs=static_cast<uint64_t>(std::llround(Number(root,"duration_s",0.1,3600)*1000000.0));
    const auto& rates=root.At("tick_rates_hz");
    Keys(rates,{"physics","camera","control"});
    config.physicsHz=static_cast<uint32_t>(Number(rates,"physics",1,1000,true));
    config.cameraHz=static_cast<uint32_t>(Number(rates,"camera",1,120,true));
    config.controlHz=static_cast<uint32_t>(Number(rates,"control",1,200,true));
    config.maxCatchupSteps=static_cast<uint32_t>(Number(root,"max_catchup_steps",1,100,true));
    if(config.physicsHz<config.controlHz) throw std::runtime_error("physics rate must be >= control rate");
    if(stage=="robot_video"&&(config.physicsHz!=100||config.cameraHz!=30||config.controlHz!=20||config.durationUs>60000000))
        throw std::runtime_error("robot_video requires 100/30/20 Hz and duration <= 60s");
    config.raw=text;
    return config;
}
FoundationConfig FoundationConfig::Load(const std::filesystem::path& path) { return Parse(ReadFile(path,65536)); }
std::string FoundationConfig::EffectiveJson() const {
    std::ostringstream out; out.imbue(std::locale::classic());
    out << "{\"schema_version\":1,\"stage\":" << JsonString(stage) << ",\"task\":" << JsonString(task)
        << ",\"seed\":" << seed << ",\"duration_s\":" << std::fixed << std::setprecision(6) << durationUs/1000000.0
        << ",\"tick_rates_hz\":{\"physics\":" << physicsHz << ",\"camera\":" << cameraHz << ",\"control\":" << controlHz
        << "},\"max_catchup_steps\":" << maxCatchupSteps;
    if(stage=="robot_video") out << ",\"encoder\":" << JsonString(encoder) << ",\"world\":{\"initial_y_m\":" << initialY
        << ",\"initial_yaw_rad\":" << initialYaw << ",\"corridor_width_m\":" << corridorWidth << "}";
    out << "}\n";
    return out.str();
}
uint64_t MonotonicUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::string Sha256(const std::string& bytes) {
    if(bytes.size()>ULONG_MAX) throw std::runtime_error("hash input too large");
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    if(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)<0) throw std::runtime_error("SHA256 provider failed");
    unsigned char hash[32]{};
    const auto status=BCryptHash(algorithm,nullptr,0,reinterpret_cast<PUCHAR>(const_cast<char*>(bytes.data())),
                               static_cast<ULONG>(bytes.size()),hash,32);
    BCryptCloseAlgorithmProvider(algorithm,0);
    if(status<0) throw std::runtime_error("SHA256 calculation failed");
    constexpr char hex[]="0123456789abcdef"; std::string result;
    for(unsigned char b:hash) { result+=hex[b>>4]; result+=hex[b&15]; } return result;
}
PeriodicDeadline::PeriodicDeadline(uint64_t startUs,uint32_t rateHz,uint32_t maxCatchup)
    :startUs_(startUs),rateHz_(rateHz),maxCatchup_(maxCatchup) {
    if(rateHz==0||rateHz>1000||maxCatchup==0) throw std::runtime_error("invalid periodic deadline");
}
uint64_t PeriodicDeadline::NextUs() const { return startUs_+nextIndex_*1000000/rateHz_; }
uint32_t PeriodicDeadline::Poll(uint64_t nowUs) {
    if(nowUs<startUs_) throw std::runtime_error("clock precedes session origin");
    if(startUs_+(nextIndex_+maxCatchup_)*1000000/rateHz_<=nowUs)
        throw std::runtime_error("schedule catchup limit exceeded");
    uint32_t count=0;
    while(NextUs()<=nowUs) {
        ++nextIndex_; ++count;
    }
    return count;
}
ResearchSession::ResearchSession(const FoundationConfig& config,const std::filesystem::path& outputRoot,
                                 const std::filesystem::path& executable,bool headless):id_(NewId()) {
    directory_=std::filesystem::absolute(outputRoot)/id_;
    std::filesystem::create_directories(directory_.parent_path());
    if(!std::filesystem::create_directory(directory_)) throw std::runtime_error("refusing to reuse session directory");
    WriteFile(directory_/"config.input.json",config.raw);
    WriteFile(directory_/"config.effective.json",config.EffectiveJson());
    events_.open(directory_/"events.jsonl",std::ios::binary);
    events_.exceptions(std::ios::badbit|std::ios::failbit);
    startUs_=MonotonicUs();
    Manifest(config,executable,headless);
    // Origin is fixed before startup IO; actual schedule uses a logged later origin.
    Event("session_started",config.stage=="foundation"?"foundation only; task and seed are labels":"robot/video validation; scripted commands; no visual controller; seed reserved (deterministic world)");
}
void ResearchSession::Manifest(const FoundationConfig& config,const std::filesystem::path& executable,bool headless) {
    std::ostringstream out;
    out << "{\"schema_version\":1,\"session_id\":" << JsonString(id_) << ",\"started_utc\":" << JsonString(UtcNow())
        << ",\"stage\":" << JsonString(config.stage) << ",\"monotonic_origin_us\":" << startUs_ << ",\"clock\":\"std::chrono::steady_clock\""
        << ",\"timestamp_unit\":\"microseconds\",\"executable\":" << JsonString(PathUtf8(executable))
        << ",\"executable_sha256\":" << JsonString(Sha256(ReadFile(executable,536870912)))
        << ",\"input_config_sha256\":" << JsonString(Sha256(config.raw))
        << ",\"effective_config_sha256\":" << JsonString(Sha256(config.EffectiveJson()))
        << ",\"headless\":" << (headless?"true":"false")
        << ",\"capabilities\":{\"config_validation\":true,\"common_clock\":true,\"lifecycle_log\":true,"
        << "\"world\":" << (config.stage=="robot_video"?"true":"false") << ",\"video_transport\":" << (config.stage=="robot_video"?"true":"false") << ",\"visual_control\":false,\"command_udp\":false},"
           "\"task_result\":\"not_run\",\"build_configuration\":";
#ifdef _DEBUG
    out << "\"Debug\"";
#else
    out << "\"non-Debug\"";
#endif
    out << ",\"msvc_version\":" << _MSC_VER << "}\n";
    WriteFile(directory_/"manifest.json",out.str());
}
ResearchSession::~ResearchSession() {
    if(!finished_) { try { Finish("incomplete","scope exited before normal finish",0,0,0); } catch(...) {} }
}
void ResearchSession::Event(const std::string& type,const std::string& detail,uint64_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto now=MonotonicUs();
    events_ << "{\"session_id\":" << JsonString(id_) << ",\"seq\":" << ++sequence_
            << ",\"monotonic_us\":" << now << ",\"elapsed_us\":" << now-startUs_
            << ",\"event\":" << JsonString(type) << ",\"detail\":" << JsonString(detail)
            << ",\"count\":" << count << "}\n";
    if(type!="clock_tick") events_.flush();
}
void ResearchSession::Finish(const std::string& status,const std::string& reason,
                             uint64_t physics,uint64_t camera,uint64_t control) {
    if(finished_) return;
    Event("session_finished",status+": "+reason);
    std::ostringstream out;
    out << "{\"session_id\":" << JsonString(id_) << ",\"status\":" << JsonString(status)
        << ",\"reason\":" << JsonString(reason) << ",\"elapsed_us\":" << MonotonicUs()-startUs_
        << ",\"clock_tick_counts\":{\"physics\":" << physics << ",\"camera\":" << camera << ",\"control\":" << control
        << "},\"task_result\":\"not_run\",\"closed_loop_validated\":false}\n";
    const auto temporary=directory_/"summary.tmp";
    WriteFile(temporary,out.str());
    std::filesystem::rename(temporary,directory_/"summary.json");
    finished_=true;
}
}
