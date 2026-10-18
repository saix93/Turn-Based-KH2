// KH2 x FFX: Turn-Based Twilight. Loads as dinput8.dll beside the KINGDOM HEARTS HD 1.5+2.5 executables
// (KH2 imports DINPUT8.dll), forwards DirectInput to the system DLL, and only does anything inside KH2.
#include "common.h"
#include "game.h"
#include "battle.h"
#include "ui.h"
#include "testmode.h"
#include "gen/sheets.h"
#include "MinHook.h"
#include <cstdio>
#include <share.h>
#include <cstdarg>

uintptr_t g_base = 0;

namespace {
HMODULE g_real = nullptr;
std::mutex g_logLock;
FILE* g_log = nullptr;

using LoopFn = void(__fastcall*)(void*);
LoopFn oLoop = nullptr;
int g_tickFaults = 0;

void LoadReal() {
    if (g_real) return;
    wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH);
    wcscat_s(p, L"\\dinput8.dll");
    g_real = LoadLibraryW(p);
}
FARPROC Real(const char* name) { LoadReal(); return g_real ? GetProcAddress(g_real, name) : nullptr; }

void TickGuarded() {
    __try { testmode::Poll(); battle::Tick(); game::PreLoop(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (++g_tickFaults <= 5) Log("tick fault %d (code %08X) - battle logic skipped this frame", g_tickFaults, GetExceptionCode());
    }
}
void __fastcall hkLoop(void* a) {
    if (g_tickFaults < 50) TickGuarded();
    oLoop(a);
}

DWORD WINAPI InitThread(LPVOID) {
    Log("KH2 x FFX %s starting (pid %lu)", FFXTB_VERSION, GetCurrentProcessId());
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    // Wait for the game to unpack and initialise its tables.
    for (int i = 0; i < 300 && !game::VerifyExe(); ++i) { if (i == 0) Log("waiting for the executable to match..."); Sleep(100); }
    if (!game::VerifyExe()) {
        Log("This KH2 executable is not the Steam build the mod was made for; the mod stays off.");
        return 0;
    }
    testmode::Init();
    battle::Init();
    if (MH_Initialize() != MH_OK) { Log("MinHook init failed"); return 0; }
    void* loop = (void*)G(sheets::RVA_EntityUpdateLoop);
    bool ok = MH_CreateHook(loop, (void*)hkLoop, (void**)&oLoop) == MH_OK && MH_EnableHook(loop) == MH_OK;
    Log("entity loop hook: %s", ok ? "installed" : "FAILED");
    bool fh = game::InstallFreezeHook();
    Log("freeze hook: %s", fh ? "installed" : "FAILED");
    ok &= fh;
    ok &= game::InstallStageHooks();
    bool r = render::InstallHooks();
    if (ok) testmode::Pass("hooks-installed");
    (void)r;
    return 0;
}

bool IsKh2() {
    wchar_t p[MAX_PATH]; GetModuleFileNameW(nullptr, p, MAX_PATH);
    std::wstring s = p; size_t i = s.find_last_of(L"\\/");
    std::wstring n = i == std::wstring::npos ? s : s.substr(i + 1);
    for (auto& c : n) c = towlower(c);
    return n == L"kingdom hearts ii final mix.exe";
}
}

std::wstring DataDir() {
    static std::wstring d;
    if (d.empty()) {
        wchar_t buf[MAX_PATH]; GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        d = std::wstring(buf) + L"\\KH2FFXTB";
        CreateDirectoryW(d.c_str(), nullptr);
    }
    return d;
}
double NowSec() {
    static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logLock);
    if (!g_log) {
        // The previous session's log is kept beside it (ffxtb.prev.log).
        std::wstring path = DataDir() + L"\\ffxtb.log";
        MoveFileExW(path.c_str(), (DataDir() + L"\\ffxtb.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        g_log = _wfsopen(path.c_str(), L"w", _SH_DENYNO); if (!g_log) return;
    }
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

extern "C" {
HRESULT WINAPI DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, void* u) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, void*);
    auto f = (Fn)Real("DirectInput8Create");
    return f ? f(h, v, r, o, u) : E_FAIL;
}
HRESULT WINAPI DllCanUnloadNow() { using Fn = HRESULT(WINAPI*)(); auto f = (Fn)Real("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) { using Fn = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*); auto f = (Fn)Real("DllGetClassObject"); return f ? f(c, r, o) : E_FAIL; }
HRESULT WINAPI DllRegisterServer() { using Fn = HRESULT(WINAPI*)(); auto f = (Fn)Real("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI DllUnregisterServer() { using Fn = HRESULT(WINAPI*)(); auto f = (Fn)Real("DllUnregisterServer"); return f ? f() : E_FAIL; }
LPCVOID WINAPI GetdfDIJoystick() { using Fn = LPCVOID(WINAPI*)(); auto f = (Fn)Real("GetdfDIJoystick"); return f ? f() : nullptr; }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        if (IsKh2()) CloseHandle(CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr));
    }
    return TRUE;
}
