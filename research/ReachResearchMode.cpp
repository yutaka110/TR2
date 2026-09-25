#define NOMINMAX
#include "ReachFoundation.h"
#include "ReachRobotVideo.h"
#include "ReachCommandUdp.h"
#include "ReachBudgetTransport.h"
#include "ReachStateFeedbackUdp.h"
#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace reach {
namespace {
class ResearchWait {
    HANDLE timer_=nullptr;
public:
    explicit ResearchWait(bool precise){
        if(precise){timer_=CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE);
            if(!timer_)throw std::runtime_error("high resolution research timer unavailable");}
    }
    ~ResearchWait(){if(timer_)CloseHandle(timer_);}
    void Wait(){
        if(!timer_){std::this_thread::sleep_for(std::chrono::milliseconds(1));return;}
        LARGE_INTEGER due{};due.QuadPart=-10000; // One millisecond, relative 100 ns units.
        if(!SetWaitableTimerEx(timer_,&due,0,nullptr,nullptr,nullptr,0)||WaitForSingleObject(timer_,1000)!=WAIT_OBJECT_0)
            throw std::runtime_error("research timer wait failed");
    }
};
std::wstring Env(const wchar_t* name) {
    const DWORD size=GetEnvironmentVariableW(name,nullptr,0);
    if(size==0) return {};
    std::wstring value(size,L'\0');
    const auto copied=GetEnvironmentVariableW(name,value.data(),size);
    if(copied==0||copied>=size) throw std::runtime_error("environment changed during read");
    value.resize(copied); return value;
}
bool Flag(const wchar_t* name) {
    const auto value=Env(name);
    if(value.empty()||value==L"0") return false;
    if(value==L"1") return true;
    throw std::runtime_error("research boolean environment options accept only 0 or 1");
}
std::wstring Wide(const std::string& value) {
    const int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(size==0&&!value.empty()) return L"(text conversion failed)";
    std::wstring result(size,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size);
    return result;
}
struct Dashboard {
    std::wstring id, task, directory, state=L"実行中";
    uint64_t elapsedUs=0, durationUs=0;
    uint64_t ticks[3]{};
    uint32_t rates[3]{};
    bool completed=false, closed=false;
    bool robot=false;
    double corridorWidth=.75;
    RobotTruth truth;
    RobotVideoView video;
    bool visual=false;
    MotionCommand command;
    bool commandUdp=false;
    bool boundedLink=false;
    std::wstring linkCaption;
    CommandApplication applied;
};
void Fill(HDC dc,RECT rect,COLORREF color) {
    HBRUSH brush=CreateSolidBrush(color); FillRect(dc,&rect,brush); DeleteObject(brush);
}
void Label(HDC dc,int x,int y,int width,int height,const std::wstring& text,int size,COLORREF color,bool bold=false) {
    HFONT font=CreateFontW(-size,0,0,0,bold?FW_SEMIBOLD:FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                          OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Yu Gothic UI");
    auto previous=SelectObject(dc,font); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,color);
    RECT rect{x,y,x+width,y+height}; DrawTextW(dc,text.c_str(),-1,&rect,DT_LEFT|DT_WORDBREAK|DT_NOPREFIX);
    SelectObject(dc,previous); DeleteObject(font);
}
LRESULT CALLBACK WindowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam) {
    auto* state=reinterpret_cast<Dashboard*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        state=static_cast<Dashboard*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(state));
    }
    if(message==WM_CLOSE) { if(state) state->closed=true; DestroyWindow(window); return 0; }
    if(message==WM_ERASEBKGND) return 1;
    if(message==WM_PAINT&&state) {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint); RECT client{}; GetClientRect(window,&client);
        HDC buffer=CreateCompatibleDC(dc); HBITMAP bitmap=CreateCompatibleBitmap(dc,client.right,client.bottom);
        auto old=SelectObject(buffer,bitmap);
        if(state->robot) {
            Fill(buffer,client,RGB(241,245,250));
            Fill(buffer,{0,0,client.right,128},RGB(18,37,62));
            Label(buffer,36,21,1000,40,L"Reach-RT  /  Robot & Video Lab",30,RGB(255,255,255),true);
            Label(buffer,38,72,1040,36,state->boundedLink?state->linkCaption:state->commandUdp?L"G1-05   映像 → 認識 → 指令UDP → 期限監視・制動":state->visual?L"G1-04   受信画像から認識・制御 ／ 既知マーカー・平面移動":L"G1-02 / 03   仮想ロボット・実映像通信・撮影時刻の照合",21,RGB(190,216,240));
            Label(buffer,36,146,640,30,L"受信映像  —  H.264 / RNVP / UDP / 復号",20,RGB(24,46,70),true);
            Fill(buffer,{36,188,676,548},RGB(25,36,47));
            if(!state->video.bgra.empty()) {
                BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=640;
                info.bmiHeader.biHeight=-360;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
                StretchDIBits(buffer,36,188,640,360,0,0,640,360,state->video.bgra.data(),&info,DIB_RGB_COLORS,SRCCOPY);
                if(state->visual&&state->video.observation.valid){
                    HPEN pen=CreatePen(PS_SOLID,2,RGB(40,230,120));auto oldPen=SelectObject(buffer,pen);
                    const auto& corners=state->video.observation.corners;
                    MoveToEx(buffer,36+int(corners[3].u),188+int(corners[3].v),nullptr);
                    for(auto corner:corners)LineTo(buffer,36+int(corner.u),188+int(corner.v));
                    SelectObject(buffer,oldPen);DeleteObject(pen);
                }
            } else Label(buffer,100,345,520,45,L"コーデックの出力を待っています",22,RGB(255,255,255));
            Fill(buffer,{702,188,client.right-36,548},RGB(255,255,255));
            Label(buffer,724,205,340,32,L"観測用マップ（真値）",20,RGB(24,46,70),true);
            Fill(buffer,{727,262,1066,338},RGB(233,239,245));
            if(state->task==L"T1") {
                const int half=static_cast<int>(state->corridorWidth*40);
                Fill(buffer,{727,298-half,967,302-half},RGB(78,103,126));Fill(buffer,{727,298+half,967,302+half},RGB(78,103,126));
            }
            const int rx=727+static_cast<int>(state->truth.x*80),ry=300-static_cast<int>(state->truth.y*80);
            HBRUSH robotBrush=CreateSolidBrush(state->truth.collision?RGB(200,50,40):RGB(30,115,205));
            auto priorBrush=SelectObject(buffer,robotBrush);Ellipse(buffer,rx-16,ry-16,rx+16,ry+16);
            SelectObject(buffer,priorBrush);DeleteObject(robotBrush);
            MoveToEx(buffer,rx,ry,nullptr);LineTo(buffer,rx+static_cast<int>(26*cos(state->truth.yaw)),ry-static_cast<int>(26*sin(state->truth.yaw)));
            wchar_t pose[180];swprintf_s(pose,L"x %.2f m   y %.2f m\n速度 %.2f m/s   接触 %s",state->truth.x,state->truth.y,state->truth.v,state->truth.collision?L"あり":L"なし");
            Label(buffer,724,353,340,68,pose,18,RGB(45,70,94));
            Label(buffer,724,432,340,28,L"復号ID  "+std::to_wstring(state->video.frameId),22,RGB(24,46,70),true);
            Label(buffer,724,473,340,50,L"撮影→復号  "+std::to_wstring(static_cast<int>(state->video.ageMs))+L" ms\n画像番号・PTS照合  "+std::to_wstring(state->video.matched)+L" / "+std::to_wstring(state->video.decoded),18,RGB(24,112,86));
            Label(buffer,36,563,1040,36,L"撮影 "+std::to_wstring(state->video.captured)+L"   符号化 "+std::to_wstring(state->video.encoded)+L"   復号 "+std::to_wstring(state->video.decoded)+L"   不一致 "+std::to_wstring(state->video.errors)+L"     "+state->state,20,RGB(24,46,70),true);
            if(state->visual){
                const auto& c=state->command;const auto& o=state->video.observation;
                wchar_t text[300];swprintf_s(text,L"%s / %s   指令 v %.2f m/s  ω %.2f rad/s   使用画像 #%u（%.0f ms）\n画像推定 x %.2f / y %.2f m  yaw %.1f° ／ %s  ※指令はローカル接続。UDP化はG1-05",
                    Wide(c.state).c_str(),Wide(c.reason).c_str(),c.v,c.w,c.sourceFrameId,c.ageMs,o.x,o.y,o.yaw*180/3.141592653589793,Wide(o.reason).c_str());
                Label(buffer,36,605,1040,55,text,17,RGB(38,84,109));
                if(state->commandUdp){
                    Fill(buffer,{36,605,1100,660},RGB(241,245,250));
                    const auto& a=state->applied;
                    swprintf_s(text,L"生成 #%llu  v %.2f / ω %.2f  → UDP受信 #%llu   %s\n実適用 v %.2f m/s / ω %.2f rad/s　使用画像 #%u ／ 指令期限100 ms・通信断監視250 ms",
                        c.sequence,c.v,c.w,a.command.sequence,Wide(a.reason).c_str(),a.command.v,a.command.w,a.command.sourceFrameId);
                    Label(buffer,36,605,1040,55,text,17,RGB(38,84,109));
                }
            }else Label(buffer,36,605,1040,55,L"接続検証：12秒まで前進、以後は制動。画像による自動制御は次のG1-04です。\n上端の白黒帯は画像IDの検証用です。マップの真値は制御に渡しません。",17,RGB(78,100,121));
            Label(buffer,36,660,1040,28,L"保存先（下の欄で全文を選択・コピーできます）",15,RGB(84,104,126));
            BitBlt(dc,0,0,client.right,client.bottom,buffer,0,0,SRCCOPY);
            SelectObject(buffer,old);DeleteObject(bitmap);DeleteDC(buffer);EndPaint(window,&paint);return 0;
        }
        Fill(buffer,client,RGB(241,245,250));
        Fill(buffer,{0,0,client.right,150},RGB(18,37,62));
        Label(buffer,36,24,1000,42,L"Reach-RT  /  Research Workbench",30,RGB(255,255,255),true);
        Label(buffer,38,76,1040,48,L"G1-01  研究モードの土台 — 実験条件・共通時計・実行記録",22,RGB(190,216,240));
        Label(buffer,36,173,1040,38,L"条件を固定し、再現できる研究実行を始める",25,RGB(24,46,70),true);
        Fill(buffer,{36,224,client.right-36,328},RGB(255,255,255));
        Label(buffer,56,238,200,26,L"SESSION",14,RGB(90,110,130),true);
        Label(buffer,56,268,650,40,state->id,20,RGB(25,52,79));
        Label(buffer,760,239,300,60,state->state,23,state->completed?RGB(20,118,89):RGB(37,94,176),true);
        const wchar_t* names[]={L"物理更新の時計",L"撮影周期の時計",L"制御周期の時計"};
        for(int i=0;i<3;++i) {
            const int x=36+i*350;
            Fill(buffer,{x,351,x+330,476},RGB(255,255,255));
            Label(buffer,x+20,367,300,28,names[i],19,RGB(75,96,119));
            Label(buffer,x+20,404,300,48,std::to_wstring(state->ticks[i])+L" ticks  /  "+std::to_wstring(state->rates[i])+L" Hz",26,RGB(20,47,78),true);
        }
        const int barWidth=client.right-72;
        Fill(buffer,{36,498,36+barWidth,506},RGB(215,226,238));
        const double progress=state->durationUs?std::min(1.0,static_cast<double>(state->elapsedUs)/state->durationUs):0;
        Fill(buffer,{36,498,36+static_cast<int>(barWidth*progress),506},RGB(36,120,210));
        Label(buffer,36,523,1040,34,L"対象ラベル: "+state->task+L"  •  経過 "+std::to_wstring(state->elapsedUs/1000000)+L" 秒 / "+std::to_wstring(state->durationUs/1000000)+L" 秒",18,RGB(60,85,112));
        Label(buffer,36,568,1040,66,L"この段階は時計と記録の確認です。仮想ロボット・映像送受信・自動制御は未接続です。\n表示中の ticks は処理周期の通知数で、映像fpsや作業成功数ではありません。",18,RGB(83,99,118));
        Label(buffer,36,650,1040,35,L"保存先（下の欄で全文を選択・コピーできます）",15,RGB(84,104,126));
        BitBlt(dc,0,0,client.right,client.bottom,buffer,0,0,SRCCOPY);
        SelectObject(buffer,old); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(window,&paint); return 0;
    }
    return DefWindowProcW(window,message,wparam,lparam);
}
}
bool RunResearchModeFromEnvironment(int& exitCode) {
    const auto mode=Env(L"TR2_RESEARCH_MODE");
    if(mode.empty()||mode==L"legacy") return false;
    std::unique_ptr<ResearchSession> session;
    HWND window=nullptr;
    bool headless=true;
    uint64_t recordedPhysics=0,recordedCamera=0,recordedControl=0;
    try {
        if(mode!=L"reach_rt") throw std::runtime_error("TR2_RESEARCH_MODE must be legacy or reach_rt");
        headless=Flag(L"TR2_REACH_HEADLESS");
        const auto configEnv=Env(L"TR2_REACH_CONFIG");
        const auto outputEnv=Env(L"TR2_REACH_OUTPUT_ROOT");
        const auto config=FoundationConfig::Load(configEnv.empty()?L"config/reach_rt_g1_foundation.json":configEnv);
        wchar_t executable[32768]{};
        const auto length=GetModuleFileNameW(nullptr,executable,32768);
        if(length==0||length>=32768) throw std::runtime_error("executable path unavailable");
        session=std::make_unique<ResearchSession>(config,outputEnv.empty()?L"artifacts/reach_rt/runs":outputEnv,executable,headless);
        Dashboard state{Wide(session->Id()),Wide(config.task),session->Directory().wstring()};
        state.boundedLink=config.boundedLink;
        state.durationUs=config.durationUs;
        state.robot=config.stage!="foundation";
        state.visual=config.HasVisualControl();
        state.commandUdp=config.stage=="command_udp";
        state.corridorWidth=config.corridorWidth;
        std::unique_ptr<RobotWorld> robot;
        std::shared_ptr<BudgetTransport> budget;
        if(config.budgeted)budget=std::make_shared<BudgetTransport>(config.ipBudget,session->Directory(),config.durationUs);
        std::unique_ptr<RobotVideo> video;
        std::unique_ptr<RemoteTaskController> controller;
        std::unique_ptr<StateFeedbackUdp> stateFeedback;
        SenderStateEstimate senderEstimate;
        std::unique_ptr<CommandUdp> commandLink;
        BufferedLog worldLog,commandLog,captureEvaluation,appliedCommands;
        uint64_t commandCount=0,movingCommands=0,stopCommands=0;
        if(state.robot) {
            robot=std::make_unique<RobotWorld>(config.task,config.initialY,config.initialYaw,config.corridorWidth);
            video=std::make_unique<RobotVideo>(config,*session,budget);
            worldLog.open(session->Directory()/"world.csv");worldLog.exceptions(std::ios::badbit|std::ios::failbit);
            worldLog.precision(12);
            worldLog<<"physics_tick,simulation_s,x_m,y_m,yaw_rad,v_m_s,w_rad_s,collision,out_of_bounds,goal_hold_s,success,timeout\n";
            if(state.visual){
                controller=std::make_unique<RemoteTaskController>(config.task,config.corridorWidth);
                commandLog.open(session->Directory()/"commands.csv");commandLog.exceptions(std::ios::badbit|std::ios::failbit);commandLog.precision(12);
                commandLog<<"sequence,generated_us,valid_until_us,source_frame_id,source_stream_id,source_capture_us,age_ms,state,reason,v_m_s,w_rad_s,distance_m,wall_margin_m,estimated_complete\n";
                captureEvaluation.open(session->Directory()/"capture_evaluation.csv");captureEvaluation.exceptions(std::ios::badbit|std::ios::failbit);captureEvaluation.precision(12);
                captureEvaluation<<"frame_id,physics_tick,x_m,y_m,yaw_rad\n";
                appliedCommands.open(session->Directory()/(state.commandUdp?"udp_applied_commands.csv":"local_applied_commands.csv"));appliedCommands.exceptions(std::ios::badbit|std::ios::failbit);appliedCommands.precision(12);
                appliedCommands<<"physics_tick,applied_us,sequence,live,v_m_s,w_rad_s"<<(state.commandUdp?",reason,accepted_us,generated_us,valid_until_us,source_frame_id,source_capture_us\n":"\n");
                if(state.commandUdp)commandLink=std::make_unique<CommandUdp>(session->Id(),session->Directory(),config.commandScenario,config.boundedLink?&config.downlink:nullptr,budget);
                if(budget){video->SetFeedbackIngress(commandLink->IngressPort());commandLink->SetFeedbackDestination(video->SenderPort());}
                if(config.stateFeedback){stateFeedback=std::make_unique<StateFeedbackUdp>(session->Id(),session->Directory(),budget);commandLink->SetStateDestination(stateFeedback->Port());}
                session->Event("visual_control_started",state.commandUdp?"receiver pixels -> fixed controller -> serialized reverse UDP -> robot deadline/watchdog":"receiver pixels -> marker pose -> fixed controller -> local diagnostic adapter; command UDP pending G1-05");
            }
        }
        state.rates[0]=config.physicsHz; state.rates[1]=config.cameraHz; state.rates[2]=config.controlHz;
        if(!headless) {
            WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=GetModuleHandleW(nullptr);
            wc.lpszClassName=L"ReachRTFoundation"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
            if(!RegisterClassW(&wc)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) throw std::runtime_error("window registration failed");
            window=CreateWindowExW(0,wc.lpszClassName,L"Reach-RT | G1-01 Research Foundation",
                WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,1140,790,
                nullptr,nullptr,wc.hInstance,&state);
            if(!window) throw std::runtime_error("research window creation failed");
            if(state.robot) SetWindowTextW(window,L"Reach-RT | G1-02 / 03 Robot & Video");
            if(state.visual) SetWindowTextW(window,L"Reach-RT | G1-04 Received Image Control");
            if(state.commandUdp) SetWindowTextW(window,L"Reach-RT | G1-05 Reverse Command UDP");
            if(state.boundedLink)SetWindowTextW(window,L"Reach-RT | G2-02 Time-based Link Trace");
            if(config.budgeted)SetWindowTextW(window,L"Reach-RT | G2-03 Shared IP Budget");
            if(config.stateFeedback)SetWindowTextW(window,L"Reach-RT | G2-05 Received Notification Estimate");
            HWND pathEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",state.directory.c_str(),WS_CHILD|WS_VISIBLE|ES_READONLY|ES_AUTOHSCROLL,
                36,690,1040,25,window,nullptr,wc.hInstance,nullptr);
            SendMessageW(pathEdit,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
            ShowWindow(window,SW_SHOW); UpdateWindow(window);
        }
        const bool preciseWait=Flag(L"TR2_REACH_PRECISE_WAIT");ResearchWait researchWait(preciseWait);
        BufferedLog scheduleTiming;scheduleTiming.open(session->Directory()/"schedule_timing.csv");
        scheduleTiming.exceptions(std::ios::badbit|std::ios::failbit);scheduleTiming<<"phase,start_us,end_us,elapsed_us\n";
        std::ofstream(session->Directory()/"schedule_model.json")<<"{\"precise_wait\":"<<(preciseWait?"true":"false")<<",\"requested_wait_us\":1000,\"record_threshold_us\":2000,\"clock_guards_unchanged\":true}";
        auto phaseStart=MonotonicUs();const char* phase="setup";
        auto mark=[&](const char* next){const auto end=MonotonicUs();if(end-phaseStart>2000)scheduleTiming<<phase<<','<<phaseStart<<','<<end<<','<<end-phaseStart<<'\n';phaseStart=end;phase=next;};
        const auto origin=MonotonicUs();
        if(budget)budget->Start(origin);
        if(video)video->StartLink(origin);
        if(commandLink)commandLink->Start(origin);
        if(stateFeedback)stateFeedback->Start(origin,config.durationUs,commandLink->IngressPort(),commandLink->RelaySourcePort());
        session->Event("schedule_origin","absolute monotonic microseconds",origin);
        PeriodicDeadline physics(origin,config.physicsHz,config.maxCatchupSteps);
        PeriodicDeadline camera(origin,config.cameraHz,config.maxCatchupSteps);
        PeriodicDeadline control(origin,config.controlHz,config.maxCatchupSteps);
        uint64_t nextUi=origin, nextHeartbeat=origin+1000000;
        while(!state.closed) {
            mark("window");
            if(window) {
                MSG message{};
                while(PeekMessageW(&message,window,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if(state.closed) break;
            mark("clock");
            const auto now=MonotonicUs();
            const auto until=std::min(now,origin+config.durationUs);
            const auto physical=physics.Poll(until);
            const auto controls=control.Poll(until);
            const auto cameras=camera.Poll(until);
            recordedPhysics=physics.Count();recordedCamera=camera.Count();recordedControl=control.Count();
            if(physical)session->Event("clock_tick","physics",physical);
            if(controls)session->Event("clock_tick","control",controls);
            if(cameras)session->Event("clock_tick","camera",cameras);
            mark("state_poll");
            if(robot) {
                video->Check();
                if(stateFeedback)stateFeedback->Poll();
                mark("control");
                if(controller&&controls){
                    if(controls>1)throw std::runtime_error("visual control deadline missed; cannot reconstruct historical observations");
                    const auto observed=video->Observation();
                    state.command=controller->Update(observed,MonotonicUs());
                    video->RecordControl(state.command);
                    if(stateFeedback){stateFeedback->Send(video->ReceiverNotification());senderEstimate=stateFeedback->Estimate();video->PublishBaselineState(senderEstimate);}
                    if(commandLink)commandLink->Offer(state.command);
                    const auto& c=state.command;++commandCount;if(c.v>0||std::abs(c.w)>0)++movingCommands;else++stopCommands;
                    commandLog<<c.sequence<<','<<c.generatedUs<<','<<c.validUntilUs<<','<<c.sourceFrameId<<','<<c.sourceStreamId<<','<<c.sourceCaptureUs<<','<<c.ageMs<<','
                        <<c.state<<','<<c.reason<<','<<c.v<<','<<c.w<<','<<c.distanceM<<','<<c.wallMarginM<<','<<c.estimatedComplete<<'\n';
                }
                mark("command_pump");if(commandLink)commandLink->Pump();mark("physics");
                for(uint32_t i=0;i<physical;++i) {
                    // Script reads time only. No pose/goal information enters its command.
                    const auto tick=physics.Count()-physical+i;
                    if(commandLink){
                        const auto appliedUs=MonotonicUs();state.applied=commandLink->Application(appliedUs);
                        const auto& a=state.applied;const auto& c=a.command;
                        // Only decoded UDP receiver state enters the actuator.
                        robot->Command(c.v,c.w);
                        appliedCommands<<tick+1<<','<<appliedUs<<','<<c.sequence<<','<<a.live<<','<<c.v<<','<<c.w<<','<<a.reason<<','<<a.acceptedUs<<','<<c.generatedUs<<','<<c.validUntilUs<<','<<c.sourceFrameId<<','<<c.sourceCaptureUs<<'\n';
                    }else if(controller){
                        // Temporary local adapter; G1-05 replaces this with the UDP command receiver.
                        const auto appliedUs=MonotonicUs();const bool live=appliedUs<=state.command.validUntilUs;
                        robot->Command(live?state.command.v:0,live?state.command.w:0);
                        appliedCommands<<tick+1<<','<<appliedUs<<','<<state.command.sequence<<','<<live<<','<<(live?state.command.v:0)<<','<<(live?state.command.w:0)<<'\n';
                    }else if(tick%5==0)robot->Command(tick<1200?.15:0,0);
                    robot->Step(.01);const auto& t=robot->Truth();
                    worldLog<<tick+1<<','<<t.elapsed<<','<<t.x<<','<<t.y<<','<<t.yaw<<','<<t.v<<','<<t.w<<','<<t.collision<<','<<t.outOfBounds<<','<<t.hold<<','<<t.success<<','<<t.timeout<<'\n';
                }
                mark("capture");if(cameras>1) throw std::runtime_error("camera deadline missed; historical pixels cannot be reconstructed");
                if(cameras){
                    video->Capture(*robot);
                    if(controller){
                        // Evaluation sink only. Nothing reads these values back into control.
                        const auto& evaluation=robot->Truth();
                        captureEvaluation<<camera.Count()<<','<<physics.Count()<<','<<evaluation.x<<','<<evaluation.y<<','<<evaluation.yaw<<'\n';
                    }
                }
                mark("ui");state.truth=robot->Truth();
                if(now>=nextUi)state.video=video->View();
            }
            state.elapsedUs=until-origin;
            if(state.boundedLink&&now>=nextUi){
                auto rate=[&](const LinkConfig& link){uint64_t bps=0;for(auto p:link.capacity){if(p.atUs>state.elapsedUs)break;bps=p.bps;}return bps;};
                auto status=[&](const LinkConfig& link){auto p=link.impairment[link.StateIndex(state.elapsedUs)];return std::to_wstring(rate(link))+L" bps / "+(p.drop?L"損失区間":L"送達区間")+L" / "+std::to_wstring(p.delayUs/1000)+L" ms";};
                state.linkCaption=L"G2-02  上り "+status(config.uplink)+L"・下り "+status(config.downlink);
                if(config.budgeted)state.linkCaption=L"G2-03  共通IP予算 "+std::to_wstring(config.ipBudget.total.rateBps)+L" bps / 最大 "+std::to_wstring(config.ipBudget.total.maxBytes)+L" byte ／ 映像・FEC・再送・ACK・指令";
                if(config.stateFeedback)state.linkCaption=L"G2-05  送信側の受信通知 #"+std::to_wstring(senderEstimate.report.sequence)+L" / 古さ "+std::to_wstring(senderEstimate.ageUs/1000)+L" ms / "+(senderEstimate.notificationLive?L"通知有効":L"未到着・失効")+L" / 世代 "+std::to_wstring(senderEstimate.report.generation);
            }
            state.ticks[0]=physics.Count(); state.ticks[1]=camera.Count(); state.ticks[2]=control.Count();
            if(now>=nextHeartbeat) { session->Event("heartbeat",state.robot?"robot/video validation active":"foundation scheduling active"); nextHeartbeat=now+1000000; }
            if(now>=nextUi) { if(window)InvalidateRect(window,nullptr,FALSE); nextUi=now+100000; }
            if(now>=origin+config.durationUs) {
                if(budget)budget->CloseAdmission();
                if(commandLink&&!budget)commandLink->Finish();
                if(video) {
                    const bool ok=video->Finish();state.video=video->View();
                    if(!ok) throw std::runtime_error("robot/video validation failed; see robot_video_summary.json");
                }
                if(budget){commandLink->Finish();video->FinishFeedback(commandLink->FeedbackDelivered());if(stateFeedback)stateFeedback->Finish(commandLink->StateDelivered());budget->Finish();}
                if(controller){
                    const auto& t=robot->Truth();
                    std::ofstream out(session->Directory()/"visual_control_summary.json");out.exceptions(std::ios::badbit|std::ios::failbit);
                    out<<"{\"commands\":"<<commandCount<<",\"moving_commands\":"<<movingCommands<<",\"stop_commands\":"<<stopCommands
                        <<",\"recognized\":"<<state.video.recognized<<",\"rejected\":"<<state.video.rejected<<",\"local_evaluator_success\":"<<(t.success?"true":"false")
                        <<",\"collision\":"<<(t.collision?"true":"false")<<",\"out_of_bounds\":"<<(t.outOfBounds?"true":"false")
                        <<",\"command_udp\":"<<(state.commandUdp?"true":"false")<<",\"closed_loop_validated\":false,\"task_result\":\""<<(state.commandUdp?"udp_integration_only":"local_diagnostic_only")<<"\"}\n";
                }
                // Flush measurement buffers after scheduling ends, before certifying completion.
                if(worldLog.is_open())worldLog.close();if(commandLog.is_open())commandLog.close();
                if(captureEvaluation.is_open())captureEvaluation.close();if(appliedCommands.is_open())appliedCommands.close();
                scheduleTiming.close();
                session->Finish(state.commandUdp?"command_udp_completed":state.visual?"visual_control_completed":state.robot?"robot_video_completed":"foundation_completed",state.boundedLink?"capacity-limited UDP trial recorded; link verification and task outcome adjudicated separately":state.commandUdp?"reverse UDP trial recorded; task and G1 gate adjudicated by external auditor":state.visual?"image recognition/control local diagnostic completed; command UDP and G1-06 validation pending":state.robot?"scripted robot/video identity validation; autonomous task not run":"configured clock interval completed; task not run",physics.Count(),camera.Count(),control.Count());
                state.completed=true; state.state=L"基盤確認 完了";
                if(state.robot)state.state=L"接続・照合 完了";
                if(state.visual)state.state=L"認識・指令生成 記録完了";
                if(window) InvalidateRect(window,nullptr,FALSE);
                break;
            }
            mark("wait");researchWait.Wait();mark("between_loops");
        }
        if(!state.completed) {
            if(budget)budget->CloseAdmission();
            if(video)video->Finish();
            if(budget){commandLink->Finish();video->FinishFeedback(commandLink->FeedbackDelivered());if(stateFeedback)stateFeedback->Finish(commandLink->StateDelivered());budget->Finish();}
            session->Finish("interrupted","window closed before interval completed",physics.Count(),camera.Count(),control.Count());
        }
        exitCode=state.completed?0:3;
        // Keep the completed report visible until the user closes the research window.
        while(window&&!state.closed) {
            MSG message{};
            if(GetMessageW(&message,window,0,0)<=0) break;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return true;
    } catch(const std::exception& error) {
        if(window&&IsWindow(window)) DestroyWindow(window);
        if(session) { try { session->Finish("invalid",error.what(),recordedPhysics,recordedCamera,recordedControl); } catch(...) {} }
        const std::string message=std::string("Reach-RT start/run failed: ")+error.what();
        OutputDebugStringA(message.c_str());
        // STDERR is captured by the launcher; UI launches also receive a visible error.
        fprintf(stderr,"%s\n",message.c_str());
        if(!headless) MessageBoxW(nullptr,Wide(message).c_str(),L"Reach-RT",MB_OK|MB_ICONERROR);
        exitCode=2;
        return true;
    }
}
}
