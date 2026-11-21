#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>

static DWORD FindProcessId(const char* exeName)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{};
    pe.dwSize = sizeof(pe);
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
#ifdef UNICODE
            char name[MAX_PATH]{};
            WideCharToMultiByte(CP_ACP, 0, pe.szExeFile, -1, name, MAX_PATH, nullptr, nullptr);
            const char* currentName = name;
#else
            const char* currentName = pe.szExeFile;
#endif
            if (_stricmp(currentName, exeName) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

static bool InjectDLL(DWORD pid, const char* path)
{
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|
                           PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ, FALSE, pid);
    if (!p) { printf("OpenProcess failed: %lu\n", GetLastError()); return false; }

    SIZE_T n = strlen(path) + 1;
    void* remote = VirtualAllocEx(p, nullptr, n, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    if (!remote) { printf("VirtualAllocEx failed: %lu\n", GetLastError()); CloseHandle(p); return false; }

    if (!WriteProcessMemory(p, remote, path, n, nullptr)) {
        printf("WriteProcessMemory failed: %lu\n", GetLastError());
        VirtualFreeEx(p, remote, 0, MEM_RELEASE); CloseHandle(p); return false;
    }

    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    auto load = GetProcAddress(k32, "LoadLibraryA");
    HANDLE t = CreateRemoteThread(p, nullptr, 0,
        reinterpret_cast<LPTHREAD_START_ROUTINE>(load), remote, 0, nullptr);
    if (!t) {
        printf("CreateRemoteThread failed: %lu\n", GetLastError());
        VirtualFreeEx(p, remote, 0, MEM_RELEASE); CloseHandle(p); return false;
    }

    WaitForSingleObject(t, 5000);
    DWORD rc = 0; GetExitCodeThread(t, &rc);
    printf("Remote LoadLibrary returned: 0x%08lX\n", (unsigned long)rc);
    CloseHandle(t); VirtualFreeEx(p, remote, 0, MEM_RELEASE); CloseHandle(p);
    return rc != 0;
}

int main(int argc, char** argv)
{
    printf("WARVR launcher injector\n-----------------------\n");
    char dllPath[MAX_PATH]{};
    if (argc > 1) strncpy_s(dllPath, argv[1], _TRUNCATE);
    else GetFullPathNameA("WARVR.dll", MAX_PATH, dllPath, nullptr);

    DWORD a = GetFileAttributesA(dllPath);
    if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) {
        printf("WARVR.dll not found: %s\n", dllPath); return 2;
    }

    printf("DLL: %s\nWaiting for WAR.exe...\n", dllPath);
    DWORD injectedPid = 0;
    for (;;) {
        DWORD pid = FindProcessId("WAR.exe");
        if (pid && pid != injectedPid) {
            printf("Found WAR.exe, PID %lu\n", (unsigned long)pid);
            if (InjectDLL(pid, dllPath)) {
                printf("Injection succeeded into PID %lu\n", (unsigned long)pid);
                injectedPid = pid;
            } else printf("Injection failed for PID %lu\n", (unsigned long)pid);
        }
        Sleep(250);
    }
}
