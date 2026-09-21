#define NOMINMAX
#include "ReachFoundation.h"
#include "ReachRobotVideo.h"
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
            Label(buffer,38,72,1040,36,L"G1-02 / 03   仮想ロボット・実映像通信・撮影時刻の照合",21,RGB(190,216,240));
            Label(buffer,36,146,640,30,L"受信映像  —  H.264 / RNVP / UDP / 復号",20,RGB(24,46,70),true);
            Fill(buffer,{36,188,676,548},RGB(25,36,47));
            if(!state->video.bgra.empty()) {
                BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=640;
                info.bmiHeader.biHeight=-360;info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;
                StretchDIBits(buffer,36,188,640,360,0,0,640,360,state->video.bgra.data(),&info,DIB_RGB_COLORS,SRCCOPY);
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
            Label(buffer,36,605,1040,55,L"接続検証：12秒まで前進、以後は制動。画像による自動制御は次のG1-04です。\n上端の白黒帯は画像IDの検証用です。マップの真値は制御に渡しません。",17,RGB(78,100,121));
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
        state.durationUs=config.durationUs;
        state.robot=config.stage=="robot_video";
        state.corridorWidth=config.corridorWidth;
        std::unique_ptr<RobotWorld> robot;
        std::unique_ptr<RobotVideo> video;
        std::ofstream worldLog;
        if(state.robot) {
            robot=std::make_unique<RobotWorld>(config.task,config.initialY,config.initialYaw,config.corridorWidth);
            video=std::make_unique<RobotVideo>(config,*session);
            worldLog.open(session->Directory()/"world.csv");worldLog.exceptions(std::ios::badbit|std::ios::failbit);
            worldLog<<"physics_tick,simulation_s,x_m,y_m,yaw_rad,v_m_s,w_rad_s,collision,out_of_bounds,goal_hold_s,success,timeout\n";
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
            HWND pathEdit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",state.directory.c_str(),WS_CHILD|WS_VISIBLE|ES_READONLY|ES_AUTOHSCROLL,
                36,690,1040,25,window,nullptr,wc.hInstance,nullptr);
            SendMessageW(pathEdit,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
            ShowWindow(window,SW_SHOW); UpdateWindow(window);
        }
        const auto origin=MonotonicUs();
        session->Event("schedule_origin","absolute monotonic microseconds",origin);
        PeriodicDeadline physics(origin,config.physicsHz,config.maxCatchupSteps);
        PeriodicDeadline camera(origin,config.cameraHz,config.maxCatchupSteps);
        PeriodicDeadline control(origin,config.controlHz,config.maxCatchupSteps);
        uint64_t nextUi=origin, nextHeartbeat=origin+1000000;
        while(!state.closed) {
            if(window) {
                MSG message{};
                while(PeekMessageW(&message,window,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            }
            if(state.closed) break;
            const auto now=MonotonicUs();
            const auto until=std::min(now,origin+config.durationUs);
            const auto physical=physics.Poll(until);
            const auto controls=control.Poll(until);
            const auto cameras=camera.Poll(until);
            recordedPhysics=physics.Count();recordedCamera=camera.Count();recordedControl=control.Count();
            if(physical)session->Event("clock_tick","physics",physical);
            if(controls)session->Event("clock_tick","control",controls);
            if(cameras)session->Event("clock_tick","camera",cameras);
            if(robot) {
                video->Check();
                for(uint32_t i=0;i<physical;++i) {
                    // Script reads time only. No pose/goal information enters its command.
                    const auto tick=physics.Count()-physical+i;
                    if(tick%5==0)robot->Command(tick<1200?.15:0,0);
                    robot->Step(.01);const auto& t=robot->Truth();
                    worldLog<<tick+1<<','<<t.elapsed<<','<<t.x<<','<<t.y<<','<<t.yaw<<','<<t.v<<','<<t.w<<','<<t.collision<<','<<t.outOfBounds<<','<<t.hold<<','<<t.success<<','<<t.timeout<<'\n';
                }
                if(cameras>1) throw std::runtime_error("camera deadline missed; historical pixels cannot be reconstructed");
                if(cameras)video->Capture(*robot);
                state.truth=robot->Truth();
                if(now>=nextUi)state.video=video->View();
            }
            state.elapsedUs=until-origin;
            state.ticks[0]=physics.Count(); state.ticks[1]=camera.Count(); state.ticks[2]=control.Count();
            if(now>=nextHeartbeat) { session->Event("heartbeat",state.robot?"robot/video validation active":"foundation scheduling active"); nextHeartbeat=now+1000000; }
            if(now>=nextUi) { if(window)InvalidateRect(window,nullptr,FALSE); nextUi=now+100000; }
            if(now>=origin+config.durationUs) {
                if(video) {
                    const bool ok=video->Finish();state.video=video->View();
                    if(!ok) throw std::runtime_error("robot/video validation failed; see robot_video_summary.json");
                }
                session->Finish(state.robot?"robot_video_completed":"foundation_completed",state.robot?"scripted robot/video identity validation; autonomous task not run":"configured clock interval completed; task not run",physics.Count(),camera.Count(),control.Count());
                state.completed=true; state.state=L"基盤確認 完了";
                if(state.robot)state.state=L"接続・照合 完了";
                if(window) InvalidateRect(window,nullptr,FALSE);
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(!state.completed) {
            if(video)video->Finish();
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
