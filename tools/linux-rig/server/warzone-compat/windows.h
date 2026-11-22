// Minimal Win32 shim so ProjectWAR's WarZone (LOS) library builds as a Linux .so.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <cmath>
#include <cstring>

#define _fseeki64 fseeko
#define _ftelli64 ftello

#define __int64 long long
#define WINAPI
#define TRUE 1
#define FALSE 0
#define DLL_PROCESS_ATTACH 1
#define DLL_PROCESS_DETACH 0
#define __declspec(x) __attribute__((visibility("default")))

typedef void* HANDLE;
typedef uint32_t DWORD;
typedef int BOOL;
typedef unsigned long ULONG;
typedef void* LPVOID;
typedef union { struct { uint32_t LowPart; int32_t HighPart; }; long long QuadPart; } LARGE_INTEGER;

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) { f->QuadPart = 1000000000LL; return TRUE; }
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c) {
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    c->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec; return TRUE;
}

// CreateFile/GetFileSizeEx are only used to get a file size.
#define GENERIC_READ 0
#define FILE_SHARE_READ 0
#define FILE_SHARE_WRITE 0
#define OPEN_EXISTING 0
#define FILE_ATTRIBUTE_NORMAL 0
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
struct WzFileHandle { long long size; };
inline HANDLE CreateFile(const wchar_t* name, int, int, void*, int, int, void*) {
    std::string n; for (const wchar_t* p = name; *p; ++p) n += (char)*p;
    struct stat st; if (stat(n.c_str(), &st) != 0) return INVALID_HANDLE_VALUE;
    WzFileHandle* h = new WzFileHandle; h->size = st.st_size; return h;
}
inline BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER* s) { s->QuadPart = ((WzFileHandle*)h)->size; return TRUE; }
inline BOOL CloseHandle(HANDLE h) { if (h && h != INVALID_HANDLE_VALUE) delete (WzFileHandle*)h; return TRUE; }

// Memory diagnostics: no-op on Linux.
#define PROCESS_QUERY_INFORMATION 0
#define PROCESS_VM_READ 0
struct PROCESS_MEMORY_COUNTERS { size_t WorkingSetSize; };
inline DWORD GetCurrentProcessId() { return (DWORD)getpid(); }
inline HANDLE OpenProcess(int, BOOL, DWORD) { return nullptr; }
inline BOOL GetProcessMemoryInfo(HANDLE, PROCESS_MEMORY_COUNTERS*, size_t) { return FALSE; }
