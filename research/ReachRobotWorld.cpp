#include "ReachRobotWorld.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace reach {
namespace {
constexpr double Pi=3.14159265358979323846;
double Approach(double value,double target,double amount) { return value+std::clamp(target-value,-amount,amount); }
uint16_t Checksum(const std::array<uint8_t,12>& bytes) {
    uint16_t crc=0xffff;
    for(int i=0;i<10;++i) for(int b=7;b>=0;--b) {
        const bool feedback=((crc>>15)^((bytes[i]>>b)&1))!=0;
        crc=static_cast<uint16_t>(crc<<1); if(feedback) crc^=0x1021;
    }
    return crc;
}
void PaintIdentity(std::vector<uint8_t>& pixels,uint32_t id,uint32_t stream) {
    std::array<uint8_t,12> bytes{0x52,0x54};
    for(int i=0;i<4;++i) { bytes[2+i]=static_cast<uint8_t>(id>>(i*8)); bytes[6+i]=static_cast<uint8_t>(stream>>(i*8)); }
    const auto crc=Checksum(bytes); bytes[10]=static_cast<uint8_t>(crc); bytes[11]=static_cast<uint8_t>(crc>>8);
    for(int y=0;y<40;++y) std::fill_n(pixels.begin()+y*640,640,uint8_t{128});
    for(int bit=0;bit<96;++bit) {
        const bool one=((bytes[bit/8]>>(bit%8))&1)!=0;
        for(int y=6;y<34;++y) for(int x=0;x<6;++x)
            pixels[y*640+32+bit*6+x]=((y<20)==one)?235:16;
    }
}
}
RobotWorld::RobotWorld(std::string task,double y,double yaw,double width):task_(std::move(task)),corridorWidth_(width) {
    if((task_!="T1"&&task_!="T2")||!std::isfinite(y)||!std::isfinite(yaw)||!std::isfinite(width)||width<.65||width>.9)
        throw std::runtime_error("invalid robot world");
    truth_.y=y; truth_.yaw=yaw;
}
void RobotWorld::Command(double v,double w) {
    if(!std::isfinite(v)||!std::isfinite(w)) throw std::runtime_error("nonfinite robot command");
    commandV_=std::clamp(v,0.0,MaxV); commandW_=std::clamp(w,-MaxW,MaxW);
}
void RobotWorld::Step(double dt) {
    if(!std::isfinite(dt)||dt<=0||dt>.010001) throw std::runtime_error("physics requires steps <= 10 ms");
    auto& s=truth_; s.elapsed+=dt;
    if(s.collision||s.outOfBounds||s.success||s.timeout) { s.v=s.w=0; return; }
    const double oldV=s.v, oldW=s.w;
    s.v=Approach(s.v,commandV_,(commandV_<s.v?Brake:Accel)*dt);
    s.w=Approach(s.w,commandW_,AngularAccel*dt);
    const double turn=(oldW+s.w)*.5*dt, travel=(oldV+s.v)*.5*dt;
    const double nx=s.x+travel*std::cos(s.yaw+turn*.5), ny=s.y+travel*std::sin(s.yaw+turn*.5);
    if(task_=="T1") {
        for(double wallY:{-corridorWidth_*.5,corridorWidth_*.5}) {
            const double dx=nx-std::clamp(nx,0.0,3.0), dy=ny-wallY;
            if(dx*dx+dy*dy<=Radius*Radius) s.collision=true;
        }
    }
    if(!s.collision) { s.x=nx; s.y=ny; s.yaw=std::remainder(s.yaw+turn,2*Pi); }
    s.outOfBounds=s.x<-.5||s.x>4.5||std::abs(s.y)>2;
    const bool inGoal=task_=="T1"?(s.x>=3.20&&s.x<=3.35):
        (std::hypot(s.x-2.0,s.y)<=.10&&std::abs(s.yaw)<=5*Pi/180);
    s.hold=(inGoal&&s.v<=.02&&!s.collision&&!s.outOfBounds)?s.hold+dt:0;
    s.success=s.hold>=1.0;
    s.timeout=!s.success&&s.elapsed>=60.0-1e-8;
    if(s.collision||s.outOfBounds||s.success||s.timeout) s.v=s.w=0;
}
std::vector<uint8_t> RobotWorld::CaptureNv12(uint32_t id,uint32_t stream) const {
    std::vector<uint8_t> pixels(Width*Height*3/2,128);
    const double fx=Width/(2*std::tan(70*Pi/360));
    const double c=std::cos(truth_.yaw),s=std::sin(truth_.yaw);
    const double ox=truth_.x+.1*c,oy=truth_.y+.1*s,oz=.35;
    const double markerX=task_=="T1"?4.0:2.8;
    constexpr uint16_t pattern=0x8B35; // fixed, asymmetric internal 4 x 4 marker
    for(uint32_t v=40;v<Height;++v) for(uint32_t u=0;u<Width;++u) {
        const double left=-(u+.5-Width*.5)/fx,up=-(v+.5-Height*.5)/fx;
        const double dx=c-left*s,dy=s+left*c;
        double closest=30; int shade=186;
        if(up<0) {
            const double t=-oz/up;
            if(t<closest) { closest=t; const double x=ox+t*dx,y=oy+t*dy;
                shade=((static_cast<int>(std::floor(x*2))+static_cast<int>(std::floor(y*2)))&1)?126:155;
                if(std::abs(y)<.015) shade=210;
            }
        }
        if(task_=="T1"&&std::abs(dy)>1e-9) for(double wallY:{-corridorWidth_*.5,corridorWidth_*.5}) {
            const double t=(wallY-oy)/dy,x=ox+t*dx,z=oz+t*up;
            if(t>0&&t<closest&&x>=0&&x<=3&&z>=0&&z<=.8) {
                closest=t; shade=(std::fmod(x,.5)<.015||std::fmod(z,.2)<.008)?90:(wallY>0?200:174);
            }
        }
        if(dx>1e-9) {
            const double t=(markerX-ox)/dx,my=oy+t*dy,mz=oz+t*up;
            if(t>0&&t<closest&&std::abs(my)<=.145&&std::abs(mz-.35)<=.145) {
                shade=235;
                if(std::abs(my)<=.12&&std::abs(mz-.35)<=.12) {
                    const int col=std::clamp(static_cast<int>((.12-my)/.04),0,5);
                    const int row=std::clamp(static_cast<int>((.47-mz)/.04),0,5);
                    shade=(row==0||row==5||col==0||col==5)?16:((pattern>>((row-1)*4+col-1))&1)?235:16;
                }
            }
        }
        pixels[v*Width+u]=static_cast<uint8_t>(shade);
    }
    PaintIdentity(pixels,id,stream); return pixels;
}
PixelIdentity ReadPixelIdentity(const uint8_t* y,uint32_t pitch,uint32_t width,uint32_t height) {
    if(!y||width<608||height<34||pitch<width) return {};
    std::array<uint8_t,12> bytes{};
    for(int bit=0;bit<96;++bit) {
        int a=0,b=0;
        for(int j=0;j<3;++j) for(int i=0;i<2;++i) { a+=y[(11+j)*pitch+34+bit*6+i]; b+=y[(25+j)*pitch+34+bit*6+i]; }
        if(std::abs(a-b)<6*70) return {};
        if(a>b) bytes[bit/8]|=1<<(bit%8);
    }
    if(bytes[0]!=0x52||bytes[1]!=0x54||Checksum(bytes)!=(bytes[10]|bytes[11]<<8)) return {};
    PixelIdentity result; result.valid=true;
    for(int i=0;i<4;++i) { result.frameId|=uint32_t(bytes[2+i])<<(8*i); result.streamId|=uint32_t(bytes[6+i])<<(8*i); }
    return result;
}
std::string RobotWorld::ModelJson() {
    return R"({"model":"planar_kinematic_v1","coordinate_axes":"x_forward_y_left_z_up","radius_m":0.20,"max_v_m_s":0.30,"max_w_rad_s":0.8,"acceleration_m_s2":0.30,"braking_m_s2":0.60,"angular_acceleration_rad_s2":1.6,"slip":false,"camera":{"width":640,"height":360,"horizontal_fov_deg":70,"forward_m":0.10,"height_m":0.35,"distortion":false,"format":"NV12","bitrate_bps":1500000,"fps":30},"marker":{"side_m":0.24,"internal_pattern_hex":"8B35","grid_including_border":6},"command_source":"time_script_v1","script":"v=0.15 until 12s, then 0; w=0","truth_access":"renderer_evaluator_dashboard_only","visual_control":false,"reverse_command_udp":false,"diagnostic_barcode_rows":[0,39],"task_deadline_s":60,"success_hold_s":1})";
}
}
