#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

#define TBKH2_VERSION "1.6.47"

// Log to %LOCALAPPDATA%\TurnBasedKH2\turnbased.log (also the file Melty collects on a crash).
void Log(const char* fmt, ...);
std::wstring DataDir();          // %LOCALAPPDATA%\TurnBasedKH2 (created on first use)
double NowSec();                 // monotonic seconds

extern uintptr_t g_base;         // KH2 module base

// Memory access that never crashes the game: a failed read returns the fallback.
template <typename T> inline T Rd(uintptr_t a, T fallback = T{}) {
    if (a < 0x10000) return fallback;
    __try { return *reinterpret_cast<volatile T*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return fallback; }
}
template <typename T> inline bool Wr(uintptr_t a, T v) {
    if (a < 0x10000) return false;
    __try { *reinterpret_cast<volatile T*>(a) = v; return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool RdBytes(uintptr_t a, void* out, size_t n) {
    if (a < 0x10000) return false;
    __try { memcpy(out, reinterpret_cast<const void*>(a), n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline uintptr_t G(uintptr_t rva) { return g_base + rva; }
