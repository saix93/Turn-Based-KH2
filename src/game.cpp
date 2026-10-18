#include "game.h"
#include "gen/sheets.h"
#include "MinHook.h"
#include <cmath>
#include <intrin.h>
#include <algorithm>

using namespace sheets;

namespace {
constexpr uintptr_t ACTOR_FLAGS120 = 0x120;
constexpr uint32_t FREEZE_BIT = 0x00080000;     // tested by 0x3BAA80 (mask 0x10080000)
constexpr uint32_t DYING_BIT = 0x10000000;
constexpr uintptr_t ACTOR_NEXT = 0xA90, ACTOR_OBJENTRY = 0x918, ACTOR_STATUS = 0x5C0, ACTOR_ANIM = 0x180, ACTOR_POS = 0x640 + 0x30;
constexpr uintptr_t CAMERA_STRUCT = 0x718C60;

using ResolveFn = uintptr_t(__fastcall*)(uint32_t);
using RequestFn = void(__fastcall*)(const void* packet, uint32_t fadeFlags, int mode, uint8_t flag, int extra);
using TakeDamageFn = void(__fastcall*)(void* actor, int delta, int idx, uint8_t react);

bool CheckSig(uintptr_t rva, const uint8_t* sig) {
    uint8_t buf[16];
    if (!RdBytes(G(rva), buf, 16)) return false;
    return memcmp(buf, sig, 16) == 0;
}
}

