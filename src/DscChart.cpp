#include "DscChart.h"
#include "Log.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace {
constexpr int kParamCount[0x6B] = {
    0,1,4,2,2,2,7,4,2,6,2,1,6,2,1,1,
    3,2,3,5,5,4,4,5,2,0,2,4,2,2,1,21,
    0,3,2,5,1,1,7,1,1,2,1,2,1,2,3,3,
    1,2,2,3,6,6,1,1,2,3,1,2,2,4,4,1,
    2,1,2,1,1,3,3,3,2,1,9,3,2,4,2,3,
    2,24,1,2,1,3,1,3,4,1,2,6,3,2,3,3,
    4,1,1,3,3,4,2,3,3,8,2
};

uint32_t readU32(const std::vector<uint8_t>& b,size_t off){
    uint32_t v=0; std::memcpy(&v,b.data()+off,sizeof(v)); return v;
}

struct RawTarget { double hit=0.0; int type=-1; bool challenge=false; };

uint8_t faceForTarget(int t){
    switch(t){
    case 0: case 4: case 18: return 0x08; // triangle
    case 1: case 5: case 19: return 0x04; // circle
    case 2: case 6: case 20: return 0x02; // cross
    case 3: case 7: case 21: return 0x01; // square
    default: return 0;
    }
}
uint8_t holdForTarget(int t){
    switch(t){case 4:return 0x08;case 5:return 0x04;case 6:return 0x02;case 7:return 0x01;default:return 0;}
}
uint8_t slideForTarget(int t){
    switch(t){case 12:case 15:case 23:return 0x01;case 13:case 16:case 24:return 0x02;default:return 0;}
}
uint8_t chainForTarget(int t){ return t==15?0x01:(t==16?0x02:0); }
uint8_t specialFaceForTarget(int t){ return (t>=18&&t<=21)?faceForTarget(t):0; }
uint8_t specialSlideForTarget(int t){ return (t==23||t==24)?slideForTarget(t):0; }
bool looksLikeOpcode(uint32_t v){return v<=0x6A;}
}

DscChart DscChart::Parse(const std::vector<uint8_t>& bytes,const std::wstring& sourcePath,
                         int groupWindowMs,int chainGapMs){
    DscChart out; out.sourcePath=sourcePath;
    if(bytes.size()<8 || (bytes.size()%4)!=0) return out;
    size_t pos=0;
    if(!looksLikeOpcode(readU32(bytes,0))) pos=4;
    double currentSeconds=0.0, flyingMs=1000.0;
    bool challenge=false;
    std::vector<RawTarget> raw;
    while(pos+4<=bytes.size()){
        const uint32_t opcode=readU32(bytes,pos); pos+=4;
        if(opcode>0x6A) break;
        const int count=kParamCount[opcode];
        const size_t need=static_cast<size_t>(count)*4;
        if(pos+need>bytes.size()) break;
        std::array<int32_t,32> p{};
        for(int i=0;i<count;++i) p[static_cast<size_t>(i)]=static_cast<int32_t>(readU32(bytes,pos+static_cast<size_t>(i)*4));
        switch(opcode){
        case 0x00: pos=bytes.size(); continue;
        case 0x01: currentSeconds=static_cast<double>(p[0])/100000.0; break;
        case 0x06: raw.push_back({currentSeconds+flyingMs/1000.0,p[0],challenge}); break;
        case 0x1A:
            if(p[0]==31){
                if(p[1]==1){challenge=true;out.challengeMarkers.push_back({currentSeconds,true});}
                else if(p[1]==3){challenge=false;out.challengeMarkers.push_back({currentSeconds,false});}
            }
            break;
        case 0x1C:
            if(p[0]>0 && p[1]>=0){
                const double bpm=static_cast<double>(p[0])/100.0;
                if(bpm>1.0) flyingMs=(60.0/bpm)*static_cast<double>(p[1]+1)*1000.0;
            }
            break;
        case 0x3A:
            if(p[0]>0 && p[0]<15000) flyingMs=static_cast<double>(p[0]);
            break;
        default: break;
        }
        pos+=need;
    }
    std::sort(out.challengeMarkers.begin(),out.challengeMarkers.end(),[](const auto&a,const auto&b){return a.seconds<b.seconds;});
    if(raw.empty()) return out;
    std::sort(raw.begin(),raw.end(),[](const auto&a,const auto&b){return a.hit<b.hit;});
    const double groupWindow=std::max(0,groupWindowMs)/1000.0;
    for(const auto&t:raw){
        if(out.groups.empty() || std::abs(out.groups.back().hitSeconds-t.hit)>groupWindow){
            DscChartGroup g; g.hitSeconds=t.hit; g.challenge=t.challenge; out.groups.push_back(g);
        }
        auto&g=out.groups.back();
        g.faceMask|=faceForTarget(t.type);
        g.holdMask|=holdForTarget(t.type);
        const uint8_t slide=slideForTarget(t.type);
        g.slideMask|=slide;
        if(slide && g.slideTargetCount<255) ++g.slideTargetCount;
        g.chainMask|=chainForTarget(t.type);
        g.specialFaceMask|=specialFaceForTarget(t.type);
        g.specialSlideMask|=specialSlideForTarget(t.type);
        g.challenge=g.challenge||t.challenge;
    }
    const double chainGap=std::max(1,chainGapMs)/1000.0;
    for(size_t i=0;i<out.groups.size();++i){
        if(!out.groups[i].slideMask) continue;
        bool nearPrev=false,nearNext=false;
        if(i>0 && out.groups[i-1].slideMask) nearPrev=(out.groups[i].hitSeconds-out.groups[i-1].hitSeconds)<=chainGap;
        if(i+1<out.groups.size() && out.groups[i+1].slideMask) nearNext=(out.groups[i+1].hitSeconds-out.groups[i].hitSeconds)<=chainGap;
        if(nearPrev||nearNext) out.groups[i].chainMask|=out.groups[i].slideMask;
        if(out.groups[i].chainMask && !nearNext) out.groups[i].chainEnd=true;
    }
    out.valid=!out.groups.empty();
    if(out.valid) Log::Info("Parsed DIVA DSC chart: "+std::to_string(out.groups.size())+" target groups, "+std::to_string(out.challengeMarkers.size())+" Challenge markers.");
    return out;
}

bool DscChart::ChallengeAt(double chartSeconds) const{
    bool active=false;
    for(const auto&m:challengeMarkers){if(m.seconds>chartSeconds)break;active=m.start;}
    return active;
}
