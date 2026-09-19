#ifdef _WIN32
#include "IatHook.h"
#include <cstring>
bool HookImport(HMODULE module,const char* functionName,void* replacement,void** original){
    if(!module||!functionName||!replacement||!original)return false;
    auto*base=reinterpret_cast<unsigned char*>(module);
    auto*dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if(dos->e_magic!=IMAGE_DOS_SIGNATURE)return false;
    auto*nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    if(nt->Signature!=IMAGE_NT_SIGNATURE)return false;
    const auto&dir=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if(!dir.VirtualAddress||!dir.Size)return false;
    auto*desc=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+dir.VirtualAddress);
    for(;desc->Name;++desc){
        auto*first=reinterpret_cast<IMAGE_THUNK_DATA*>(base+desc->FirstThunk);
        auto*names=desc->OriginalFirstThunk?reinterpret_cast<IMAGE_THUNK_DATA*>(base+desc->OriginalFirstThunk):nullptr;
        if(!names)continue;
        for(;names->u1.AddressOfData;++names,++first){
            if(IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))continue;
            auto*byName=reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData);
            if(std::strcmp(reinterpret_cast<const char*>(byName->Name),functionName)!=0)continue;
            DWORD old=0;if(!VirtualProtect(&first->u1.Function,sizeof(first->u1.Function),PAGE_READWRITE,&old))return false;
            *original=reinterpret_cast<void*>(first->u1.Function);
#ifdef _WIN64
            first->u1.Function=reinterpret_cast<ULONGLONG>(replacement);
#else
            first->u1.Function=reinterpret_cast<DWORD>(replacement);
#endif
            DWORD ignored=0;VirtualProtect(&first->u1.Function,sizeof(first->u1.Function),old,&ignored);
            FlushInstructionCache(GetCurrentProcess(),&first->u1.Function,sizeof(first->u1.Function));
            return true;
        }
    }
    return false;
}
#endif