namespace game {
std::atomic<uintptr_t> g_itemCmdActor{0}; std::atomic<int> g_itemCmdCount{0};

bool VerifyExe() {
    struct { const char* n; uintptr_t r; const uint8_t* s; } f[] = {
        {"EntityUpdateLoop", RVA_EntityUpdateLoop, SIG_EntityUpdateLoop},
        {"RequestTransition", RVA_RequestTransition, SIG_RequestTransition},
        {"TakeDamage", RVA_TakeDamage, SIG_TakeDamage},
        {"HandleResolve", RVA_HandleResolve, SIG_HandleResolve},
    };
    bool ok = true;
    for (auto& x : f) {
        bool m = CheckSig(x.r, x.s);
        Log("verify %s @+0x%llX: %s", x.n, (unsigned long long)x.r, m ? "ok" : "MISMATCH");
        ok &= m;
    }
    return ok;
}

Location Now() {
    Location l{};
    uintptr_t a = G(RVA_Now);
    l.world = Rd<uint8_t>(a); l.room = Rd<uint8_t>(a + 1); l.door = Rd<uint8_t>(a + 2);
    l.map = Rd<uint16_t>(a + 4); l.btl = Rd<uint16_t>(a + 6); l.evt = Rd<uint16_t>(a + 8);
    return l;
}
bool InField() { return Rd<uint8_t>(G(RVA_InField)) != 0; }
bool CutsceneOrEvent() { return Rd<int32_t>(G(RVA_CutsceneState)) != 0 || Rd<uint64_t>(G(RVA_EventContext)) != 0; }
bool MenuOpen() { return Rd<uint8_t>(G(RVA_OpenMenu), 0xFF) != 0xFF; }
bool SafeGameplay() { return InField() && !CutsceneOrEvent() && !MenuOpen(); }
int RoxasHp() { return Rd<int32_t>(G(RVA_Slot0)); }
int RoxasMaxHp() { return Rd<int32_t>(G(RVA_Slot0) + 4); }
int SoraMp() { return Rd<int32_t>(G(RVA_Slot0) + 0x180); }
int SoraMaxMp() { return Rd<int32_t>(G(RVA_Slot0) + 0x184); }
int SaveByte(uint32_t off) { return Rd<uint8_t>(G(RVA_SaveBase) + off); }
void SetSaveByte(uint32_t off, int v) { Wr<uint8_t>(G(RVA_SaveBase) + off, (uint8_t)v); }

uint32_t ResolveHandle(uint32_t h, uintptr_t* out) {
    *out = 0;
    if (!h) return 0;
    __try { *out = reinterpret_cast<ResolveFn>(G(RVA_HandleResolve))(h); } __except (EXCEPTION_EXECUTE_HANDLER) { *out = 0; }
    return *out ? 1 : 0;
}

bool ReadActor(uintptr_t actor, ActorInfo& a) {
    a = {};
    a.actor = actor;
    a.objentry = Rd<uintptr_t>(actor + ACTOR_OBJENTRY);
    if (a.objentry <= g_base || a.objentry >= g_base + 0x4000000) return false;
    a.objectId = Rd<uint32_t>(a.objentry);
    a.handle = Rd<uint32_t>(actor);
    a.type = Rd<uint8_t>(a.objentry + 4);
    RdBytes(a.objentry + 8, a.name, 32); a.name[32] = 0;
    a.status = Rd<uintptr_t>(actor + ACTOR_STATUS);
    if (a.status) { a.hp = Rd<int32_t>(a.status); a.maxHp = Rd<int32_t>(a.status + 4); }
    a.x = Rd<float>(actor + ACTOR_POS); a.y = Rd<float>(actor + ACTOR_POS + 4); a.z = Rd<float>(actor + ACTOR_POS + 8);
    a.anim = Rd<uint32_t>(actor + ACTOR_ANIM);
    return true;
}

void EnumActors(std::vector<ActorInfo>& out) {
    out.clear();
    uintptr_t a = Rd<uintptr_t>(G(RVA_ActiveListHead));
    for (int i = 0; a && i < 256; ++i) {
        ActorInfo info;
        if (ReadActor(a, info)) out.push_back(info);
        uint32_t h = Rd<uint32_t>(a + ACTOR_NEXT);
        uintptr_t next = 0;
        if (!ResolveHandle(h, &next) || next == a) break;
        a = next;
    }
}

bool IsDying(uintptr_t actor) { return (Rd<uint32_t>(actor + ACTOR_FLAGS120) & DYING_BIT) != 0; }

// Actors we froze. KH2's update loop (0x3BF5E0) updates children (weapons, attachments) only after their
// parent sets "updated this frame" (+0x9B8 bit 11); a frozen parent never does, and the loop would spin
// forever. PreLoop marks our frozen actors as updated before the native loop runs.
std::mutex g_frozenLock;
std::vector<uintptr_t> g_frozen;
void SyncFast();

void SetFrozen(uintptr_t actor, bool frozen) {
    uint32_t f = Rd<uint32_t>(actor + ACTOR_FLAGS120);
    uint32_t n = frozen ? (f | FREEZE_BIT) : (f & ~FREEZE_BIT);
    if (n != f) Wr<uint32_t>(actor + ACTOR_FLAGS120, n);
    std::lock_guard<std::mutex> lk(g_frozenLock);
    auto it = std::find(g_frozen.begin(), g_frozen.end(), actor);
    if (frozen && it == g_frozen.end()) g_frozen.push_back(actor);
    if (!frozen && it != g_frozen.end()) g_frozen.erase(it);
    SyncFast();
}

void PreLoop() {
    // Forget frozen actors that left the active list (they may be freed).
    std::lock_guard<std::mutex> lk(g_frozenLock);
    if (g_frozen.empty()) return;
    std::vector<uintptr_t> alive;
    uintptr_t a = Rd<uintptr_t>(G(RVA_ActiveListHead));
    for (int i = 0; a && i < 512; ++i) {
        if (std::find(g_frozen.begin(), g_frozen.end(), a) != g_frozen.end()) alive.push_back(a);
        uintptr_t next = 0;
        if (!ResolveHandle(Rd<uint32_t>(a + ACTOR_NEXT), &next) || next == a) break;
        a = next;
    }
    g_frozen.swap(alive);
    SyncFast();
}

namespace {
using FrozenFn = bool(__fastcall*)(uintptr_t);
FrozenFn oFrozen = nullptr;
uintptr_t g_frozenFast[64]; std::atomic<int> g_frozenFastN{0};
uintptr_t g_free[8]; std::atomic<int> g_freeN{0};   // actors KH2 must keep updating whatever freezes are on
bool __fastcall hkFrozen(uintptr_t actor) {
    int fn = g_freeN.load();
    for (int i = 0; i < fn; ++i) if (g_free[i] == actor) return false;
    bool r = oFrozen(actor);
    // Only from the per-entity update (0x3BFD30 calls it at 0x3BFD8A): that is the moment the actor
    // is processed this frame. Other callers run before KH2's loop has counted its pending actors.
    if (r && reinterpret_cast<uintptr_t>(_ReturnAddress()) == G(0x3BFD8F)) {
        int n = g_frozenFastN.load();
        for (int i = 0; i < n; ++i) if (g_frozenFast[i] == actor) {
            // Called at the top of KH2's per-entity update: mark the actor updated so children can follow.
            *reinterpret_cast<volatile uint32_t*>(actor + 0x9B8) |= 0x800;
            break;
        }
    }
    return r;
}
}

void SetUnfreezable(uintptr_t actor, bool on) {
    int n = g_freeN.load(), at = -1;
    for (int i = 0; i < n; ++i) if (g_free[i] == actor) at = i;
    if (on && at < 0 && n < 8) { g_free[n] = actor; g_freeN.store(n + 1); }
    else if (!on && at >= 0) { g_free[at] = g_free[n - 1]; g_freeN.store(n - 1); }
}

bool InstallFreezeHook() {
    uint8_t buf[16];
    if (!RdBytes(G(RVA_FreezePredicate), buf, 16) || memcmp(buf, SIG_FreezePredicate, 16)) return false;
    return MH_CreateHook((void*)G(RVA_FreezePredicate), (void*)hkFrozen, (void**)&oFrozen) == MH_OK &&
           MH_EnableHook((void*)G(RVA_FreezePredicate)) == MH_OK;
}

bool RequestTransition(const Location& to) {
    alignas(16) uint8_t packet[16] = {};
    packet[0] = to.world; packet[1] = to.room;
    memcpy(packet + 2, &to.door, 2); memcpy(packet + 4, &to.map, 2);
    memcpy(packet + 6, &to.btl, 2); memcpy(packet + 8, &to.evt, 2);
    __try {
        reinterpret_cast<RequestFn>(G(RVA_RequestTransition))(packet, 1, 0, 0, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

int TakeDamage(uintptr_t actor, int delta, bool react) {
    uintptr_t st = Rd<uintptr_t>(actor + ACTOR_STATUS);
    if (!st) return -1;
    __try {
        reinterpret_cast<TakeDamageFn>(G(RVA_TakeDamage))(reinterpret_cast<void*>(actor), delta, 0, react ? 1 : 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
    return Rd<int32_t>(st);
}

bool SetPos(uintptr_t actor, float x, float y, float z) {
    return Wr<float>(actor + ACTOR_POS, x) && Wr<float>(actor + ACTOR_POS + 4, y) && Wr<float>(actor + ACTOR_POS + 8, z);
}

int InventoryCount(uint32_t off) { return Rd<uint8_t>(G(RVA_SaveBase) + off); }
void SetInventoryCount(uint32_t off, int n) { Wr<uint8_t>(G(RVA_SaveBase) + off, (uint8_t)std::max(0, std::min(99, n))); }

// Approximate projection from KH2's camera (eased eye at +0x18, eased look-at at +0x08, Y down).
bool WorldToScreen(float x, float y, float z, float sw, float sh, float& ox, float& oy) {
    uintptr_t c = G(CAMERA_STRUCT);
    float ex = Rd<float>(c + 0x18), ey = Rd<float>(c + 0x1C), ez = Rd<float>(c + 0x20);
    float lx = Rd<float>(c + 0x08), ly = Rd<float>(c + 0x0C), lz = Rd<float>(c + 0x10);
    float fx = lx - ex, fy = ly - ey, fz = lz - ez;
    float fl = std::sqrt(fx * fx + fy * fy + fz * fz); if (fl < 1e-3f) return false;
    fx /= fl; fy /= fl; fz /= fl;
    // KH2 uses -Y as up; build right/up from world up (0,-1,0).
    float ux0 = 0, uy0 = -1, uz0 = 0;
    float rx = fy * uz0 - fz * uy0, ry = fz * ux0 - fx * uz0, rz = fx * uy0 - fy * ux0;
    float rl = std::sqrt(rx * rx + ry * ry + rz * rz); if (rl < 1e-3f) return false;
    rx /= rl; ry /= rl; rz /= rl;
    float ux = ry * fz - rz * fy, uy = rz * fx - rx * fz, uz = rx * fy - ry * fx;
    float dx = x - ex, dy = y - ey, dz = z - ez;
    float cz = dx * fx + dy * fy + dz * fz; if (cz < 10.f) return false;
    float cx = dx * rx + dy * ry + dz * rz, cy = dx * ux + dy * uy + dz * uz;
    const float tanHalf = 0.57f; // fitted to KH2's view from in-game screenshots
    float aspect = sw / sh;
    ox = sw * 0.5f * (1.f + cx / (cz * tanHalf * aspect)); // KH2's view is mirrored against this basis
    oy = sh * 0.5f * (1.f - cy / (cz * tanHalf));
    return ox > -sw && ox < 2 * sw && oy > -sh && oy < 2 * sh;
}
}

namespace game {
void SyncFast() {
    // g_frozenLock is held by the caller.
    int n = (int)std::min<size_t>(g_frozen.size(), 64);
    for (int i = 0; i < n; ++i) g_frozenFast[i] = g_frozen[i];
    g_frozenFastN.store(n);
}
}

namespace {
using SetRotFn = void(__fastcall*)(uintptr_t actor, float yaw);
using CamApplyFn = uintptr_t(__fastcall*)(uintptr_t cam, uint8_t flag);
using CollectFn = uintptr_t(__fastcall*)(void* input);
CamApplyFn oCamApply = nullptr;
CollectFn oCollect = nullptr;

SRWLOCK g_stageLock = SRWLOCK_INIT;
bool g_camOn = false; float g_camEye[3]{}, g_camLook[3]{};
std::atomic<bool> g_inputLocked{false};
std::atomic<uint16_t> g_press{0};
// The battle logic re-asserts both every frame; if it stops (pause, load, a crash in our tick),
// they lapse on their own after a moment so the game is never left without control.
std::atomic<double> g_camStamp{-100}, g_lockStamp{-100};
constexpr double STAGE_LAPSE_SEC = 0.75;

uintptr_t __fastcall hkCamApply(uintptr_t cam, uint8_t flag) {
    bool on; float e[3], l[3];
    AcquireSRWLockShared(&g_stageLock);
    on = g_camOn; memcpy(e, g_camEye, sizeof e); memcpy(l, g_camLook, sizeof l);
    ReleaseSRWLockShared(&g_stageLock);
    if (on && cam == G(RVA_CameraStruct) && NowSec() - g_camStamp.load() < STAGE_LAPSE_SEC) {
        // Raw targets (and their previous-frame copies); KH2 then eases +0x18/+0x08 toward them.
        __try {
            memcpy(reinterpret_cast<void*>(cam + 0x64), e, sizeof e); memcpy(reinterpret_cast<void*>(cam + 0x74), e, sizeof e);
            memcpy(reinterpret_cast<void*>(cam + 0x84), l, sizeof l); memcpy(reinterpret_cast<void*>(cam + 0x94), l, sizeof l);
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return oCamApply(cam, flag);
}

using CmdExecHookFn = uintptr_t(__fastcall*)(uintptr_t actor, uint16_t* record);
CmdExecHookFn oCmdExec = nullptr;
std::atomic<uintptr_t>* g_noMeleePtr = nullptr;
uintptr_t __fastcall hkCmdExec(uintptr_t actor, uint16_t* record) {
    uintptr_t blocked = g_noMeleePtr ? g_noMeleePtr->load() : 0;
    if (blocked && actor == blocked && record && *record == 1) { *record = 0; return 0; }
    if (record && (*record == 20 || *record == 21 || *record == 22 || *record == 23 || (*record >= 242 && *record <= 244))) {
        game::g_itemCmdActor.store(actor); game::g_itemCmdCount.fetch_add(1);
        // Diagnostics: an item command as KH2 builds it (target fields), with what its handles resolve to.
        const uint8_t* r = reinterpret_cast<const uint8_t*>(record);
        char hex[3 * 0x20 + 1] = {}; for (int i = 0; i < 0x20; ++i) snprintf(hex + i * 3, 4, "%02X ", r[i]);
        uint32_t h8 = *reinterpret_cast<const uint32_t*>(r + 8); uintptr_t o8 = 0; game::ResolveHandle(h8, &o8);
        Log("KH2 item command on actor %p (its handle %08X): %s| +8 -> %p (+8 of that: %08X)", (void*)actor, Rd<uint32_t>(actor), hex, (void*)o8, o8 ? Rd<uint32_t>(o8 + 8) : 0u);
    }
    return oCmdExec(actor, record);
}

uintptr_t __fastcall hkCollect(void* input) {
    uintptr_t r = oCollect(input);
    if (g_inputLocked.load() && NowSec() - g_lockStamp.load() < STAGE_LAPSE_SEC) {
        __try {
            auto* raw = reinterpret_cast<uint8_t*>(input) + 0x18; // raw slot 0, the one that drives the player
            *reinterpret_cast<uint16_t*>(raw) = g_press.load();
            raw[2] = raw[3] = raw[4] = raw[5] = 0x80;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return r;
}

bool Hook(uintptr_t rva, const uint8_t* sig, void* fn, void** orig, const char* name) {
    bool ok = CheckSig(rva, sig) && MH_CreateHook((void*)G(rva), fn, orig) == MH_OK && MH_EnableHook((void*)G(rva)) == MH_OK;
    Log("%s hook: %s", name, ok ? "installed" : "FAILED");
    return ok;
}
}

namespace game {
bool SetFacing(uintptr_t actor, float yaw) {
    if (!CheckSig(RVA_SetFacing, SIG_SetFacing)) return false;
    __try { reinterpret_cast<SetRotFn>(G(RVA_SetFacing))(actor, yaw); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
float Facing(uintptr_t actor) { return Rd<float>(actor + 0x68C); }

void SetBattleCamera(bool on, const float eye[3], const float look[3]) {
    AcquireSRWLockExclusive(&g_stageLock);
    g_camOn = on && eye && look;
    if (g_camOn) { memcpy(g_camEye, eye, sizeof g_camEye); memcpy(g_camLook, look, sizeof g_camLook); }
    ReleaseSRWLockExclusive(&g_stageLock);
    g_camStamp.store(NowSec());
}
void SetInputLock(bool locked, uint16_t press) { g_press.store(locked ? press : 0); g_inputLocked.store(locked); g_lockStamp.store(NowSec()); }

std::atomic<uintptr_t> g_noMelee{0};
void BlockMelee(uintptr_t actor) { g_noMeleePtr = &g_noMelee; g_noMelee.store(actor); }

bool InstallStageHooks() {
    bool a = Hook(RVA_CameraApply, SIG_CameraApply, (void*)hkCamApply, (void**)&oCamApply, "battle camera");
    bool b = Hook(RVA_InputCollector, SIG_InputCollector, (void*)hkCollect, (void**)&oCollect, "input lock");
    bool c = Hook(RVA_CommandExec, SIG_CommandExec, (void*)hkCmdExec, (void**)&oCmdExec, "command filter");
    return a && b && c;
}
}

namespace {
using CmdExecFn = uintptr_t(__fastcall*)(uintptr_t actor, void* record);
using CmdLookupFn = uintptr_t(__fastcall*)(int id);
}
namespace game {
bool RunCommand(uintptr_t actor, uint16_t cmdId, uint32_t targetHandle, uint16_t arg) {
    // Once hooked, the executor's first bytes are MinHook's jump: call through the trampoline.
    CmdExecFn exec = oCmdExec ? reinterpret_cast<CmdExecFn>(oCmdExec) : nullptr;
    if (!exec) { if (!CheckSig(RVA_CommandExec, SIG_CommandExec)) return false; exec = reinterpret_cast<CmdExecFn>(G(RVA_CommandExec)); }
    // The record FIELD_COMMAND builds: id, flags, then the target block that lands at actor +0xBF8.
    alignas(16) uint8_t rec[0x20] = {};
    memcpy(rec, &cmdId, 2);
    memcpy(rec + 2, &arg, 2);
    memcpy(rec + 8, &targetHandle, 4);
    int32_t none = -1; memcpy(rec + 0x14, &none, 4);
    __try { exec(actor, rec); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int CommandMpCost(uint16_t cmdId) {
    if (!CheckSig(RVA_CommandLookup, SIG_CommandLookup)) return -1;
    uintptr_t e = 0;
    __try { e = reinterpret_cast<CmdLookupFn>(G(RVA_CommandLookup))(cmdId); } __except (EXCEPTION_EXECUTE_HANDLER) { e = 0; }
    if (!e || Rd<uint16_t>(e) != cmdId) return -1;
    return Rd<uint8_t>(e + 28);
}
}

// ---- research: find a byte that follows KH2's command-menu cursor ----
namespace game {
namespace {
struct ScanRegion { uintptr_t base; size_t size; std::vector<uint8_t> v; };
std::vector<ScanRegion> g_scan;
std::vector<uintptr_t> g_scanHits;
int g_scanStep = -1;
}
// Step 0 snapshots the module's writable memory (cursor on row 0); steps 1, 2... keep only the bytes that read
// the step number now and read the previous step's number before.
void CursorScan(int step) {
    if (step == 0) {
        g_scan.clear(); g_scanHits.clear();
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(g_base + dos->e_lfanew);
        auto* sec = IMAGE_FIRST_SECTION(nt);
        for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
            if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
            ScanRegion r{g_base + sec->VirtualAddress, sec->Misc.VirtualSize, {}};
            r.v.resize(r.size);
            if (RdBytes(r.base, r.v.data(), r.size)) g_scan.push_back(std::move(r));
        }
        for (auto& r : g_scan) for (size_t k = 0; k < r.size; ++k) if (r.v[k] == 0) g_scanHits.push_back(r.base + k);
        g_scanStep = 0;
        Log("research: cursor scan, %zu writable regions, %zu bytes at 0", g_scan.size(), g_scanHits.size());
        return;
    }
    if (g_scanStep != step - 1) return;
    std::vector<uintptr_t> keep;
    for (auto a : g_scanHits) { uint8_t b = 0; if (RdBytes(a, &b, 1) && b == step) keep.push_back(a); }
    g_scanHits.swap(keep);
    g_scanStep = step;
    std::string out; int n = 0;
    for (auto a : g_scanHits) { if (n++ >= 30) break; char t[24]; snprintf(t, sizeof t, " +0x%llX", (unsigned long long)(a - g_base)); out += t; }
    Log("research: cursor scan step %d: %zu bytes left:%s", step, g_scanHits.size(), out.c_str());
}
}
