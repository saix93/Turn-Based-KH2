#include "testmode.h"
#include <set>
#include <fstream>
#include <sstream>

namespace {
std::mutex g_m;
bool g_active = false;
std::wstring g_checksPath, g_statePath;
std::string g_testId;
std::set<std::string> g_passed;
std::vector<std::string> g_early;   // checks passed before the test file was seen
double g_lastPoll = -10;
int g_encounters = 0;

std::wstring TestDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    return n ? std::wstring(buf) + L"\\Melty\\test" : L"";
}
std::string JsonString(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k); if (p == std::string::npos) return "";
    p = j.find(':', p); if (p == std::string::npos) return "";
    p = j.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos) return "";
    if (j[p] != '"') { size_t e = j.find_first_of(",}", p); return j.substr(p, e - p); }
    std::string out;
    for (size_t i = p + 1; i < j.size() && j[i] != '"'; ++i) {
        if (j[i] == '\\' && i + 1 < j.size()) { ++i; out += j[i] == 'n' ? '\n' : j[i]; }
        else out += j[i];
    }
    return out;
}
void Append(const std::string& line) {
    std::ofstream f(g_checksPath, std::ios::app | std::ios::binary);
    f << line << "\n";
}
}

namespace testmode {

void Init() { Poll(); }

void Poll() {
    double now = NowSec();
    if (now - g_lastPoll < 1.5) return;
    g_lastPoll = now;
    std::wstring dir = TestDir(); if (dir.empty()) return;
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    std::wstring newest; FILETIME best{};
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            // The mashup's test file, by its slug (it was renamed once; the newest file wins).
            if (_wcsicmp(n.c_str(), L"turn-based-kingdom-hearts-2.json") != 0 && _wcsicmp(n.c_str(), L"roxas-s-summer-turn-based.json") != 0) continue;
            if (CompareFileTime(&fd.ftLastWriteTime, &best) > 0) { best = fd.ftLastWriteTime; newest = dir + L"\\" + n; }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::lock_guard<std::mutex> lk(g_m);
    if (newest.empty()) { if (g_active) Log("test mode: test file gone"); g_active = false; return; }
    std::ifstream f(newest, std::ios::binary); std::stringstream ss; ss << f.rdbuf();
    std::string j = ss.str();
    std::string id = JsonString(j, "testId") + "/" + JsonString(j, "run");
    std::string checks = JsonString(j, "checks");
    if (checks.empty()) return;
    if (!g_active || id != g_testId) {
        g_active = true; g_testId = id; g_passed.clear(); g_encounters = 0;
        int len = MultiByteToWideChar(CP_UTF8, 0, checks.c_str(), -1, nullptr, 0);
        std::wstring w(len, 0); MultiByteToWideChar(CP_UTF8, 0, checks.c_str(), -1, &w[0], len); w.resize(len - 1);
        g_checksPath = w;
        std::string st = JsonString(j, "state");
        if (!st.empty()) {
            int l2 = MultiByteToWideChar(CP_UTF8, 0, st.c_str(), -1, nullptr, 0);
            std::wstring w2(l2, 0); MultiByteToWideChar(CP_UTF8, 0, st.c_str(), -1, &w2[0], l2); w2.resize(l2 - 1);
            g_statePath = w2;
        }
        Log("test mode on: test %s, checks -> %s", id.c_str(), checks.c_str());
        for (auto& c : g_early) if (g_passed.insert(c).second) Append("pass " + c);
    }
}

bool Active() { std::lock_guard<std::mutex> lk(g_m); return g_active; }
bool WantEncounter() { std::lock_guard<std::mutex> lk(g_m); return g_active && g_encounters < 3; } // up to three battles a run
void EncounterStarted() { std::lock_guard<std::mutex> lk(g_m); ++g_encounters; }

void Pass(const char* check) {
    std::lock_guard<std::mutex> lk(g_m);
    if (!g_active) {
        // hooks-installed can happen before Melty's file is read: keep them for the run.
        if (std::find(g_early.begin(), g_early.end(), check) == g_early.end()) g_early.push_back(check);
        return;
    }
    if (!g_passed.insert(check).second) return;
    Append(std::string("pass ") + check);
    Log("test check passed: %s", check);
}

void WriteState(const std::string& json) {
    std::wstring p;
    { std::lock_guard<std::mutex> lk(g_m); if (!g_active || g_statePath.empty()) return; p = g_statePath; }
    std::wstring tmp = p + L".tmp";
    { std::ofstream f(tmp, std::ios::binary | std::ios::trunc); f << json; }
    MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING);
}
}
