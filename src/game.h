#pragma once
#include <atomic>
#include "common.h"

struct Location { uint8_t world, room; uint16_t door, map, btl, evt; };

struct ActorInfo {
    uintptr_t actor = 0, objentry = 0, status = 0;
    uint32_t objectId = 0, handle = 0;    // handle: actor +0, what KH2 uses to name a target
    uint8_t type = 0xFF;        // 0 player, 1 party, 3 boss, 4 mob
    char name[33] = {};
    int hp = 0, maxHp = 0;
    float x = 0, y = 0, z = 0;
    uint32_t anim = 0;
};

namespace game {
bool VerifyExe();                         // byte-checks every hooked/called function from sheets
Location Now();
bool SafeGameplay();                      // room live, no cutscene/event/menu/pause
bool InField();
bool CutsceneOrEvent();
bool MenuOpen();
int RoxasHp(); int RoxasMaxHp();
int SoraMp(); int SoraMaxMp();            // unit slot 0 +0x180/+0x184
int SaveByte(uint32_t saveOffset);        // magic levels, item counts
void SetSaveByte(uint32_t saveOffset, int v);
void CursorScan(int step); // research: find KH2's command-menu cursor byte
// KH2's item commands as they run (any actor): who ran the last one, and how many have run.
extern std::atomic<uintptr_t> g_itemCmdActor; extern std::atomic<int> g_itemCmdCount;
uint32_t ResolveHandle(uint32_t h, uintptr_t* out);
void EnumActors(std::vector<ActorInfo>& out); // whole active list, classified
bool ReadActor(uintptr_t actor, ActorInfo& a);
bool IsDying(uintptr_t actor);

// Per-actor freeze: actor+0x120 bit 19 makes KH2's per-entity update skip the actor (0x3BAA80).
void SetFrozen(uintptr_t actor, bool frozen);
bool IsFrozen(uintptr_t actor);
// Keeps one actor updating through every freeze (a knocked-out partner playing his fall).
void SetUnfreezable(uintptr_t actor, bool on);
void PreLoop();
bool InstallFreezeHook();  // needs MH_Initialize first  // call on the game thread right before KH2's entity loop

bool RequestTransition(const Location& to);   // native 0x152990
int TakeDamage(uintptr_t actor, int delta, bool react); // native 0x3D5E50 (negative = damage)
bool SetPos(uintptr_t actor, float x, float y, float z);
int InventoryCount(uint32_t saveOffset);
void SetInventoryCount(uint32_t saveOffset, int n);
bool WorldToScreen(float x, float y, float z, float sw, float sh, float& outX, float& outY);

// FFX staging. KH2 Y points down: "up" is -Y.
bool SetFacing(uintptr_t actor, float yaw);   // native 0x3B6560; yaw = atan2(dx, dz)
float Facing(uintptr_t actor);                // +0x68C
// While on, KH2's camera eases to this eye/look-at instead of following the player.
void SetBattleCamera(bool on, const float eye[3] = nullptr, const float look[3] = nullptr);
// While locked, KH2 sees a neutral pad (no walking, camera or KH2 menus); `press` is a PS2 button mask
// fed to it instead (0x4000 = Cross, KH2's attack).
void SetInputLock(bool locked, uint16_t press = 0);
bool InstallStageHooks();                     // camera + input hooks; needs MH_Initialize first
// KH2 commands (magic, items): runs the command on `actor` through KH2's own executor, aimed at
// `targetHandle` (0 = none). Returns false if it faulted.
bool RunCommand(uintptr_t actor, uint16_t cmdId, uint32_t targetHandle, uint16_t arg = 0); // arg: record +2 (item id for items)
int CommandMpCost(uint16_t cmdId);            // command table +28, -1 if unknown
// While set, KH2's command executor refuses the plain Attack command (id 1) for this actor, so a partner
// whose turn is a spell or technique goes straight to it instead of swinging first.
void BlockMelee(uintptr_t actor);
}
