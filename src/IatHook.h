#pragma once
#ifdef _WIN32
#include <Windows.h>
bool HookImport(HMODULE module,const char* functionName,void* replacement,void** original);
#endif
