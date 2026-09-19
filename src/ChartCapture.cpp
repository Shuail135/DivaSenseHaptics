#ifdef _WIN32
#include "ChartCapture.h"
#include "IatHook.h"
#include "Log.h"
#include <algorithm>
#include <cwctype>
#include <vector>

namespace {
using CreateFileWFn=HANDLE(WINAPI*)(LPCWSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
using CreateFileAFn=HANDLE(WINAPI*)(LPCSTR,DWORD,DWORD,LPSECURITY_ATTRIBUTES,DWORD,DWORD,HANDLE);
CreateFileWFn gW=nullptr; CreateFileAFn gA=nullptr; ChartCapture* gCapture=nullptr;
std::wstring widenA(const char*s){if(!s)return{};int n=MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0);if(n<=1)return{};std::wstring o(static_cast<size_t>(n),L'\0');MultiByteToWideChar(CP_ACP,0,s,-1,o.data(),n);o.resize(static_cast<size_t>(n-1));return o;}
bool isDsc(std::wstring p){std::transform(p.begin(),p.end(),p.begin(),[](wchar_t c){return static_cast<wchar_t>(std::towlower(c));});return p.size()>=4&&p.substr(p.size()-4)==L".dsc";}
HANDLE WINAPI hookW(LPCWSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD creation,DWORD flags,HANDLE templ){HANDLE h=gW?gW(name,access,share,sa,creation,flags,templ):INVALID_HANDLE_VALUE;if(gCapture&&name&&h!=INVALID_HANDLE_VALUE&&isDsc(name))gCapture->OnOpened(h,name);return h;}
HANDLE WINAPI hookA(LPCSTR name,DWORD access,DWORD share,LPSECURITY_ATTRIBUTES sa,DWORD creation,DWORD flags,HANDLE templ){HANDLE h=gA?gA(name,access,share,sa,creation,flags,templ):INVALID_HANDLE_VALUE;if(gCapture&&name&&h!=INVALID_HANDLE_VALUE){auto w=widenA(name);if(isDsc(w))gCapture->OnOpened(h,w);}return h;}
}

ChartCapture::ChartCapture(Callback callback):callback_(std::move(callback)){}
ChartCapture::~ChartCapture(){Stop();}

bool ChartCapture::Start(){
    if(thread_.joinable())return true;
    gCapture=this;
    // Seed the original functions before patching the IAT so another game thread
    // cannot enter our hook during the tiny HookImport -> assignment window.
    if(HMODULE k=GetModuleHandleW(L"kernel32.dll")) {
        gW=reinterpret_cast<CreateFileWFn>(GetProcAddress(k,"CreateFileW"));
        gA=reinterpret_cast<CreateFileAFn>(GetProcAddress(k,"CreateFileA"));
    }
    void*ow=nullptr,*oa=nullptr; HMODULE main=GetModuleHandleW(nullptr);
    const bool hw=HookImport(main,"CreateFileW",reinterpret_cast<void*>(&hookW),&ow);
    const bool ha=HookImport(main,"CreateFileA",reinterpret_cast<void*>(&hookA),&oa);
    if(hw&&ow)gW=reinterpret_cast<CreateFileWFn>(ow);
    if(ha&&oa)gA=reinterpret_cast<CreateFileAFn>(oa);
    if(!hw&&!ha){gCapture=nullptr;Log::Warn("DSC chart file hook could not be installed; chart-aware multi notes/Challenge Time will fall back to judgement/input detection.");return false;}
    stop_=false; thread_=std::thread(&ChartCapture::threadMain,this);
    Log::Info("DSC chart capture hook installed (CreateFileW/A).");
    return true;
}

void ChartCapture::Stop(){
    stop_=true;cv_.notify_all();if(thread_.joinable())thread_.join();
    if(gCapture==this)gCapture=nullptr;
    std::lock_guard lock(mutex_);for(auto&i:queue_)if(i.handle!=INVALID_HANDLE_VALUE)CloseHandle(i.handle);queue_.clear();
}

void ChartCapture::OnOpened(HANDLE handle,const std::wstring&path){
    HANDLE copy=ReOpenFile(handle,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,FILE_FLAG_SEQUENTIAL_SCAN);
    if(copy==INVALID_HANDLE_VALUE)return;
    {std::lock_guard lock(mutex_);queue_.push_back({copy,path});while(queue_.size()>6){CloseHandle(queue_.front().handle);queue_.pop_front();}}
    cv_.notify_one();
}

void ChartCapture::threadMain(){
    while(!stop_){
        Item item;
        {std::unique_lock lock(mutex_);cv_.wait(lock,[&]{return stop_||!queue_.empty();});if(stop_)break;item=std::move(queue_.front());queue_.pop_front();}
        LARGE_INTEGER size{};if(!GetFileSizeEx(item.handle,&size)||size.QuadPart<=0||size.QuadPart>16ll*1024ll*1024ll){CloseHandle(item.handle);continue;}
        std::vector<uint8_t> bytes(static_cast<size_t>(size.QuadPart));DWORD total=0;
        while(total<bytes.size()){DWORD got=0;DWORD req=static_cast<DWORD>(std::min<size_t>(bytes.size()-total,1u<<20));if(!ReadFile(item.handle,bytes.data()+total,req,&got,nullptr)||!got)break;total+=got;}
        CloseHandle(item.handle);
        if(total!=bytes.size())continue;
        Log::Info("Captured DIVA DSC chart: "+std::string(item.path.begin(),item.path.end()));
        auto chart=DscChart::Parse(bytes,item.path);
        if(chart.valid&&callback_)callback_(std::move(chart));
    }
}
#endif
