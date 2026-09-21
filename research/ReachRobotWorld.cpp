#include "ReachRobotWorld.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace reach {
namespace {
constexpr double Pi=3.14159265358979323846;
struct CoveragePoint { double x,y; };
double PixelCoverage(const std::vector<CoveragePoint>& quad,int x,int y){
    std::array<CoveragePoint,16> polygon{},clipped{};
    size_t count=quad.size();std::copy(quad.begin(),quad.end(),polygon.begin());
    for(int edge=0;edge<4;++edge){
        size_t nextCount=0;
        auto value=[&](CoveragePoint p){return edge==0?p.x-x:edge==1?x+1-p.x:edge==2?p.y-y:y+1-p.y;};
        if(count==0)return 0;
        auto a=polygon[count-1];double da=value(a);
        for(size_t i=0;i<count;++i){const auto b=polygon[i];const double db=value(b);
            if((da>=0)!=(db>=0)){const double t=da/(da-db);clipped[nextCount++]={a.x+t*(b.x-a.x),a.y+t*(b.y-a.y)};}
            if(db>=0)clipped[nextCount++]=b;a=b;da=db;
        }
        polygon=clipped;count=nextCount;
    }
    double area=0;if(count==0)return 0;
    auto a=polygon[count-1];for(size_t i=0;i<count;++i){const auto b=polygon[i];area+=a.x*b.y-a.y*b.x;a=b;}
    return std::clamp(std::abs(area)*.5,0.,1.);
}
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
    std::vector<double> depths(Width*Height,30);
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
        pixels[v*Width+u]=static_cast<uint8_t>(shade);depths[v*Width+u]=closest;
    }
    // Integrate polygon coverage over each pixel rather than point-sample the marker.
    // This is camera rasterisation, not pose/visibility information sent to recognition.
    auto paint=[&](double left,double right,double bottom,double top,int shade){
        std::vector<CoveragePoint> quad;
        for(auto yz:{std::pair{left,top},std::pair{right,top},std::pair{right,bottom},std::pair{left,bottom}}){
            const double wx=markerX-ox,wy=yz.first-oy,z=c*wx+s*wy;
            if(z<=.01)return;
            quad.push_back({Width*.5-fx*(-s*wx+c*wy)/z,Height*.5-fx*(yz.second-oz)/z});
        }
        double minX=640,maxX=0,minY=360,maxY=0;
        for(auto p:quad){minX=std::min(minX,p.x);maxX=std::max(maxX,p.x);minY=std::min(minY,p.y);maxY=std::max(maxY,p.y);}
        for(int v=std::max(40,int(std::floor(minY)));v<std::min(360,int(std::ceil(maxY)));++v)
            for(int u=std::max(0,int(std::floor(minX)));u<std::min(640,int(std::ceil(maxX)));++u){
                const double rayX=c+(u+.5-320)/fx*s;
                if(rayX<=0||(markerX-ox)/rayX>=depths[v*640+u])continue;
                const double coverage=PixelCoverage(quad,u,v);
                pixels[v*640+u]=static_cast<uint8_t>(std::clamp(std::lround(pixels[v*640+u]*(1-coverage)+shade*coverage),0L,255L));
            }
    };
    paint(.145,-.145,.205,.495,235);
    paint(.12,-.12,.23,.47,16);
    for(int row=0;row<4;++row)for(int col=0;col<4;++col)if((pattern>>(row*4+col))&1)
        paint(.08-col*.04,.04-col*.04,.39-row*.04,.43-row*.04,235);
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
std::string RobotWorld::ModelJson(bool visualControl,bool commandUdp) {
    std::string model=R"({"model":"planar_kinematic_v1","coordinate_axes":"x_forward_y_left_z_up","radius_m":0.20,"max_v_m_s":0.30,"max_w_rad_s":0.8,"acceleration_m_s2":0.30,"braking_m_s2":0.60,"angular_acceleration_rad_s2":1.6,"slip":false,"camera":{"width":640,"height":360,"horizontal_fov_deg":70,"forward_m":0.10,"height_m":0.35,"distortion":false,"format":"NV12","bitrate_bps":1500000,"fps":30,"marker_rasterisation":"analytic_polygon_pixel_coverage"},"marker":{"side_m":0.24,"internal_pattern_hex":"8B35","grid_including_border":6},"command_source":"time_script_v1","script":"v=0.15 until 12s, then 0; w=0","truth_access":"renderer_evaluator_dashboard_only","visual_control":false,"reverse_command_udp":false,"diagnostic_barcode_rows":[0,39],"task_deadline_s":60,"success_hold_s":1})";
    if(visualControl){
        const auto at=model.find("time_script_v1");model.replace(at,std::string("time_script_v1").size(),"received_image_v1");
        const auto script=model.find("v=0.15 until 12s, then 0; w=0");model.replace(script,std::string("v=0.15 until 12s, then 0; w=0").size(),"disabled; local image command adapter");
        const auto visual=model.find("\"visual_control\":false");model.replace(visual,22,"\"visual_control\":true");
    }
    if(commandUdp){
        auto at=model.find("disabled; local image command adapter");model.replace(at,std::string("disabled; local image command adapter").size(),"disabled; reverse command UDP");
        at=model.find("\"reverse_command_udp\":false");model.replace(at,std::string("\"reverse_command_udp\":false").size(),"\"reverse_command_udp\":true");
    }
    return model;
}
}
