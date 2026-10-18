// Dev tool: finds the busiest thread of a process and samples where it is executing (module+offset),
// plus a few return addresses from its stack. usage: ripsample <pid>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdio>
#include <vector>
#include <string>
#include <map>
#pragma comment(lib, "psapi.lib")

struct Mod { uintptr_t base, size; std::string name; };
std::vector<Mod> mods;
std::string Where(uintptr_t a) {
    for (auto& m : mods) if (a >= m.base && a < m.base + m.size) { char b[300]; snprintf(b, sizeof b, "%s+0x%llX", m.name.c_str(), (unsigned long long)(a - m.base)); return b; }
    char b[64]; snprintf(b, sizeof b, "0x%llX", (unsigned long long)a); return b;
}
int main(int argc, char** argv) {
    DWORD pid = atoi(argv[1]);
    HANDLE p = OpenProcess(PROCESS_ALL_ACCESS, FALSE, pid);
    HMODULE hm[1024]; DWORD need;
    EnumProcessModules(p, hm, sizeof hm, &need);
    for (DWORD i = 0; i < need / sizeof(HMODULE); ++i) { MODULEINFO mi; char n[MAX_PATH]; GetModuleInformation(p, hm[i], &mi, sizeof mi); GetModuleBaseNameA(p, hm[i], n, MAX_PATH); mods.push_back({(uintptr_t)mi.lpBaseOfDll, mi.SizeOfImage, n}); }
    std::map<DWORD, ULONGLONG> t0;
    auto snap = [&](std::map<DWORD, ULONGLONG>& out) {
        HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0); THREADENTRY32 te{sizeof te};
        for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te)) if (te.th32OwnerProcessID == pid) {
            HANDLE t = OpenThread(THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID); FILETIME c, e, k, u;
            if (t && GetThreadTimes(t, &c, &e, &k, &u)) out[te.th32ThreadID] = ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime) + ((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime);
            if (t) CloseHandle(t);
        }
        CloseHandle(s);
    };
    snap(t0); Sleep(1000); std::map<DWORD, ULONGLONG> t1; snap(t1);
    DWORD best = 0; ULONGLONG bd = 0;
    for (auto& kv : t1) { ULONGLONG d = kv.second - t0[kv.first]; if (d > bd) { bd = d; best = kv.first; } }
    printf("busiest thread %lu (%.0f%%)\n", best, bd / 100000.0);
    HANDLE t = OpenThread(THREAD_ALL_ACCESS, FALSE, best);
    for (int i = 0; i < 12; ++i) {
        SuspendThread(t); CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_FULL; GetThreadContext(t, &ctx);
        printf("rip %s", Where(ctx.Rip).c_str());
        // scan the stack for return addresses into known modules
        uintptr_t stk[256]; SIZE_T got = 0; ReadProcessMemory(p, (void*)ctx.Rsp, stk, sizeof stk, &got);
        int shown = 0;
        for (size_t k = 0; k < got / 8 && shown < 8; ++k) { std::string w = Where(stk[k]); if (w[0] != '0') { printf(" | %s", w.c_str()); ++shown; } }
        printf("\n");
        ResumeThread(t); Sleep(150);
    }
    return 0;
}
