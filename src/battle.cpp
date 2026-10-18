#include "battle.h"
#include "game.h"
#include "testmode.h"
#include "gen/sheets.h"
#include <algorithm>
#include <random>
#include <cmath>
#include <climits>

using namespace sheets;

namespace {

struct Unit {
    uintptr_t actor = 0, objentry = 0, status = 0;
    uint32_t objectId = 0, handle = 0;
    Side side = Side::Enemy;
    int sheet = -1;               // index into sheets::units
    std::string name;
    int ctb = 0;
    bool defending = false;
    int hp = 0, maxHp = 0, lastHp = -1;
    bool down = false;            // a partner knocked out: stays down until a Cure or a party item revives him
    bool seenHome = false;        // put into its place in the line once (enemies joining after the staging)
    uint32_t downMotion = 0xFFFFFFFF;
    double downAt = -1;           // knocked out at (he plays KH2's downed loop until revived)
    float x = 0, y = 0, z = 0;
    bool seen = false;
    bool settle = false;          // free to react during the settle window
    uint8_t type = 4;             // objentry type: 0 player, 1 partner, 3 boss, 4 mob
    std::string model;            // objentry +8 (partners' moves are listed by model)
    bool held = false;            // we set +0x6C8 bit 7 (counted in the frozen player group)
    // Staging: every battler has its own place in the party line or the enemy line.
    bool homed = false, fixed = false; // fixed: bosses stay where KH2 put them
    float hx = 0, hy = 0, hz = 0, hyaw = 0;
    float fx = 0, fy = 0, fz = 0;      // where the current step started
    bool party() const { return side != Side::Enemy; }
    bool ko() const { return party() && (hp <= 0 || down); }
};

std::mutex g_lock;                 // guards everything below that the UI reads
std::vector<Unit> g_units;
Phase g_phase = Phase::Field;
double g_phaseStart = 0;
int g_current = -1;                // index into g_units
bool g_active = false, g_encounter = false, g_boss = false;
std::string g_banner, g_info, g_action; double g_infoTime = 0;
double g_shatterStart = -1;
int g_wins = 0;
std::vector<BattleCommand> g_cmds;

// Encounter bookkeeping
Location g_returnLoc{}; float g_returnPos[3]{}; bool g_returnPosValid = false;
int g_arena = -1;                  // index into battleMaps: where this encounter is fought
double g_foesSince = -1, g_revealAt = -1, g_waveAt = -1;
int g_wave = 1;
constexpr int kMaps = (int)(sizeof(battleMaps) / sizeof(battleMaps[0]));
// The battle spot of this room (an encounter is fought in the room the party is in), or -1.
int SpotForRoom(int world, int room) {
    for (int i = 0; i < kMaps; ++i) if (battleMaps[i].world == world && battleMaps[i].room == room) return i;
    return -1;
}
// Test mode only, from a room without enemies: this world's rooms with a battle spot, nearest first, in turn
// (a room whose enemies were just beaten doesn't bring them back straight away).
constexpr int kTestWorld = 7, kTestRoom = 0;   // test runs fight in Agrabah (al00); 0 = the save's own world
int NearestSpot(int world, int room) {
    std::vector<int> mine;
    for (int i = 0; i < kMaps; ++i) if (battleMaps[i].world == world) mine.push_back(i);
    if (mine.empty()) return -1;
    std::sort(mine.begin(), mine.end(), [&](int x, int y) { return std::abs((int)battleMaps[x].room - room) < std::abs((int)battleMaps[y].room - room); });
    static int turn = 0;
    return mine[turn++ % std::min<int>((int)mine.size(), 4)];
}
bool g_spotHere = false;           // this battle is fought at the current room's battle spot (no room change)
double g_meter = 0, g_meterGoal = 3000, g_lastBattleEnd = -100, g_noFoesSince = -1;
float g_lastPos[3]{}; bool g_lastPosValid = false;
bool g_loadLeft = false;

// Turn bookkeeping
std::mt19937 g_rng{12345};
double g_fieldSince = -1;
int g_partyTurns = 0, g_enemyTurns = 0;
bool g_victoryCounted = false;
std::string g_dbg;

// The action being carried out.
struct Act {
    CmdKind kind = CmdKind::Attack;
    int target = -1;              // unit index
    int sheetIndex = -1;          // magic or item row
    uint16_t cmd = 0;
    int step = 0; double stepStart = 0;
    float window = 2.f;
    double hitTime = -1;
    long long enemyHpMark = 0, partyHpMark = 0;
    int mpMark = 0, itemMark = 0;
    int partnerMp = -1;           // the acting partner's MP when his move began
    int itemCmdMark = -1;         // KH2's item command count when the item turn began
    int itemOn = -1;              // the unit a single-target item is used on
    bool cursorDone = false;      // KH2's menu cursor read for this act
    int slotBlock = -1, slotCount = 0; uint8_t slotSave[16] = {};  // a partner's item slots, while they hold only the chosen item
    bool backedOut = false;
    bool hit = false, ran = false;
    uint16_t lastRec = 0; uint32_t lastMotion = 0xFFFFFFFF; // partner diagnostics
    bool healDirect = false;      // a partner's Cure (his AI, with Sora's HP reading low)
    bool healFallback = false;
    int fakeHp = -1;              // Sora's real HP while it reads low for the partner's AI
    int selfHp = -1;              // the acting partner's real HP while it reads low for his AI
    std::vector<std::pair<uintptr_t, int>> hpBefore; // items: each party member's HP when the item was chosen
} g_act;
constexpr int kMoves = (int)(sizeof(partnerMoves) / sizeof(partnerMoves[0]));
bool MoveFor(int m, const std::string& model) { return !strncmp(model.c_str(), partnerMoves[m].partnerModel, strlen(partnerMoves[m].partnerModel)); }
// A partner's moves are its equipped abilities (its record in the save: 80 u16 slots, bit 0x8000 = equipped)
// and its KH2 AI picks among them. For its turn the mod leaves equipped only the move the player chose
// (none for Attack: plain melee), then puts the list back exactly as it was.
struct AbilityBackup { uint32_t list = 0; uint16_t v[80]{}; bool active = false; } g_abil;
uint32_t AbilityListFor(const std::string& model) {
    for (int i = 0; i < kMoves; ++i) if (MoveFor(i, model)) return partnerMoves[i].abilityList;
    return 0;
}
// 0 not learned, 1 learned but unequipped, 2 equipped. The partner AI only knows the abilities that were
// equipped when the room loaded, so only equipped ones can be chosen in battle.
int AbilityState(uint32_t list, uint16_t id) {
    if (g_abil.active && g_abil.list == list) {
        for (int k = 0; k < 80; ++k) if ((g_abil.v[k] & 0x7FFF) == id) return g_abil.v[k] & 0x8000 ? 2 : 1;
        return 0;
    }
    for (int k = 0; k < 80; ++k) {
        uint16_t v = (uint16_t)(game::SaveByte(list + k * 2) | (game::SaveByte(list + k * 2 + 1) << 8));
        if ((v & 0x7FFF) == id) return v & 0x8000 ? 2 : 1;
    }
    return 0;
}
bool HasAbility(uint32_t list, uint16_t id) { return AbilityState(list, id) == 2; }
void RestoreAbilities() {
    game::BlockMelee(0);
    if (!g_abil.active) return;
    for (int k = 0; k < 80; ++k) Wr<uint16_t>(G(RVA_SaveBase) + g_abil.list + k * 2, g_abil.v[k]);
    g_abil.active = false;
}
// keep: the one move left equipped (0 = none, plain melee).
void GateAbilities(const std::string& model, uint16_t keep) {
    RestoreAbilities();
    uint32_t list = AbilityListFor(model);
    if (!list) return;
    g_abil.list = list; g_abil.active = true;
    uintptr_t base = G(RVA_SaveBase) + list;
    for (int k = 0; k < 80; ++k) g_abil.v[k] = Rd<uint16_t>(base + k * 2);
    for (int k = 0; k < 80; ++k) {
        uint16_t v = g_abil.v[k], id = v & 0x7FFF;
        bool move = false;
        for (int i = 0; i < kMoves; ++i) if (MoveFor(i, model) && partnerMoves[i].abilityId == id) move = true;
        if (!move) continue;
        uint16_t nv = id == keep ? (uint16_t)(id | 0x8000) : id;
        if (nv != v) Wr<uint16_t>(base + k * 2, nv);
    }
}

// A world's enemies: taken out of the field into the room's encounter pool (frozen, far below the floor)
// until an encounter calls some of them.
struct PoolMob { uintptr_t actor, objentry, status; uint32_t objectId; float x, y, z; bool held; };
std::vector<PoolMob> g_pool;
bool g_testArena = false;          // the encounter took the party to a battle map: go back after
bool InPool(uintptr_t actor) { for (auto& p : g_pool) if (p.actor == actor) return true; return false; }
uint32_t BattleType() { return Rd<uint32_t>(G(RVA_BattleStatus)); } // 0 field, 1 battle, 2 forced (story/boss)

// Staging
bool g_staged = false;
bool g_probing = false;            // test mode is pressing KH2's own menu buttons
float g_camScale = 1.f;            // > 1 when more than 3 enemies line up: the camera pulls back to show them all
float g_dirX = 0, g_dirZ = 1, g_partyX = 0, g_partyY = 0, g_partyZ = 0, g_enemyDist = 0;
uint16_t g_press = 0;
bool g_menuSeen = false; double g_lastTick = 0;

int TickSpeed(int agi) {
    for (auto& r : ctbTickTable) if (agi >= r.agiMin && agi <= r.agiMax) return r.tick;
    return 16;
}
int UnitAgility(const Unit& u) { return u.sheet >= 0 ? units[u.sheet].agility : 10; }
int UnitTick(const Unit& u) { return std::max(1, TickSpeed(UnitAgility(u))); }
constexpr int kUnits = (int)(sizeof(units) / sizeof(units[0]));
constexpr int kMagic = (int)(sizeof(magic) / sizeof(magic[0]));
constexpr int kItems = (int)(sizeof(items) / sizeof(items[0]));
int SheetForEnemy(uint32_t id) { for (int i = 0; i < kUnits; ++i) if (units[i].enemy && units[i].objectId == id) return i; return -1; }
int SheetForPartner(const char* model) { for (int i = 0; i < kUnits; ++i) if (units[i].matchModel && !strncmp(model, units[i].kh2Model, strlen(units[i].kh2Model))) return i; return -1; }
int SheetForSora() { for (int i = 0; i < kUnits; ++i) if (units[i].matchPlayer) return i; return -1; }
bool Excluded(uint32_t id) { for (auto x : excludedObjectIds) if (x == id) return true; return false; }
bool IsFoe(const ActorInfo& a) {
    if (a.type != 3 && a.type != 4) return false;
    if (a.name[0] == 'F' && a.name[1] == '_') return false;
    if (a.name[0] == 'N' && a.name[1] == '_') return false;
    if (Excluded(a.objectId)) return false;
    if (!strncmp(a.name, "M_EX350", 7)) return false;   // the Mushroom XIII mushrooms: never a random encounter
    if (!a.status || a.maxHp <= 0 || a.hp <= 0) return false;
    if (game::IsDying(a.actor)) return false;
    return true;
}
bool IsBattleFoe(const ActorInfo& a) { return IsFoe(a) && !InPool(a.actor); }
bool IsPartner(const ActorInfo& a) { return a.type == 1 && a.status && a.maxHp > 0; }
int FindFieldRoom(const Location& l) {
    for (int i = 0; i < (int)(sizeof(fieldRooms) / sizeof(fieldRooms[0])); ++i)
        if (fieldRooms[i].world == l.world && fieldRooms[i].room == l.room && fieldRooms[i].field) return i;
    return -1;
}
void SetPhase(Phase p) { g_phase = p; g_phaseStart = NowSec(); }
void Info(const std::string& s) { g_info = s; g_infoTime = NowSec(); }

std::wstring ProgressPath() { return DataDir() + L"\\progress.ini"; }
void LoadProgress() { g_wins = GetPrivateProfileIntW(L"progress", L"wins", 0, ProgressPath().c_str()); }
void SaveProgress() { WritePrivateProfileStringW(L"progress", L"wins", std::to_wstring(g_wins).c_str(), ProgressPath().c_str()); }

int SoraIndex() { for (int i = 0; i < (int)g_units.size(); ++i) if (g_units[i].side == Side::Sora) return i; return -1; }
int EnemyCount() { int n = 0; for (auto& u : g_units) if (!u.party()) ++n; return n; }
long long EnemyHpSum() { long long s = 0; for (auto& u : g_units) if (!u.party()) s += u.hp; return s; }
long long PartyHpSum() { long long s = 0; for (auto& u : g_units) if (u.party()) s += std::max(0, u.hp); return s; }
// A character's KH2 item slots (the items equipped on him), in the order KH2's Items menu lists them (filled
// slots only). The save keeps one 0x114-byte block per character: Sora 0x24F0, Donald 0x2604, Goofy 0x2718;
// +0x12 is the number of item slots and +0x44 the slots' item ids (u16).
struct SlotItem { int row; int pos; };
bool IsDonald(const std::string& m); bool IsGoofy(const std::string& m);
int CharBlock(const Unit& u) {
    if (u.side == Side::Sora) return 0x24F0;
    if (IsDonald(u.model)) return 0x2604;
    if (IsGoofy(u.model)) return 0x2718;
    return -1;
}
std::vector<SlotItem> ItemSlots(int block) {
    std::vector<SlotItem> v;
    if (block < 0) return v;
    int n = std::clamp(game::SaveByte(block + 0x12), 0, 8);
    for (int k = 0; k < n; ++k) {
        int id = (game::SaveByte(block + 0x44 + k * 2) | (game::SaveByte(block + 0x44 + k * 2 + 1) << 8)) & 0xFFF;
        if (!id) continue;
        int row = -1;
        for (int i = 0; i < kItems; ++i) if ((int)(items[i].saveOffset - 0x3580 + 1) == id) row = i;
        if (row >= 0) v.push_back({row, (int)v.size()});
    }
    return v;
}
std::vector<SlotItem> SoraItemSlots() { return ItemSlots(0x24F0); }
// KH2's battle flags (0x2A11400): bit 3 makes every prize on the floor fly to the player (0x3ABD40, read by
// the prize pickup check 0x3DBEA0).
bool g_prizeMagnet = false;
void PrizeMagnet(bool on) {
    uintptr_t a = G(0x2A11400);
    uint32_t f = Rd<uint32_t>(a);
    if (on && !(f & 8)) { Wr<uint32_t>(a, f | 8); g_prizeMagnet = true; }
    else if (!on && g_prizeMagnet) { Wr<uint32_t>(a, f & ~8u); g_prizeMagnet = false; }
}
bool AnyoneHurt() { for (auto& u : g_units) if (u.party() && ((u.hp > 0 && u.hp < u.maxHp) || u.down)) return true; return false; }
// Knocked-out partners get back up only through a Cure (Sora's or Donald's) or a party item: frac of max HP.
int g_testBattles = 0;             // battles fought this run (test mode)
int g_testKo = 0;                  // test mode: 1 = Donald knocked out, 2 = he stayed down, 3 = revived
bool HealingAct();
constexpr uintptr_t kDownTimer = 0xDC0;
// A knocked-out partner: KH2 plays his fall, then he is held lying down (frozen) until revived.
void DownPose(Unit& u, bool on) {
    // KH2 plays its own downed loop (STUN_LOOP, motion 54) while the partner keeps updating; actor +0xDC0 is
    // the time left (float, ~60 a second) before KH2 gets him up by himself. Getting up: it is set near zero.
    if (on) { u.downAt = NowSec(); }
    else { if (u.downAt > 0 && u.actor) Wr<float>(u.actor + kDownTimer, 1.f); u.downAt = -1; }
}
void Revive(float frac, const char* by) {
    for (auto& u : g_units) if (u.side == Side::Partner && u.down) {
        int hp = std::max(1, (int)std::lround(u.maxHp * frac));
        u.down = false; u.hp = hp; u.lastHp = hp;
        DownPose(u, false);
        if (u.status) Wr<int32_t>(u.status, hp);
        Log("%s revived by %s (%d HP)", u.name.c_str(), by, hp);
        Info(u.name + " is back up!");
        if (testmode::Active() && g_testKo == 2) { g_testKo = 3; testmode::Pass("partner-revived"); }
    }
}
bool HealMove(int m) { return !partnerMoves[m].targetEnemy; }
int FirstEnemy() { for (int i = 0; i < (int)g_units.size(); ++i) if (!g_units[i].party()) return i; return -1; }
const Command* FindCommand(const char* kind) { for (auto& c : commands) if (!strcmp(c.kind, kind)) return &c; return nullptr; }
int CommandRank(const char* kind) { auto c = FindCommand(kind); return c ? c->ctbRank : ctbBaseRank; }
float CommandWindow(const char* kind) { auto c = FindCommand(kind); return c && c->windowSec > 0 ? c->windowSec : 2.f; }

// The spell a magic row casts at the save's level (0 = not learned).
int MagicLevel(int i) { return std::clamp(game::SaveByte(magic[i].saveOffset), 0, 3); }
uint16_t MagicCmd(int i) { int l = MagicLevel(i); return l ? magic[i].cmd[l - 1] : 0; }
// MP costs come from KH2's command table, read on the game thread and cached for the UI.
int g_magicCost[kMagic];
void RefreshMagicCosts() { for (int i = 0; i < kMagic; ++i) { uint16_t c = MagicCmd(i); g_magicCost[i] = c ? game::CommandMpCost(c) : -1; } }
int MagicCost(int i) { return g_magicCost[i]; }
bool MagicUsable(int i) {
    if (!MagicCmd(i)) return false;
    int mp = game::SoraMp(), cost = MagicCost(i);
    if (cost < 0 || cost >= 0xFF) return mp > 0; // unknown, or "all MP" spells (Cure)
    return mp >= cost;
}

// ---- freezing ----
// KH2's own group freeze (0x2A171E8): bit n holds every actor whose group is n. Groups: 0 player,
// 1 party, 3 boss, 4 mob. A battler that must stay still while its group moves is moved into group 0
// for the moment (actor +0x6C8 bit 7, read by 0x3B5DC0).
uint32_t g_groupMask = 0;
void SetGroups(uint32_t mask) {
    if (mask == g_groupMask && Rd<uint32_t>(G(RVA_FrozenGroups)) == mask) return;
    Wr<uint32_t>(G(RVA_FrozenGroups), mask);
    g_groupMask = mask;
}
void SetHold(Unit& u, bool hold) {
    // Checked every frame: KH2 can clear the flag itself (an enemy still finishing its appearance), so it is set
    // again whenever it is missing.
    uint32_t f = Rd<uint32_t>(u.actor + 0x6C8);
    if (hold && !(f & 0x80)) { Wr<uint32_t>(u.actor + 0x6C8, f | 0x80); u.held = true; }
    else if (!hold && u.held) { Wr<uint32_t>(u.actor + 0x6C8, f & ~0x80u); u.held = false; }
}
void UnfreezeAll() {
    for (auto& u : g_units) if (u.actor && u.side != Side::Sora) SetHold(u, false);
    if (g_groupMask) { if (Rd<uint32_t>(G(RVA_FrozenGroups)) == g_groupMask) Wr<uint32_t>(G(RVA_FrozenGroups), 0u); g_groupMask = 0; }
}

// Partners by model (OpenKh's object list); Donald and Goofy have a model of their own in some worlds.
bool IsDonald(const std::string& m) { return m.rfind("P_EX020", 0) == 0 || m.rfind("P_WI020", 0) == 0 || m.rfind("P_LK020", 0) == 0; }
bool IsGoofy(const std::string& m) { return m.rfind("P_EX030", 0) == 0 || m.rfind("P_WI030", 0) == 0 || m.rfind("P_LK030", 0) == 0; }
const char* PartnerName(const std::string& m) {
    static const std::pair<const char*, const char*> names[] = {
        {"P_BB000", "Beast"}, {"P_NM000", "Jack"}, {"P_LK000", "Simba"}, {"P_AL000", "Aladdin"}, {"P_MU000", "Mulan"},
        {"P_MU010", "Ping"}, {"P_HE000", "Auron"}, {"P_CA000", "Jack Sparrow"}, {"P_TR000", "Tron"}, {"P_EH000", "Riku"},
        {"P_EX220", "Mickey"}, {"P_EX200", "Mickey"}, {"P_EX110", "Roxas"}};
    if (IsDonald(m)) return "Donald";
    if (IsGoofy(m)) return "Goofy";
    for (auto& n : names) if (m.rfind(n.first, 0) == 0) return n.second;
    return nullptr;
}
void NameUnit(Unit& u, const char* model) {
    if (u.side == Side::Sora) { u.name = u.objectId == 90 ? "Roxas" : "Sora"; return; }
    if (u.side == Side::Partner) { const char* pn = PartnerName(model); u.name = u.sheet >= 0 ? units[u.sheet].name : pn ? pn : model; return; }
    std::string base = u.sheet >= 0 ? units[u.sheet].name : "";
    if (base.empty()) for (auto& e : enemyNames) if (e.objectId == u.objectId) { base = e.name; break; }
    if (base.empty()) base = model;
    int count = 0;
    for (auto& o : g_units) if (&o != &u && o.side == Side::Enemy && o.objectId == u.objectId) ++count;
    u.name = base + " " + char('A' + std::min(count, 25));
    if (u.side == Side::Enemy && u.sheet < 0) Log("enemy %s is objectId %u (%s)", u.name.c_str(), u.objectId, model);
}

void EndBattle() {
    Revive(0.25f, "the end of the battle");
    RestoreAbilities();
    PrizeMagnet(false);
    UnfreezeAll();
    g_units.clear();
    g_active = false; g_boss = false; g_current = -1; g_staged = false; g_press = 0; g_revealAt = -1;
    g_lastBattleEnd = NowSec();
    g_meter = 0;
    std::uniform_real_distribution<double> d(encounterMeterMin, encounterMeterMax);
    g_meterGoal = d(g_rng);
}

// ---- turns (FFX CTB: the lowest counter acts; everyone's counter drops by that amount) ----
// Test mode: in the 1st battle Sora and Donald each use an item equipped on them; in the 2nd Sora swaps a partner
// for the world's own one through KH2's Party command; in the 3rd Sora falls (a loss). (The knockout battle,
// Donald down until a Cure, is switched off with kTestKnockout.)
int g_testKoTurns = 0; bool g_testSoraKo = false;
constexpr bool kTestKnockout = false;   // the knockout/revive battle: well tested, left out of test runs
constexpr int kTestSwapBattle = 0, kTestSoraKoBattle = 99; // swap and loss: well tested, left out (0/99 = off)
// Partner item use, tried a different way per test battle: 0 his own AI (low HP), 1 KH2's item command run on
// him targeting himself, 2 the same targeting Sora. Play uses 0.
int g_partnerItemWay = 0;
int g_testItemSora = 0;            // Sora's item turns this battle (two: the second checks KH2's cursor came back)
bool g_testItemDonald = false, g_testItemGoofy = false, g_testHurt = false;
int g_testItemBattle = 0;          // the battle the item flags above belong to
int g_testSwap = 0; double g_testSwapAt = -1;
// KH2's Party command lists the partners by party slot: Donald's slot first, then Goofy's. A partner swapped in
// takes the slot of the one he replaced.
std::vector<std::string> g_slots = {"P_EX020", "P_EX030"};
int g_swapSlot = -1; std::string g_swapLeaving;     // a swap under way: its slot and who is leaving
bool g_kh2Menu = false;
// Heals the mod applies itself (a partner's item): shown as a glow and a rising number over the one healed.
struct HealPopup { uintptr_t actor; std::string text; double at; uint32_t color; };
std::vector<HealPopup> g_popups;
// Where KH2's command-menu cursor is (0 Attack, 1 Magic, 2 Items, 3 Drive; page 2 opens on the same row). After
// an item KH2 leaves it on Items and ignores presses for a while, so the next menu action starts from there.
int g_kh2Cursor = 0;
// KH2's command-menu cursor row, read from the game (the cursor scan found two bytes that follow it; the first is
// used, the second only logged to confirm).
constexpr uintptr_t kCursorA = 0x2581C24, kCursorB = 0x2A106BC;
int ReadKh2Cursor() { int v = Rd<uint8_t>(G(kCursorB)); return (v >= 0 && v < 4) ? v : -1; }
// Presses taking the cursor from g_kh2Cursor to row `to` (the shortest way; the menu wraps over 4 rows).
void CursorTo(std::vector<uint16_t>& seq, int to) {
    int d = ((to - g_kh2Cursor) % 4 + 4) % 4;
    if (d <= 2) for (int k = 0; k < d; ++k) seq.push_back(0x0040);
    else seq.push_back(0x0010);
}            // KH2's own command menu is being driven (the mod's windows step aside)
// Test only: make sure a character has a Potion equipped and in the inventory.
void TestEquipPotion(int block) {
    if (block < 0) return;
    std::string raw; char b[16];
    int n = game::SaveByte(block + 0x12);
    for (int k = 0; k < 8; ++k) { snprintf(b, sizeof b, " %u", game::SaveByte(block + 0x44 + k * 2) | (game::SaveByte(block + 0x45 + k * 2) << 8)); raw += b; }
    Log("test: save block 0x%X: %d item slots, ids%s", block, n, raw.c_str());
    auto slots = ItemSlots(block);
    if (slots.empty()) {
        game::SetSaveByte(block + 0x12, std::max(1, n));
        game::SetSaveByte(block + 0x44, 1); game::SetSaveByte(block + 0x45, 0);  // Potion
        Log("test: equipped a Potion on save block 0x%X", block);
        slots = ItemSlots(block);
    }
    // The first equipped item: some in the inventory to use.
    if (!slots.empty() && game::InventoryCount(items[slots[0].row].saveOffset) < 2) game::SetInventoryCount(items[slots[0].row].saveOffset, 2);
}
// The world's own partner's party command (KH2's command list: Auron 58, Ping 59, Mulan 60, Sparrow 62,
// Beast 63, Jack 64, Simba 65, Tron 66), and the second form of each (94-102).
uint16_t WorldPartnerCmd(int world, bool second) {
    int c = world == 6 ? 58 : world == 8 ? 60 : world == 16 ? 62 : world == 5 ? 63 : world == 14 ? 64 : world == 10 ? 65 : world == 17 ? 66 : 0;
    return (uint16_t)(c && second ? c + 36 : c);
}
void TestScript(int idx) {
    if (!testmode::Active()) return;
    int donald = -1;
    for (int i = 0; i < (int)g_units.size(); ++i) if (g_units[i].side == Side::Partner && IsDonald(g_units[i].model)) donald = i;
    if (kTestKnockout && g_testBattles == 2 && g_testKo == 0 && donald >= 0 && g_units[donald].hp > 0 && g_partyTurns + g_enemyTurns > 0) {
        Unit& d = g_units[donald];
        int after = game::TakeDamage(d.actor, -d.hp, true);
        d.hp = std::max(0, after);
        g_testKo = 1; g_testKoTurns = 0;
        Log("test: knocked Donald out (hp %d)", after);
    } else if (g_testKo == 1 && donald >= 0) {
        if (!g_units[donald].down) Log("test: Donald isn't down (hp %d)", g_units[donald].hp);
        else if (++g_testKoTurns >= 3) { g_testKo = 2; testmode::Pass("partner-stays-ko"); Log("test: Donald stayed down for 3 turns"); }
    }
    if (g_testItemBattle != g_testBattles) {
        g_testItemBattle = g_testBattles;
        g_testItemSora = 0; g_testItemDonald = g_testItemGoofy = g_testHurt = false;
        g_partnerItemWay = 0; // ways 1 and 2 (the item command run on a partner) fault: not tried any more
        Log("test: battle %d, partners use items by way %d", g_testBattles, g_partnerItemWay);
    }
    if (!g_testHurt) {
        // Battle 1: Sora and Donald are hurt a little, so their items have something to do.
        g_testHurt = true;
        for (auto& u : g_units) if (u.party() && u.hp > 1) game::TakeDamage(u.actor, -std::max(1, u.hp / 3), false);
        int si = SoraIndex();
        if (si >= 0) {
            // Sora: his first slot as it is (a party item), his second a Potion (a single-target item).
            int b = CharBlock(g_units[si]);
            TestEquipPotion(b);
            game::SetSaveByte(b + 0x12, std::max(2, game::SaveByte(b + 0x12)));
            game::SetSaveByte(b + 0x46, 1); game::SetSaveByte(b + 0x47, 0);
        }
        for (auto& u : g_units) if (u.side == Side::Partner && CharBlock(u) >= 0) {
            // Donald's and Goofy's first slot holds a Potion for the test (KH2's partners may not use every item).
            int b = CharBlock(u);
            TestEquipPotion(b);
            game::SetSaveByte(b + 0x44, 1); game::SetSaveByte(b + 0x45, 0);
            if (game::InventoryCount(items[0].saveOffset) < 4) game::SetInventoryCount(items[0].saveOffset, 4);
        }
        if (game::InventoryCount(items[0].saveOffset) < 3) game::SetInventoryCount(items[0].saveOffset, 3);
        Log("test: party hurt a little; Potions equipped where none were");
    }
    if (g_testBattles >= kTestSoraKoBattle && !g_testSoraKo && !g_units[idx].party()) {
        int si = SoraIndex();
        if (si >= 0) {
            g_testSoraKo = true;
            int after = game::TakeDamage(g_units[si].actor, -g_units[si].hp, true);
            Log("test: Sora brought to 0 HP (hp %d)", after);
        }
    }
}
void StartTurn(int idx) {
    TestScript(idx);
    g_current = idx;
    Unit& u = g_units[idx];
    g_banner = u.name;
    g_action.clear();
    if (u.party()) { u.defending = false; ++g_partyTurns; }
    else ++g_enemyTurns;
    SetPhase(Phase::TurnIntro);
}
void NextTurn() {
    int best = -1;
    for (int i = 0; i < (int)g_units.size(); ++i)
        if (!g_units[i].ko() && (best < 0 || g_units[i].ctb < g_units[best].ctb)) best = i;
    if (best < 0) return;
    int dec = g_units[best].ctb;
    for (auto& u : g_units) if (!u.ko()) u.ctb -= dec;
    StartTurn(best);
}
void Spend(Unit& u, int rank) { u.ctb = UnitTick(u) * std::max(1, rank); }
void Preview(std::vector<int>& idx) {
    idx.clear();
    if (g_units.empty()) return;
    const int skip = INT_MAX / 2;
    std::vector<int> c(g_units.size());
    for (size_t i = 0; i < g_units.size(); ++i) c[i] = g_units[i].ko() ? skip : g_units[i].ctb;
    int cur = g_current;
    if (cur >= 0 && cur < (int)g_units.size()) { idx.push_back(cur); c[cur] = UnitTick(g_units[cur]) * ctbBaseRank; }
    while ((int)idx.size() < ctbPreviewCount) {
        int b = 0;
        for (size_t i = 1; i < c.size(); ++i) if (c[i] < c[b]) b = (int)i;
        if (c[b] >= skip) break;
        int dec = c[b];
        for (auto& v : c) if (v < skip) v -= dec;
        idx.push_back(b);
        c[b] = UnitTick(g_units[b]) * ctbBaseRank;
    }
}

// ---- staging ----
float YawTo(float dx, float dz) { return std::atan2(dx, dz); }
float Smooth(float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3 - 2 * t); }
void Face(Unit& u, float yaw) { if (!u.fixed) game::SetFacing(u.actor, yaw); }
void StepFrom(Unit& u) { u.fx = u.x; u.fy = u.y; u.fz = u.z; }
void StepTo(Unit& u, float tx, float ty, float tz, float t) {
    if (u.fixed) return;
    float k = Smooth(t);
    float x = u.fx + (tx - u.fx) * k, y = u.fy + (ty - u.fy) * k, z = u.fz + (tz - u.fz) * k;
    game::SetPos(u.actor, x, y, z);
    u.x = x; u.y = y; u.z = z;
}
void PinHome(Unit& u) {
    if (u.fixed || !u.homed) return;
    game::SetPos(u.actor, u.hx, u.hy, u.hz);
    u.x = u.hx; u.y = u.hy; u.z = u.hz;
}
// Step `u` toward `t` until `reach` away from it, facing it.
void StepToward(Unit& u, const Unit& t, float reach, float k) {
    float dx = t.x - u.fx, dz = t.z - u.fz, d = std::max(1.f, std::sqrt(dx * dx + dz * dz));
    float r = std::min(reach, d);
    StepTo(u, t.x - dx / d * r, u.fy, t.z - dz / d * r, k);
    if (!u.fixed) game::SetFacing(u.actor, YawTo(dx, dz));
}

// Both lines run side to side across the axis from the party to the enemies, as in FFX.
void LayOutLines() {
    float rx = g_dirZ, rz = -g_dirX;
    auto across = [&](Unit* a) { return a->x * rx + a->z * rz; };
    auto line = [&](std::vector<Unit*>& v, float cx, float cz, float spacing, float yaw) {
        int n = (int)v.size();
        for (int i = 0; i < n; ++i) {
            float off = (i - (n - 1) * 0.5f) * spacing;
            v[i]->hx = cx + rx * off; v[i]->hz = cz + rz * off; v[i]->hy = g_partyY; v[i]->hyaw = yaw; v[i]->homed = true;
        }
    };
    // Party: Sora in the middle, partners either side. Enemies keep their left-to-right order.
    std::vector<Unit*> party, foes;
    Unit* sora = nullptr;
    for (auto& u : g_units) { if (u.side == Side::Sora) sora = &u; else if (u.side == Side::Partner) party.push_back(&u); else if (!u.fixed) foes.push_back(&u); }
    std::sort(party.begin(), party.end(), [&](Unit* a, Unit* b) { return across(a) < across(b); });
    std::sort(foes.begin(), foes.end(), [&](Unit* a, Unit* b) { return across(a) < across(b); });
    if (sora) party.insert(party.begin() + party.size() / 2, sora);
    line(party, g_partyX, g_partyZ, formationPartySpacing, YawTo(g_dirX, g_dirZ));
    line(foes, g_partyX + g_dirX * g_enemyDist, g_partyZ + g_dirZ * g_enemyDist, formationEnemySpacing, YawTo(-g_dirX, -g_dirZ));
    for (auto& u : g_units) if (u.fixed && !u.homed) { u.hx = u.x; u.hy = u.y; u.hz = u.z; u.hyaw = game::Facing(u.actor); u.homed = true; }
}

void Stage() {
    for (auto& u : g_units) u.seenHome = true;
    int si = SoraIndex();
    if (si < 0) return;
    Unit& me = g_units[si];
    float ex = 0, ez = 0; int n = 0;
    for (auto& u : g_units) if (!u.party()) { ex += u.x; ez += u.z; ++n; }
    if (!n) return;
    ex /= n; ez /= n;
    float dx = ex - me.x, dz = ez - me.z, d = std::sqrt(dx * dx + dz * dz);
    if (d < 1.f) { float yaw = game::Facing(me.actor); dx = std::sin(yaw); dz = std::cos(yaw); d = 1.f; }
    else { dx /= d; dz /= d; }
    g_dirX = dx; g_dirZ = dz;
    g_partyX = me.x; g_partyY = me.y; g_partyZ = me.z;
    g_enemyDist = std::clamp(d, formationGap * 0.6f, formationGap);
    if ((g_testArena || g_spotHere) && g_arena >= 0) {
        // A battle spot: checked for room (tools/battlemaps.py), the lines go exactly there.
        const BattleMap& bm = battleMaps[g_arena];
        g_dirX = bm.dirX; g_dirZ = bm.dirZ; g_enemyDist = encounterDistance;
        g_partyX = bm.spotX - bm.dirX * encounterDistance * 0.5f; g_partyY = bm.spotY; g_partyZ = bm.spotZ - bm.dirZ * encounterDistance * 0.5f;
        dx = g_dirX; dz = g_dirZ;
    }
    for (auto& u : g_units) u.homed = false;
    LayOutLines();
    for (auto& u : g_units) { PinHome(u); Face(u, u.hyaw); }
    g_staged = true;
    int partners = 0; for (auto& u : g_units) if (u.side == Side::Partner) ++partners;
    Log("staged: Sora + %d partners at (%.0f,%.0f,%.0f) facing (%.2f,%.2f), %d enemies %.0f away%s", partners, g_partyX, g_partyY, g_partyZ, dx, dz, n, g_enemyDist,
        (g_testArena || g_spotHere) && g_arena >= 0 ? (std::string(" on battle spot ") + battleMaps[g_arena].key).c_str() : "");
    testmode::Pass("formation");
    for (auto& u : g_units) if (u.side == Side::Partner) {
        std::string moves;
        for (int i = 0; i < kMoves; ++i) if (MoveFor(i, u.model)) {
            uint32_t list = partnerMoves[i].abilityList; int state = 0;
            for (int k = 0; k < 80; ++k) { uint16_t v = (uint16_t)(game::SaveByte(list + k * 2) | (game::SaveByte(list + k * 2 + 1) << 8)); if ((v & 0x7FFF) == partnerMoves[i].abilityId) state = v & 0x8000 ? 2 : 1; }
            moves += std::string(partnerMoves[i].name) + (state == 2 ? "(on) " : state == 1 ? "(off) " : "(-) ");
        }
        Log("%s's moves: %s", u.name.c_str(), moves.c_str());
    }
}

// Camera behind the party, a little to the side, looking across at the enemies; input belongs to the menu.
void UpdateStage() {
    bool cutscene = game::CutsceneOrEvent();
    bool battlePhase = g_active && g_staged && g_phase != Phase::GameOver && g_phase != Phase::Returning;
    if (battlePhase && !cutscene) {
        float rx = g_dirZ, rz = -g_dirX;
        int foes = EnemyCount();
        float want = foes > 3 ? 1.f + 0.3f * (foes - 3) : 1.f;
        g_camScale += (want - g_camScale) * 0.05f; // ease into it
        float back = cameraBack * g_camScale, high = cameraHeight * (1.f + (g_camScale - 1.f) * 0.6f);
        float eye[3] = {g_partyX - g_dirX * back + rx * cameraSide, g_partyY - high, g_partyZ - g_dirZ * back + rz * cameraSide};
        float look[3] = {g_partyX + g_dirX * g_enemyDist * cameraLookAhead, g_partyY - cameraLookHeight, g_partyZ + g_dirZ * g_enemyDist * cameraLookAhead};
        game::SetBattleCamera(true, eye, look);
    } else game::SetBattleCamera(false);
    // KH2's own menu is the player's while the Party command has it open.
    bool menuTime = g_phase == Phase::PartyMenu && game::MenuOpen();
    bool lockPhase = (g_active && g_phase != Phase::GameOver) || g_phase == Phase::Shatter || g_phase == Phase::LoadingArena || g_phase == Phase::AwaitFoes;
    game::SetInputLock((lockPhase && !cutscene && !menuTime) || g_probing, g_press);
}

// Keep exactly the battlers that may move this instant unfrozen.
void ApplyFreezes() {
    const uint32_t PL = 1u << groupPlayer, PA = 1u << groupParty, E = (1u << groupBoss) | (1u << groupMob);
    int cur = (g_current >= 0 && g_current < (int)g_units.size()) ? g_current : -1;
    Side cs = cur >= 0 ? g_units[cur].side : Side::Enemy;
    bool acting = (g_phase == Phase::Act || g_phase == Phase::EnemyAct) && g_act.step == 1;
    bool soraActing = acting && cs == Side::Sora;
    // A partner's AI won't target a held enemy, so while a partner acts the enemies' groups freeze instead.
    bool partnerActing = acting && cs == Side::Partner;
    bool off = g_phase == Phase::GameOver;
    // While Sora acts his group (0) is free, so the enemies' groups are frozen instead. Otherwise the
    // enemies' groups stay free and each living enemy that isn't acting is held in the frozen group 0:
    // a defeated enemy is let go at once and always gets to play out its death.
    uint32_t mask;
    if (off) mask = 0;
    else if (soraActing) mask = PA | E;
    else if (partnerActing) mask = PL | E;
    else mask = PL | PA;
    // The partners' group stays free for a downed partner; the other partners are held instead.
    // A downed partner keeps updating (KH2's downed loop), except while Sora acts.
    bool falling = false;
    for (auto& u : g_units) if (u.side == Side::Partner && u.down) falling = true;
    if (falling && !off && !soraActing) mask &= ~PA;
    for (int i = 0; i < (int)g_units.size(); ++i) {
        Unit& u = g_units[i];
        if (u.side == Side::Sora) continue;
        bool hold = false;
        if (!off && !soraActing) {
            // A partner's item turn: the enemies are held, so his AI has nobody to chase and uses the item.
            bool itemTurn = partnerActing && g_act.kind == CmdKind::Item;
            if (u.side == Side::Enemy) hold = (!partnerActing || itemTurn) && !(acting && i == cur);
            else if (u.down) hold = false;                            // down: free, in KH2's downed loop
            else hold = (partnerActing && i != cur) || (falling && !(partnerActing && i == cur));
        }
        SetHold(u, hold);
    }
    for (auto& p : g_pool) {
        bool hold = !off && !soraActing && !partnerActing;
        uint32_t f = Rd<uint32_t>(p.actor + 0x6C8);
        if (hold && !(f & 0x80)) { Wr<uint32_t>(p.actor + 0x6C8, f | 0x80); p.held = true; }
        else if (!hold && p.held) { Wr<uint32_t>(p.actor + 0x6C8, f & ~0x80u); p.held = false; }
    }
    (void)E;
    SetGroups(mask);
}

void RefreshUnits(const std::vector<ActorInfo>& actors) {
    for (auto& u : g_units) u.seen = false;
    bool added = false;
    for (auto& a : actors) {
        Side side;
        if (a.type == 0) side = Side::Sora;
        else if (IsPartner(a)) side = Side::Partner;
        else if (IsBattleFoe(a)) side = Side::Enemy;
        else continue;
        Unit* found = nullptr;
        for (auto& u : g_units) if (u.actor == a.actor && u.objentry == a.objentry && u.status == a.status) { found = &u; break; }
        if (!found && side == Side::Enemy && g_active && g_partyTurns + g_enemyTurns > 0 && EnemyCount() == 0 && g_phase != Phase::Victory) {
            ++g_wave; g_waveAt = NowSec();
            Log("wave %d arrives", g_wave);
        }
        bool lateArrival = g_active && g_partyTurns + g_enemyTurns > 0 && EnemyCount() > 0;
        if (!found && side == Side::Enemy && g_active && lateArrival) {
            // An enemy appearing while the fight is under way (big maps spawn more as the party passes their
            // triggers) waits out of sight like the field's enemies, instead of joining mid-turn.
            g_pool.push_back({a.actor, a.objentry, a.status, a.objectId, a.x, a.y, a.z, false});
            Log("%s (objectId %u) appeared mid-battle; kept out of it", a.name, a.objectId);
            continue;
        }
        if (!found && side == Side::Enemy && (g_testArena || g_spotHere) && g_active && (EnemyCount() >= encounterMaxFoes || g_phase == Phase::Victory)) {
            // The battle map's spot was checked for this many enemies; any more (or late spawns after the win)
            // are put out of the way like field enemies.
            g_pool.push_back({a.actor, a.objentry, a.status, a.objectId, a.x, a.y, a.z, false});
            Log("extra %s (objectId %u) kept out of this battle", a.name, a.objectId);
            continue;
        }
        if (!found && side != Side::Enemy) {
            // A scripted fight can bring its own copies of Sora, Donald and Goofy: the party is only ever one of each.
            bool twin = false;
            for (auto& u : g_units) if (u.side == side && (side == Side::Sora || u.model == a.name)) twin = true;
            if (twin) {
                static uintptr_t lastTwin = 0;
                if (lastTwin != a.actor) { lastTwin = a.actor; Log("second %s (%s) left out of the party", side == Side::Sora ? "Sora" : "partner", a.name); }
                continue;
            }
        }
        if (!found) {
            Unit u;
            u.actor = a.actor; u.objentry = a.objentry; u.status = a.status; u.objectId = a.objectId; u.handle = a.handle;
            u.side = side; u.type = a.type; u.model = a.name;
            u.sheet = side == Side::Sora ? SheetForSora() : side == Side::Partner ? SheetForPartner(a.name) : SheetForEnemy(a.objectId);
            u.x = a.x; u.y = a.y; u.z = a.z;
            u.fixed = a.type == 3;
            std::uniform_int_distribution<int> d(ctbFirstTurnMin, ctbFirstTurnMax);
            g_units.push_back(u);
            found = &g_units.back();
            found->ctb = UnitTick(*found) * d(g_rng);
            NameUnit(*found, a.name);
            if (side == Side::Enemy && found->sheet < 0) Log("unit without a sheet row: objectId=%u name=%s (default agility)", a.objectId, a.name);
            if (side == Side::Partner) Log("partner joins: %s (%s)", found->name.c_str(), a.name);
            if (side == Side::Partner && g_active && g_partyTurns + g_enemyTurns > 0) {
                if (g_swapSlot >= 0 && g_swapSlot < (int)g_slots.size()) {
                    Log("party swap: %s in for %s (slot %d), Sora's turn %d", a.name, g_swapLeaving.c_str(), g_swapSlot, g_partyTurns);
                    g_slots[g_swapSlot] = a.name; g_swapSlot = -1;
                    if (g_phase == Phase::Act && g_act.kind == CmdKind::Party) { g_act.hit = true; g_act.hitTime = NowSec(); }
                }
                testmode::Pass("partner-swap");
            }
            if (side == Side::Enemy && testmode::Active() && a.status) {
                // Test battles: tougher enemies, for longer fights.
                Wr<int32_t>(a.status + 4, (int32_t)(a.maxHp * testEnemyHpMul)); Wr<int32_t>(a.status, (int32_t)(a.hp * testEnemyHpMul));
                found->maxHp = (int)(a.maxHp * testEnemyHpMul); found->hp = (int)(a.hp * testEnemyHpMul);
            }
            if (a.type == 3) g_boss = true;
            added = true;
        }
        found->seen = true;
        found->hp = side == Side::Sora ? game::RoxasHp() : a.hp;
        found->maxHp = side == Side::Sora ? game::RoxasMaxHp() : a.maxHp;
        found->x = a.x; found->y = a.y; found->z = a.z;
        found->handle = a.handle;
    }
    for (int i = (int)g_units.size() - 1; i >= 0; --i) {
        Unit& u = g_units[i];
        if (!u.seen || (u.side == Side::Enemy && u.hp <= 0)) {
            Log("unit %s leaves the battle (%s)", u.name.c_str(), u.seen ? "defeated" : "gone");
            // Always let go of a defeated enemy that still exists, so it plays out its death right away.
            bool exists = false; for (auto& a : actors) if (a.actor == u.actor) exists = true;
            if (exists) SetHold(u, false);
            if (g_current == i) g_current = -1; else if (g_current > i) --g_current;
            if (g_act.target == i) g_act.target = -1; else if (g_act.target > i) --g_act.target;
            g_units.erase(g_units.begin() + i);
        }
    }
    if (added && g_staged) {
        LayOutLines();
        for (auto& u : g_units) {
            if (!u.party() && !u.fixed && u.homed && !u.seenHome) { PinHome(u); u.seenHome = true; Log("  %s placed in the enemy line", u.name.c_str()); }
            Face(u, u.hyaw);
        }
    }
}

void BeginBattle() {
    g_wave = 1; g_waveAt = -1;
    g_kh2Cursor = 0;
    {
        // KH2's party slots: Donald's (or whoever took it), then Goofy's (or whoever took it).
        std::vector<std::string> here; for (auto& u : g_units) if (u.side == Side::Partner) here.push_back(u.model);
        std::string d, g, other;
        for (auto& m : here) { if (IsDonald(m)) d = m; else if (IsGoofy(m)) g = m; else other = m; }
        if (!here.empty()) {
            g_slots.clear();
            g_slots.push_back(!d.empty() ? d : other);
            g_slots.push_back(!g.empty() ? g : other);
        }
    }
    if (testmode::Active()) { ++g_testBattles; Log("test: battle %d of this run", g_testBattles); }
    g_active = true; g_partyTurns = g_enemyTurns = 0; g_victoryCounted = false;
    g_noFoesSince = -1;
    for (auto& u : g_units) u.lastHp = u.hp;
    Log("battle start: %d enemies, boss=%d, encounter=%d", EnemyCount(), g_boss, g_encounter);
    testmode::Pass("battle-started");
    Stage();
    if (g_testArena || g_spotHere) { g_revealAt = NowSec() + 0.5; SetPhase(Phase::Hold); } // a battle spot: let the scene appear first
    else NextTurn();
}

void StartReturn(const char* why) {
    Log("returning to the field (%s)", why);
    Revive(0.25f, "the end of the battle");
    UnfreezeAll();
    g_units.clear(); g_current = -1; g_staged = false; g_press = 0;
    PrizeMagnet(false);
    if (g_testArena) {
        g_testArena = false;
        g_pool.clear();
        game::RequestTransition(g_returnLoc);
        g_loadLeft = false;
        SetPhase(Phase::Returning);
    } else {
        EndBattle();
        if (g_spotHere && g_returnPosValid) {
            // A battle at this room's spot: back where the party was walking, with a fade in.
            std::vector<ActorInfo> party; game::EnumActors(party);
            for (auto& a : party) if (a.type == 0 || a.type == 1)
                game::SetPos(a.actor, g_returnPos[0], g_returnPos[1], g_returnPos[2]);
            g_revealAt = NowSec();
            Log("back where the party was walking");
        }
        g_spotHere = false;
        SetPhase(Phase::Field);
    }
}

// After an action everyone walks back to their own place, then the next turn starts.
void StartRegroup() {
    for (auto& u : g_units) StepFrom(u);
    g_press = 0;
    SetPhase(Phase::Regroup);
}
void FinishAction(Unit& actor, int rank) {
    g_press = 0; g_kh2Menu = false;
    RestoreAbilities();
    Spend(actor, rank);
    for (auto& u : g_units) if (u.settle && u.party() == actor.party()) u.settle = false;
    SetPhase(Phase::Settle);
}

// ---- commands ----
void BeginAct(CmdKind kind, int target, int sheetIndex, uint16_t cmd, float window, const std::string& label) {
    Unit& me = g_units[g_current];
    g_act = Act{};
    g_act.kind = kind; g_act.target = target; g_act.sheetIndex = sheetIndex; g_act.cmd = cmd; g_act.window = window;
    g_act.stepStart = NowSec();
    g_act.enemyHpMark = EnemyHpSum(); g_act.partyHpMark = PartyHpSum();
    g_act.mpMark = game::SoraMp();
    g_action = label;
    StepFrom(me);
    for (auto& u : g_units) u.settle = false;
    SetPhase(Phase::Act);
}

int ValidTarget(int t) {
    if (t >= 0 && t < (int)g_units.size() && !g_units[t].party()) return t;
    return FirstEnemy();
}

void HandleCommand(const BattleCommand& c) {
    if (g_current < 0 || g_current >= (int)g_units.size() || !g_units[g_current].party()) return;
    Unit& me = g_units[g_current];
    bool sora = me.side == Side::Sora;
    switch (c.kind) {
    case CmdKind::Attack: {
        int t = ValidTarget(c.target); if (t < 0) return;
        BeginAct(CmdKind::Attack, t, -1, 0, CommandWindow("attack"), "Attack");
        break; }
    case CmdKind::Magic: {
        if (!sora || c.index < 0 || c.index >= kMagic) return;
        if (!MagicUsable(c.index)) { Info("Not enough MP"); return; }
        int t = magic[c.index].targetEnemy ? ValidTarget(c.target) : -1;
        int l = MagicLevel(c.index);
        BeginAct(CmdKind::Magic, t, c.index, MagicCmd(c.index), magic[c.index].castWindowSec, magic[c.index].names[l - 1]);
        for (auto& u : g_units) if (u.party()) g_act.hpBefore.push_back({u.actor, u.hp});
        break; }
    case CmdKind::Item: {
        if (c.index < 0 || c.index >= kItems) return;
        int n = game::InventoryCount(items[c.index].saveOffset);
        if (n <= 0) { Info("None left"); return; }
        // Who a single-target item is for (a party member; the user himself when none was chosen).
        int on = (c.on >= 0 && c.on < (int)g_units.size() && g_units[c.on].party() && !g_units[c.on].ko()) ? c.on : g_current;
        if (!sora) {
            // A partner uses an item equipped on him. KH2 doesn't let the mod run a partner's item (his own AI
            // only uses one by itself), so the mod applies it: the item's effect on its target, one used up.
            bool mine = false; for (auto& si : ItemSlots(CharBlock(me))) if (si.row == c.index) mine = true;
            if (!mine) { Info(me.name + " doesn't have " + items[c.index].name + " equipped"); return; }
            BeginAct(CmdKind::Item, -1, c.index, items[c.index].cmdId, 1.2f, items[c.index].name);
            g_act.itemMark = n;
            g_act.target = -1;
            g_act.itemOn = on;
            break;
        }
        // Used through KH2's own command menu (Items, then the slot in c.target), so KH2's animation, effect,
        // heal and inventory count all happen as in the normal game.
        BeginAct(CmdKind::Item, -1, c.index, 0, 9.0f, items[c.index].name);
        g_act.itemMark = n;
        g_act.target = -1;
        g_act.itemOn = on;
        g_act.cmd = (uint16_t)std::max(0, c.target); // the slot's position in KH2's Items list
        for (auto& u : g_units) if (u.party()) g_act.hpBefore.push_back({u.actor, u.hp});
        break; }
    case CmdKind::Defend:
        me.defending = true;
        BeginAct(CmdKind::Defend, -1, -1, 0, sora ? guardSec : 0.4f, "Defend");
        break;
    case CmdKind::Move: {
        if (sora || c.index < 0 || c.index >= kMoves || !MoveFor(c.index, me.model)) return;
        const PartnerMove& m = partnerMoves[c.index];
        int t = m.targetEnemy ? ValidTarget(c.target) : -1;
        if (!HasAbility(m.abilityList, m.abilityId)) { Info(me.name + " needs " + m.name + " equipped (pause menu: Abilities)"); return; }
        if (HealMove(c.index) && !AnyoneHurt()) { Info("Nobody needs healing"); return; }
        BeginAct(CmdKind::Move, t, c.index, 0, m.windowSec, m.name);
        break; }
    case CmdKind::Party: {
        // KH2's own Party command (command menu page 2): the partner in slot c.index leaves, the world's partner
        // joins.
        if (!sora || c.index < 0 || c.index >= (int)g_slots.size()) return;
        bool here = false; for (auto& u : g_units) if (u.side == Side::Partner && u.model.rfind(g_slots[c.index], 0) == 0) here = true;
        if (!here) { Info("That partner isn't here"); return; }
        BeginAct(CmdKind::Party, -1, c.index, 0, 9.f, "Party");
        g_act.step = 1; g_act.stepStart = NowSec();
        g_swapSlot = c.index; g_swapLeaving = g_slots[c.index];
        break; }
    case CmdKind::Escape:
        if (!g_encounter) { Info("Can't escape!"); return; }
        Info("Escaped!");
        // The enemies go back where they came from: the room's pool.
        for (auto& u : g_units) if (!u.party()) g_pool.push_back({u.actor, u.objentry, u.status, u.objectId, u.x, u.y, u.z, false});
        StartReturn("escape");
        break;
    }
}

void AutoPick() {
    // Test mode: nobody is playing, so the party picks its own commands, covering each kind.
    Unit& me = g_units[g_current];
    int t = FirstEnemy(); if (t < 0) return;
    BattleCommand c{CmdKind::Attack, 0, t};
    static int soraTurns = 0, partnerTurns = 0;
    if (me.side == Side::Sora && g_testItemSora < 2) {
        ++g_testItemSora;
        int donald = -1; for (int i = 0; i < (int)g_units.size(); ++i) if (g_units[i].side == Side::Partner && IsDonald(g_units[i].model)) donald = i;
        for (auto& si : SoraItemSlots()) if (game::InventoryCount(items[si.row].saveOffset) > 0) {
            if (g_testItemSora == 2 && items[si.row].party) continue;   // the second turn: a single-target item
            int on = g_testItemSora == 2 && donald >= 0 ? donald : g_current;
            Log("test: Sora uses %s (slot %d) on %s, item turn %d of this battle", items[si.row].name, si.pos, g_units[on].name.c_str(), g_testItemSora);
            HandleCommand({CmdKind::Item, si.row, si.pos, on});
            return;
        }
    }
    bool& partnerDone = IsDonald(me.model) ? g_testItemDonald : g_testItemGoofy;
    if (me.side == Side::Partner && (IsDonald(me.model) || IsGoofy(me.model)) && !partnerDone) {
        partnerDone = true;
        for (auto& si : ItemSlots(CharBlock(me))) if (game::InventoryCount(items[si.row].saveOffset) > 0) {
            int on = IsDonald(me.model) ? SoraIndex() : g_current;   // Donald's on Sora, Goofy's on himself
            Log("test: %s uses %s (slot %d) on %s", me.name.c_str(), items[si.row].name, si.pos, on >= 0 ? g_units[on].name.c_str() : "-");
            HandleCommand({CmdKind::Item, si.row, si.pos, on});
            return;
        }
    }
    if (me.side == Side::Sora && g_testKo == 2) {
        for (int i = 0; i < kMagic; ++i) if (!strcmp(magic[i].key, "cure") && MagicCmd(i)) {
            Wr<int32_t>(G(RVA_Slot0) + 0x180, game::SoraMaxMp()); // test only: enough MP for it
            Log("test: Sora casts Cure for Donald");
            HandleCommand({CmdKind::Magic, i, -1});
            return;
        }
    }
    if (me.side == Side::Sora) {
        int spell = -1, item = -1;
        for (int i = 0; i < kMagic; ++i) if (magic[i].targetEnemy && MagicUsable(i)) { spell = i; break; }
        int itemPos = -1;
        if (AnyoneHurt()) for (auto& si : SoraItemSlots()) if (game::InventoryCount(items[si.row].saveOffset) > 0) { item = si.row; itemPos = si.pos; break; }
        switch (soraTurns++ % 4) {
        case 1: if (spell >= 0) c = {CmdKind::Magic, spell, t}; break;
        case 2: if (item >= 0) c = {CmdKind::Item, item, itemPos}; break;
        case 3: c = {CmdKind::Defend, 0, -1}; break;
        default: break;
        }
    } else if (partnerTurns++ % 2 == 1) {
        // Cycle through this partner's own moves.
        std::vector<int> mv; for (int i = 0; i < kMoves; ++i) if (MoveFor(i, me.model) && HasAbility(partnerMoves[i].abilityList, partnerMoves[i].abilityId) && (!HealMove(i) || AnyoneHurt())) mv.push_back(i);
        if (!mv.empty()) c = {CmdKind::Move, mv[(partnerTurns / 2) % mv.size()], t};
    }
    HandleCommand(c);
}

// The act going on heals the party (a Cure, a party item or a partner's healing move).
bool HealingAct() {
    if (g_act.sheetIndex < 0) return false;
    if (g_act.kind == CmdKind::Magic) return g_act.sheetIndex < kMagic && !strcmp(magic[g_act.sheetIndex].key, "cure");
    if (g_act.kind == CmdKind::Item) return g_act.sheetIndex < kItems && items[g_act.sheetIndex].party;
    if (g_act.kind == CmdKind::Move) return g_act.sheetIndex < kMoves && HealMove(g_act.sheetIndex);
    return false;
}
// After a partner's Cure: Sora's real HP back (it read low for the partner's AI), plus what the Cure healed.
void RestoreSoraHp() {
    if (g_act.fakeHp <= 0) return;
    int fake = std::max(1, game::RoxasMaxHp() / 10), now2 = game::RoxasHp();
    int healed = std::max(0, now2 - fake);
    int real = std::min(game::RoxasMaxHp(), g_act.fakeHp + healed);
    Wr<int32_t>(G(RVA_Slot0), real);
    Log("  Sora's HP back to %d (Cure healed %d)", real, healed);
    g_act.fakeHp = -1;
}
void TickAct(double now) {
    Unit& me = g_units[g_current];
    bool sora = me.side == Side::Sora;
    if (sora && !g_act.cursorDone && (g_act.kind == CmdKind::Attack || g_act.kind == CmdKind::Item || g_act.kind == CmdKind::Party)) {
        g_act.cursorDone = true;
        int c = ReadKh2Cursor();
        if (c >= 0 && c != g_kh2Cursor) Log("  KH2 cursor: %d (the mod thought %d)", c, g_kh2Cursor);
        if (c >= 0) g_kh2Cursor = c;
    }
    bool wantsEnemy = g_act.kind == CmdKind::Attack || (g_act.kind == CmdKind::Magic && magic[g_act.sheetIndex].targetEnemy) ||
                      (g_act.kind == CmdKind::Move && partnerMoves[g_act.sheetIndex].targetEnemy);
    if (wantsEnemy && (g_act.target < 0 || g_act.target >= (int)g_units.size() || g_units[g_act.target].party())) g_act.target = FirstEnemy();
    int t = g_act.target;
    double st = now - g_act.stepStart;
    if (g_act.step == 0) {
        float reach = 0;
        if (g_act.kind == CmdKind::Attack) reach = sora ? attackReach : partnerReach;
        else if (g_act.kind == CmdKind::Magic) reach = magic[g_act.sheetIndex].castReach;
        else if (g_act.kind == CmdKind::Move && partnerMoves[g_act.sheetIndex].targetEnemy)
            reach = partnerMoves[g_act.sheetIndex].technique ? partnerReach : (!strcmp(partnerMoves[g_act.sheetIndex].name, "Fire") ? 150.f : 350.f); // only Fire works at melee range; the others need him within casting range
        if (!sora && g_act.kind == CmdKind::Move && t >= 0 && reach <= 0)
            Face(me, YawTo(g_units[t].x - me.x, g_units[t].z - me.z));   // he casts the way he faces
        if (reach > 0 && t >= 0 && st < stepSec) { StepToward(me, g_units[t], reach, (float)(st / stepSec)); return; }
        if (reach > 0 && t >= 0) StepToward(me, g_units[t], reach, 1.f);
        g_act.step = 1; g_act.stepStart = now; st = 0;
    }
    // Step 1: the battler acts with KH2's own moves.
    if (!g_act.ran) {
        g_act.ran = true;
        uint32_t th = t >= 0 ? g_units[t].handle : 0;
        if (sora && g_act.kind == CmdKind::Magic) {
            bool ok = game::RunCommand(me.actor, g_act.cmd, th);
            Log("%s: command %u (%s) on %s -> %s", me.name.c_str(), g_act.cmd, g_action.c_str(), t >= 0 ? g_units[t].name.c_str() : "-", ok ? "run" : "FAULT");
        } else if (!sora && g_act.kind == CmdKind::Item) {
            game::BlockMelee(me.actor); // he stands while the item is applied
        } else if (!sora && g_act.kind == CmdKind::Move && HealMove(g_act.sheetIndex)) {
            // A partner's Cure: KH2's AI only casts it when Sora is nearly out of HP, so for this turn Sora's HP
            // reads low (he is frozen meanwhile); his real HP, plus what the Cure heals, is put back after.
            GateAbilities(me.model, partnerMoves[g_act.sheetIndex].abilityId);
            game::BlockMelee(me.actor);
            g_act.fakeHp = game::RoxasHp();
            Wr<int32_t>(G(RVA_Slot0), std::max(1, game::RoxasMaxHp() / 10));
            g_act.healDirect = true;
            Log("%s: only %s equipped; Sora's HP reads %d for his AI (really %d)", me.name.c_str(), g_action.c_str(), std::max(1, game::RoxasMaxHp() / 10), g_act.fakeHp);
        } else if (!sora && (g_act.kind == CmdKind::Attack || g_act.kind == CmdKind::Move)) {
            uint16_t keep = g_act.kind == CmdKind::Move ? partnerMoves[g_act.sheetIndex].abilityId : 0;
            GateAbilities(me.model, keep);
            game::BlockMelee(keep ? me.actor : 0);
            Log("%s: only %s equipped for this turn (%s) on %s", me.name.c_str(), keep ? g_action.c_str() : "melee", g_action.c_str(), t >= 0 ? g_units[t].name.c_str() : "-");
        }
    }
    if (!sora) {
        // What the partner actually does (its command record and motion), to learn its moves' ids.
        uint16_t rec = Rd<uint16_t>(me.actor + 0xEA8); uint32_t mo = Rd<uint32_t>(me.actor + 0x180);
        if (rec != g_act.lastRec || mo != g_act.lastMotion) {
            Log("  %s t=%.2f record %u motion %u mp %d", me.name.c_str(), st, rec, mo, Rd<int32_t>(me.status + 0x180));
            g_act.lastRec = rec; g_act.lastMotion = mo;
        }
        if (g_act.kind == CmdKind::Attack) {
            // A partner's swing runs to its end before the turn does (melee motions 151-153, 173, 181-182).
            bool swinging = (mo >= 151 && mo <= 153) || mo == 173 || mo == 181 || mo == 182;
            if (swinging) {
                g_act.window = std::max(g_act.window, (float)st + 0.4f);
                if (g_act.hitTime < 0) g_act.hitTime = now;   // the swing has started
                game::BlockMelee(me.actor);                    // one strike: no second swing
            }
            // The turn ends when that one swing is over.
            if (g_act.hitTime >= 0 && mo <= 2 && now - g_act.hitTime > 0.4) g_act.window = std::min(g_act.window, (float)st);
        }
        if (g_act.kind == CmdKind::Move) {
            const PartnerMove& m = partnerMoves[g_act.sheetIndex];
            // The move is under way: one of its listed motions, any ability motion (150 and up; plain swings are
            // blocked on Move turns), or the partner's MP going down.
            bool moving = mo >= 150; for (auto mm : m.moveMotions) if (mm && mo == mm) moving = true;
            int mpNow = Rd<int32_t>(me.status + 0x180);
            if (g_act.partnerMp < 0) g_act.partnerMp = mpNow;
            if (mpNow < g_act.partnerMp) moving = true;
            if (moving && g_act.hitTime < 0) {
                g_act.hitTime = now; g_act.hit = true;
                Log("  %s: %s started (motion %u)", me.name.c_str(), m.name, mo);
                // Once is enough: take the move away again so the AI can't repeat it this turn.
                GateAbilities(me.model, 0);
                game::BlockMelee(me.actor);
            }
            // Sora's real HP back once the Cure has landed (or never started).
            if (g_act.healDirect && g_act.fakeHp > 0 && ((g_act.hit && now - g_act.hitTime > 2.0) || (!g_act.hit && st > 4.0))) {
                RestoreSoraHp();
                if (!g_act.hit) g_act.window = std::min(g_act.window, (float)st + 0.2f);
            }
            if (g_act.healDirect && g_act.fakeHp > 0 && g_act.hit) g_act.window = std::max(g_act.window, (float)st + 0.3f); // not before
            if (g_act.healDirect && !g_act.hit && st > 4.0 && g_act.fakeHp < 0 && !g_act.healFallback) {
                g_act.healFallback = true;
                g_act.hit = true; g_act.hitTime = now;
                for (auto& u : g_units) if (u.party() && !u.down) {
                    uintptr_t st2 = u.side == Side::Sora ? G(RVA_Slot0) : u.status;
                    if (st2) Wr<int32_t>(st2, std::min(u.maxHp, u.hp + std::max(1, u.maxHp / 4)));
                }
                Revive(0.25f, (me.name + "'s Cure").c_str());
                Log("  %s's Cure: his AI didn't cast it (last motion %u); healed the party directly", me.name.c_str(), mo);
            }
            // Done once the move has played out and the partner is back to standing or walking.
            bool idle = mo <= 2;
            if (g_act.hitTime >= 0 && idle && now - g_act.hitTime > 0.6) g_act.window = (float)st;
            if (g_act.hitTime >= 0 && !idle) g_act.window = std::max(g_act.window, (float)st + 0.5f);
            if (g_act.hitTime >= 0) g_act.window = std::min(g_act.window, (float)(g_act.hitTime - g_act.stepStart) + 7.f); // long moves (Tornado) end the turn by 7 s
        }
    }
    if (g_act.kind == CmdKind::Party && sora) {
        g_kh2Menu = true;
        // KH2's command menu: Left (page 2: Attack, Summon, Party, Limit, opening at page 1's cursor, Attack),
        // Down, Down (Party), Cross: its FRIENDS list; Down x slot, Cross: that partner leaves and the world's
        // partner joins. Then Left again, back to page 1.
        std::vector<uint16_t> seq = {0x0080}; CursorTo(seq, 2); seq.push_back(0x4000); seq.push_back(0);
        for (int k = 0; k < g_act.sheetIndex; ++k) seq.push_back(0x0040);
        seq.push_back(0x4000);
        int k = (int)(st / 0.5); double in = st - k * 0.5;
        g_press = (k < (int)seq.size() && in < 0.12) ? seq[k] : 0;
        if (g_act.hit && g_act.hitTime > 0) {
            double after = now - g_act.hitTime;
            g_press = (after > 1.0 && after < 1.12) ? 0x0080 : 0;   // back to page 1
            g_kh2Cursor = 0;                                         // KH2 puts it back on Attack after a swap
            if (after > 1.8) g_act.window = (float)st;
        }
    }
    else if (g_act.kind == CmdKind::Attack && sora) {
        // KH2's cursor to Attack, then Cross, once: one strike.
        std::vector<uint16_t> seq; CursorTo(seq, 0); seq.push_back(0x4000);
        int k = (int)(st / 0.25); double in = st - k * 0.25;
        g_press = (k < (int)seq.size() && in < 0.12) ? seq[k] : 0;
        if (k >= (int)seq.size()) g_kh2Cursor = 0;
    }
    else if (g_act.kind == CmdKind::Item && sora) {
        g_kh2Menu = true; // KH2's own menu is in use: the mod's windows step aside
        // Down, Down (Items), Cross, Down x slot, Cross, (target) Cross: one press every 0.5 s (KH2's menu needs the time to open
        // its list). Then, once the item is used, Up, Up to put KH2's cursor back on Attack.
        // Then KH2 asks for the target (PARTY: "Everyone" for a party item, else the first is Sora): Cross.
        std::vector<uint16_t> seq; CursorTo(seq, 2); seq.push_back(0x4000); seq.push_back(0);
        for (int k = 0; k < g_act.cmd; ++k) seq.push_back(0x0040);
        seq.push_back(0x4000); seq.push_back(0);
        // KH2's target list (single-target items): Sora, then the partners by party slot.
        if (!items[g_act.sheetIndex].party && g_act.itemOn >= 0 && g_act.itemOn < (int)g_units.size()) {
            const Unit& tu = g_units[g_act.itemOn];
            int pos = 0;
            if (tu.side == Side::Partner) for (int k = 0; k < (int)g_slots.size(); ++k) if (tu.model.rfind(g_slots[k], 0) == 0) pos = k + 1;
            for (int k = 0; k < pos; ++k) seq.push_back(0x0040);
        }
        seq.push_back(0x4000);
        int k = (int)(st / 0.5); double in = st - k * 0.5;
        g_press = 0;
        if (k < (int)seq.size()) g_press = in < 0.12 ? seq[k] : 0;
        uint32_t mo = Rd<uint32_t>(me.actor + 0x180);
        if (g_act.itemCmdMark < 0) g_act.itemCmdMark = game::g_itemCmdCount.load();
        bool ran = game::g_itemCmdCount.load() != g_act.itemCmdMark && game::g_itemCmdActor.load() == me.actor;
        if ((mo == 9 || ran) && !g_act.hit) { g_act.hit = true; g_act.hitTime = now; Log("  Sora is using %s (KH2's item %s)", g_action.c_str(), ran ? "command" : "motion"); }
        if (!g_act.hit && st > seq.size() * 0.5 + 2.0 && !g_act.backedOut) {
            // KH2 didn't take it: back out of its Items list (Circle, twice) so the next turn starts on Attack.
            double b = st - (seq.size() * 0.5 + 2.0);
            g_press = (b < 0.12 || (b > 0.5 && b < 0.62)) ? 0x2000 : 0;
            if (b > 1.0) { g_act.backedOut = true; g_act.window = (float)st; g_kh2Cursor = 2; }
        }
        if (g_act.hit) {
            double after = now - g_act.hitTime;
            g_kh2Cursor = 2;                                         // KH2 leaves it on Items
            if (after > 2.2) g_act.window = (float)st;               // done
        }
    }
    else if (g_act.kind == CmdKind::Defend && sora) g_press = 0x8000;                                            // Square: Guard
    else g_press = 0;

    bool partnerMove = !sora && g_act.kind == CmdKind::Move;
    if (!partnerMove && g_act.kind != CmdKind::Item && g_act.kind != CmdKind::Party && EnemyHpSum() < g_act.enemyHpMark && g_act.hitTime < 0) {
        g_act.hitTime = now; g_act.hit = true;
        for (auto& u : g_units) u.settle = !u.party();
    }
    bool done = st >= g_act.window || (!partnerMove && g_act.kind != CmdKind::Item && g_act.hitTime >= 0 && now - g_act.hitTime >= attackHitGraceSec);
    if (!done) return;

    switch (g_act.kind) {
    case CmdKind::Attack:
        if (!g_act.hit && t >= 0) {
            // The swing missed (enemy moved, KH2's command cursor elsewhere): the blow still lands, as in FFX.
            Unit& tg = g_units[t];
            float pw = sora ? attackFallbackPower : attackFallbackPower * 0.6f;
            int dmg = std::max(1, (int)std::lround(tg.maxHp * pw));
            int after = game::TakeDamage(tg.actor, -dmg, true);
            tg.settle = true;
            Log("%s's attack missed; applied %d to %s (hp after %d)", me.name.c_str(), dmg, tg.name.c_str(), after);
        } else Log("%s's attack hit (KH2 move)", me.name.c_str());
        testmode::Pass(sora ? "attack" : "partner-turn");
        FinishAction(me, CommandRank("attack"));
        break;
    case CmdKind::Magic: {
        int mp = game::SoraMp();
        bool cast = mp < g_act.mpMark || g_act.hit;
        for (auto& hb : g_act.hpBefore) for (auto& u : g_units) if (u.actor == hb.first && u.hp > hb.second) cast = true; // a heal landed
        // KH2's executor runs the spell; its command menu is what normally charges the MP, so charge it here.
        int cost = MagicCost(g_act.sheetIndex);
        if (mp >= g_act.mpMark && cost != 0) {
            int left = (cost < 0 || cost >= 0xFF) ? 0 : std::max(0, g_act.mpMark - cost);
            Wr<int32_t>(G(RVA_Slot0) + 0x180, left);
            mp = left;
        }
        Log("%s cast %s: mp %d -> %d (cost %d), enemy hit %d", me.name.c_str(), g_action.c_str(), g_act.mpMark, mp, cost, g_act.hit);
        if (cast) testmode::Pass("kh2-magic");
        else Info(g_action + " fizzled");
        if (cast && !magic[g_act.sheetIndex].targetEnemy && !strcmp(magic[g_act.sheetIndex].key, "cure"))
            Revive(0.25f + 0.15f * (MagicLevel(g_act.sheetIndex) - 1), g_action.c_str());
        FinishAction(me, ctbBaseRank);
        break; }
    case CmdKind::Item: {
        const Item& it = items[g_act.sheetIndex];
        int n = game::InventoryCount(it.saveOffset);
        // Used by KH2 itself: someone's HP went up, or the inventory count went down.
        for (auto& hb : g_act.hpBefore) for (auto& u : g_units) if (u.actor == hb.first && u.hp > hb.second) g_act.hit = true;
        if (n < g_act.itemMark) g_act.hit = true;
        if (g_act.itemCmdMark >= 0 && game::g_itemCmdCount.load() != g_act.itemCmdMark && game::g_itemCmdActor.load() == me.actor) g_act.hit = true;
        if (!g_act.hit) {
            // KH2 didn't get to it: apply the item's effect so the turn isn't lost (a party item heals everyone,
            // any other the one using it).
            for (auto& u : g_units) {
                uintptr_t onActor = (g_act.itemOn >= 0 && g_act.itemOn < (int)g_units.size()) ? g_units[g_act.itemOn].actor : me.actor;
                if (!u.party() || u.ko() || (!it.party && u.actor != onActor)) continue;
                uintptr_t st2 = u.side == Side::Sora ? G(RVA_Slot0) : u.status;
                if (st2 && it.hp > 0) {
                    int to = std::min(u.maxHp, u.hp + std::max(1, (int)std::lround(u.maxHp * it.hp)));
                    Wr<int32_t>(st2, to);
                    if (to > u.hp) g_popups.push_back({u.actor, "+" + std::to_string(to - u.hp) + " HP", NowSec(), 0x7CF08A});
                }
            }
            for (auto& u : g_units) {
                // MP: Sora's is in the save slot, a partner's at his status +0x180 (max +0x184).
                uintptr_t onActor = (g_act.itemOn >= 0 && g_act.itemOn < (int)g_units.size()) ? g_units[g_act.itemOn].actor : me.actor;
                if (it.mp <= 0 || !u.party() || u.ko() || (!it.party && u.actor != onActor)) continue;
                uintptr_t mpAt = u.side == Side::Sora ? G(RVA_Slot0) + 0x180 : (u.status ? u.status + 0x180 : 0);
                if (!mpAt) continue;
                int maxMp = Rd<int32_t>(mpAt + 4); if (maxMp <= 0 || maxMp > 999) maxMp = 100;
                int had = Rd<int32_t>(mpAt), to = std::min(maxMp, had + std::max(1, (int)std::lround(maxMp * it.mp)));
                Wr<int32_t>(mpAt, to);
                if (to > had) g_popups.push_back({u.actor, "+" + std::to_string(to - had) + " MP", NowSec() + 0.25, 0x7CB8FF});
            }
            if (n >= g_act.itemMark) game::SetInventoryCount(it.saveOffset, std::max(0, n - 1));
            Log("item %s (%s on %s): applied by the mod", it.name, me.name.c_str(), (g_act.itemOn >= 0 && g_act.itemOn < (int)g_units.size()) ? g_units[g_act.itemOn].name.c_str() : "-");
        } else Log("item %s used by %s through KH2 (count %d -> %d)", it.name, me.name.c_str(), g_act.itemMark, n);
        if (it.party && it.hp > 0) Revive(it.hp, it.name);
        testmode::Pass(sora ? "item" : "partner-item");
        FinishAction(me, CommandRank("submenu_items"));
        break; }
    case CmdKind::Defend:
        FinishAction(me, CommandRank("defend"));
        break;
    case CmdKind::Move:
        RestoreSoraHp();
        Log("%s %s %s", me.name.c_str(), g_act.hit ? "used" : "did not get to use", g_action.c_str());
        if (g_act.hit) testmode::Pass("partner-move");
        if (g_act.hit && HealMove(g_act.sheetIndex)) Revive(0.25f, (me.name + "'s " + g_action).c_str());
        else if (!g_act.hit && partnerMoves[g_act.sheetIndex].targetEnemy && t >= 0 && !g_units[t].party()) {
            // His AI didn't cast it (out of its range, or no line to the target): the move still lands, as in FFX.
            Unit& tg = g_units[t];
            int dmg = std::max(1, (int)std::lround(tg.maxHp * attackFallbackPower * 0.8f));
            int after = game::TakeDamage(tg.actor, -dmg, true);
            tg.settle = true;
            Log("  %s's %s applied by the mod: %d to %s (hp after %d)", me.name.c_str(), g_action.c_str(), dmg, tg.name.c_str(), after);
        }
        else if (!g_act.hit) Info(me.name + " couldn't use " + g_action);
        testmode::Pass("partner-turn");
        FinishAction(me, partnerMoves[g_act.sheetIndex].technique ? CommandRank("submenu_tech") : CommandRank("submenu_magic"));
        break;
    case CmdKind::Party:
        // A swap costs no turn: the line is laid out again and Sora chooses again.
        if (!g_act.hit) { Info("The swap didn't happen"); Log("party swap: nobody joined"); g_swapSlot = -1; }
        LayOutLines();
        for (auto& u : g_units) { PinHome(u); Face(u, u.hyaw); }
        g_press = 0; g_kh2Menu = false;
        RestoreAbilities();
        SetPhase(Phase::PlayerMenu);
        break;
    default: FinishAction(me, ctbBaseRank); break;
    }
}

// Enemy-like actors (an enemy model) that are neither in the battle nor in the pool: logged once, and held so
// their own AI can't act out of the turn order; with HP they are pooled like any other enemy.
std::vector<uintptr_t> g_strays;
void CatchStrays(const std::vector<ActorInfo>& actors) {
    for (auto& a : actors) {
        bool enemyModel = (a.name[0] == 'M' || a.name[0] == 'B') && a.name[1] == '_' && strncmp(a.name, "M_EX350", 7) != 0;
        // hp 0 of its max: a defeated enemy playing out its death; left alone (one with no status yet is still held)
        bool defeated = a.status && a.maxHp > 0 && a.hp <= 0;
        if (!enemyModel || InPool(a.actor) || game::IsDying(a.actor) || defeated) continue;
        bool inBattle = false; for (auto& u : g_units) if (u.actor == a.actor) inBattle = true;
        if (inBattle) continue;
        if (std::find(g_strays.begin(), g_strays.end(), a.actor) == g_strays.end()) {
            g_strays.push_back(a.actor);
            Log("stray enemy-like actor %s (objectId %u, type %u, hp %d/%d, status %p): kept out of the battle", a.name, a.objectId, a.type, a.hp, a.maxHp, (void*)a.status);
        }
        if (a.status && a.maxHp > 0 && a.hp > 0) g_pool.push_back({a.actor, a.objentry, a.status, a.objectId, a.x, a.y, a.z, false});
        else { uint32_t f = Rd<uint32_t>(a.actor + 0x6C8); if (!(f & 0x80)) Wr<uint32_t>(a.actor + 0x6C8, f | 0x80); }
    }
}
void TickBattle(const std::vector<ActorInfo>& actors, double now) {
    if (g_active && g_staged) CatchStrays(actors);
    for (auto& p : g_pool) game::SetPos(p.actor, p.x, p.y + hideDepth, p.z);
    if (g_abil.active && g_phase != Phase::Act) RestoreAbilities();
    RefreshUnits(actors);
    int si = SoraIndex();

    // Defend halves every HP drop on a defending party member.
    for (auto& u : g_units) {
        if (!u.party()) continue;
        if (u.lastHp >= 0 && u.hp < u.lastHp && u.hp > 0 && u.defending) {
            int back = (int)std::lround((u.lastHp - u.hp) * (1.f - defendDamageMul));
            uintptr_t st = u.side == Side::Sora ? G(RVA_Slot0) : u.status;
            if (back > 0 && st) { Wr<int32_t>(st, std::min(u.maxHp, u.hp + back)); u.hp += back; }
        }
        u.lastHp = u.hp;
    }

    // Partners: knocked out at 0 HP, and they stay down (KH2 would get them up again by itself after a while).
    for (auto& u : g_units) {
        if (u.side != Side::Partner) continue;
        if (u.down) {
            // Diagnostics: the motions a knocked-out partner goes through (to keep him in KH2's down pose).
            uint32_t mo = Rd<uint32_t>(u.actor + 0x180);
            if (mo != u.downMotion) { Log("  %s down: motion %u", u.name.c_str(), mo); u.downMotion = mo; }
        }
        if (u.down && u.actor) {
            // Keep KH2 from getting him up by himself: its get-up countdown never runs low.
            float left = Rd<float>(u.actor + kDownTimer);
            if (left > 0.f && left < 1000.f) Wr<float>(u.actor + kDownTimer, 2000.f);
        }
        if (!u.down && u.hp <= 0) {
            u.down = true;
            DownPose(u, true);
            Log("%s is knocked out", u.name.c_str());
            Info(u.name + " is knocked out!");
        } else if (u.down && u.hp > 0 && g_phase == Phase::Act && HealingAct()) {
            // The Cure (or healing item or move) being used reached him: that's the revive.
            u.down = false; u.lastHp = u.hp; g_act.hit = true;
            DownPose(u, false);
            Log("%s revived by %s (%d HP)", u.name.c_str(), g_action.c_str(), u.hp);
            Info(u.name + " is back up!");
            if (testmode::Active() && g_testKo == 2) { g_testKo = 3; testmode::Pass("partner-revived"); }
        } else if (u.down && u.hp > 0) {
            int after = game::TakeDamage(u.actor, -u.hp, true);  // back down, through KH2's own damage (its KO)
            Log("%s got up by himself (%d HP) %.1f s after going down; knocked out again (hp %d)", u.name.c_str(), u.hp, u.downAt > 0 ? NowSec() - u.downAt : -1.0, after);
            DownPose(u, true);
            u.hp = 0; u.lastHp = 0;
        }
    }
    if (si >= 0 && g_units[si].hp <= 0 && g_phase != Phase::GameOver) {
        Log("Sora fell");
        if (testmode::Active()) testmode::Pass("sora-ko-loses");
        g_banner = "Game Over";
        SetPhase(Phase::GameOver);
    }

    // Victory: every foe gone for a moment (and a forced battle has ended its waves).
    if (g_phase != Phase::GameOver && g_phase != Phase::Victory && g_phase != Phase::Returning && g_phase != Phase::PartyMenu) {
        if (EnemyCount() == 0) {
            if (g_noFoesSince < 0) g_noFoesSince = now;
            bool forced = Rd<uint32_t>(G(RVA_BattleStatus)) == 2;
            double need = (g_encounter || !forced) ? 1.2 : 4.0;
            if (now - g_noFoesSince > need) {
                g_banner = "Victory!";
                g_press = 0;
                PrizeMagnet(true); // everything left on the floor goes to Sora
                if (!g_victoryCounted) {
                    for (auto& a : actors) if (a.type != 0 && a.type != 1 && a.type != 3 && a.type != 4)
                        Log("  at victory: %s type %u objectId %u at (%.0f,%.0f,%.0f)", a.name, a.type, a.objectId, a.x, a.y, a.z);
                    g_victoryCounted = true; ++g_wins;
                    SaveProgress();
                    testmode::Pass("battle-won");
                    Log("victory (wins=%d)", g_wins);
                    Revive(0.25f, "the end of the battle");
                }
                SetPhase(Phase::Victory);
            }
        } else g_noFoesSince = -1;
    }

    RefreshMagicCosts();
    double t = now - g_phaseStart;
    if (g_staged && (g_phase == Phase::TurnIntro || g_phase == Phase::PlayerMenu))
        for (auto& u : g_units) if (!u.ko()) PinHome(u);
    bool curValid = g_current >= 0 && g_current < (int)g_units.size();
    switch (g_phase) {
    case Phase::TurnIntro:
        if (!curValid || g_units[g_current].ko()) { NextTurn(); break; }
        if (t >= turnIntroSec) {
            Unit& u = g_units[g_current];
            if (u.party()) SetPhase(Phase::PlayerMenu);
            else {
                g_act = Act{}; g_act.stepStart = now; g_act.partyHpMark = PartyHpSum(); g_action = "Attack";
                StepFrom(u);
                SetPhase(Phase::EnemyAct);
            }
        }
        break;
    case Phase::PlayerMenu: {
        if (!curValid || !g_units[g_current].party()) { NextTurn(); break; }
        if (testmode::Active() && g_testBattles == kTestSwapBattle && g_testSwap < 2 && g_units[g_current].side == Side::Sora && t > 2.0) {
            // Test: on one turn, Sora swaps Donald (slot 0) for the world's partner, then Goofy (slot 1) for Donald,
            // through KH2's Party command; a swap costs no turn, so he then acts as usual.
            int slot = g_testSwap++;
            Log("test: Party, swapping out %s (slot %d)", g_slots[slot].c_str(), slot);
            HandleCommand({CmdKind::Party, slot, -1});
            break;
        }
        if (testmode::Active() && t > 2.0) { AutoPick(); break; }
        std::vector<BattleCommand> cmds; cmds.swap(g_cmds);
        for (auto& c : cmds) { HandleCommand(c); if (g_phase != Phase::PlayerMenu) break; }
        break; }
    case Phase::Act:
        if (!curValid) { NextTurn(); break; }
        TickAct(now);
        break;
    case Phase::EnemyAct: {
        if (!curValid || g_units[g_current].party()) { NextTurn(); break; }
        Unit& u = g_units[g_current];
        double st = now - g_act.stepStart;
        if (g_act.step == 0) {
            // Step out toward the nearest party member; KH2's own AI then makes the attack.
            int tgt = -1; float best = 1e9f;
            for (int i = 0; i < (int)g_units.size(); ++i) if (g_units[i].party() && !g_units[i].ko()) {
                float dx = g_units[i].x - u.fx, dz = g_units[i].z - u.fz, d = dx * dx + dz * dz;
                if (d < best) { best = d; tgt = i; }
            }
            g_act.target = tgt;
            if (tgt >= 0 && !u.fixed) StepToward(u, g_units[tgt], enemyReach, (float)(st / stepSec));
            if (st >= stepSec || u.fixed || tgt < 0) { g_act.step = 1; g_act.stepStart = now; }
            break;
        }
        if (PartyHpSum() < g_act.partyHpMark && g_act.hitTime < 0) g_act.hitTime = now;
        float window = u.sheet >= 0 ? units[u.sheet].actWindowSec : 2.5f;
        if ((g_act.hitTime >= 0 && now - g_act.hitTime >= enemyHitGraceSec) || st >= window) {
            for (auto& o : g_units) o.settle = o.party() && g_act.hitTime >= 0;
            Spend(u, ctbBaseRank);
            SetPhase(Phase::Settle);
            testmode::Pass("turn-order");
        }
        break; }
    case Phase::Settle:
        if (t >= settleSec) { for (auto& u : g_units) u.settle = false; if (EnemyCount() > 0) StartRegroup(); }
        break;
    case Phase::Regroup: {
        float k = (float)(t / stepSec);
        for (auto& u : g_units) if (u.homed && !u.ko()) StepTo(u, u.hx, u.hy, u.hz, k);
        if (k >= 1.f) {
            for (auto& u : g_units) { if (!u.ko()) PinHome(u); Face(u, u.hyaw); }
            if (EnemyCount() > 0) NextTurn();
        }
        break; }
    case Phase::PartyMenu:
        // Press Start once; KH2's pause menu then belongs to the player until it closes.
        g_press = t < 0.15 ? 0x0008 : 0;
        if (game::MenuOpen()) g_menuSeen = true;
        if (g_menuSeen && !game::MenuOpen() && t > 0.5) {
            LayOutLines();
            for (auto& u : g_units) { PinHome(u); Face(u, u.hyaw); }
            Log("party menu closed");
            SetPhase(Phase::PlayerMenu);
        } else if (!g_menuSeen && t > 2.0) {
            Info("The menu can't open right now");
            g_press = 0;
            SetPhase(Phase::PlayerMenu);
        }
        break;
    case Phase::Victory:
        if (t >= ((g_testArena || g_spotHere) ? 2.6 : victorySec)) { // a battle map fades out quickly (prizes fly in first), before any late wave shows
            if (g_encounter) StartReturn("victory");
            else { EndBattle(); SetPhase(Phase::Field); }
        }
        break;
    case Phase::Hold:
        if (t >= 1.8) NextTurn();  // the battle map has faded in: first turn
        break;
    case Phase::GameOver:
        // KH2 shows its own game over; hand everything back to it.
        if (t >= 0.5) { EndBattle(); g_encounter = false; SetPhase(Phase::Field); }
        break;
    default: break;
    }
    if (g_active) ApplyFreezes();
}

// Keep the room's enemies out of the world: every enemy the room spawns (outside story and boss fights)
// goes into the pool, frozen and parked below the floor where it stood.
void PoolTick(const std::vector<ActorInfo>& actors) {
    for (int i = (int)g_pool.size() - 1; i >= 0; --i) {
        bool alive = false;
        for (auto& a : actors) if (a.actor == g_pool[i].actor && a.objentry == g_pool[i].objentry && a.status == g_pool[i].status && a.hp > 0) alive = true;
        if (!alive) g_pool.erase(g_pool.begin() + i);
    }
    if (BattleType() != 2) {
        for (auto& a : actors) {
            if (!IsFoe(a) || InPool(a.actor)) continue;
            bool inBattle = false; for (auto& u : g_units) if (u.actor == a.actor) inBattle = true;
            if (inBattle) continue;
            g_pool.push_back({a.actor, a.objentry, a.status, a.objectId, a.x, a.y, a.z, false});
            Log("pooled %s (objectId %u) from (%.0f,%.0f,%.0f)", a.name, a.objectId, a.x, a.y, a.z);
        }
    }
    for (auto& p : g_pool) game::SetPos(p.actor, p.x, p.y + hideDepth, p.z);
}

// An encounter: a few of the pool's enemies line up in front of the party, where it stands.
bool StartPoolBattle(const std::vector<ActorInfo>& actors) {
    const ActorInfo* me = nullptr;
    for (auto& a : actors) if (a.type == 0) { me = &a; break; }
    if (!me || g_pool.empty()) return false;
    std::shuffle(g_pool.begin(), g_pool.end(), g_rng);
    int maxN = std::min<int>((int)g_pool.size(), encounterMaxFoes);
    std::uniform_int_distribution<int> d(std::min(2, maxN), maxN);
    int n = testmode::Active() ? maxN : d(g_rng);
    float yaw = game::Facing(me->actor), fx = std::sin(yaw), fz = std::cos(yaw), rx = fz, rz = -fx;
    for (int i = 0; i < n; ++i) {
        PoolMob p = g_pool.back(); g_pool.pop_back();
        if (p.held) { Wr<uint32_t>(p.actor + 0x6C8, Rd<uint32_t>(p.actor + 0x6C8) & ~0x80u); }
        float off = (i - (n - 1) * 0.5f) * formationEnemySpacing;
        game::SetPos(p.actor, me->x + fx * encounterDistance + rx * off, me->y, me->z + fz * encounterDistance + rz * off);
        game::SetFacing(p.actor, std::atan2(-fx, -fz));
    }
    Log("encounter: %d of the room's enemies step in (%zu stay in the pool)", n, g_pool.size());
    std::vector<ActorInfo> now;
    game::EnumActors(now);
    RefreshUnits(now);
    return EnemyCount() > 0;
}

void TickField(const std::vector<ActorInfo>& actors, double now) {
#ifdef MENU_PROBE
    // Probe build: in the field, KH2's command menu page 2 (Left), then Down x5 and Up x6, a press every 1.5 s,
    // to see whether its cursor wraps. No battles.
    if (testmode::Active() && game::SafeGameplay()) {
        static double start = -1; static int lastK = -1;
        if (start < 0) { start = now + 3.0; testmode::Pass("menu-probe"); }
        // Page 2 wraps (Attack, Summon, Party, Limit) and opens at page 1's cursor position: from Attack, Left,
        // Down, Down is Party. Then Cross, and the member list a step at a time (3 s each).
        static const uint16_t seq[] = {0x0080, 0x0040, 0x0040, 0x4000, 0, 0x0040, 0, 0x0040, 0, 0x4000, 0, 0x0040, 0, 0x4000, 0, 0x2000, 0, 0x2000, 0};
        const int n = (int)(sizeof seq / sizeof seq[0]);
        double t = now - start; int k = t < 0 ? -1 : (int)(t / 2.0);
        g_probing = k >= 0 && k < n;
        g_press = (g_probing && t - k * 2.0 < 0.12) ? seq[k] : 0;
        if (g_probing && k != lastK && seq[k]) { lastK = k; Log("menu probe press %d: %04X", k, seq[k]); }
        return;
    }
#endif
    // A story or boss fight (KH2 battle type 2): take it over as a turn-based battle.
    bool foes = false;
    for (auto& a : actors) if (IsBattleFoe(a)) { foes = true; break; }
    if (foes && BattleType() == 2 && game::SafeGameplay()) {
        g_encounter = false; g_arena = -1;
        RefreshUnits(actors);
        if (EnemyCount() > 0) { BeginBattle(); return; }
    }
    PoolTick(actors);
    SetGroups(g_pool.empty() ? 0u : ((1u << groupBoss) | (1u << groupMob)));

    // Encounter meter: walking where the room has enemies fills it. No enemies here (a safe zone): no encounters.
    Location loc = game::Now();
    const ActorInfo* me = nullptr;
    for (auto& a : actors) if (a.type == 0) { me = &a; break; }
    if (!me || !game::SafeGameplay()) { g_lastPosValid = false; g_fieldSince = -1; return; }
    if (g_fieldSince < 0) g_fieldSince = now;
    if (g_lastPosValid && !g_pool.empty()) {
        float dx = me->x - g_lastPos[0], dz = me->z - g_lastPos[2];
        float d = std::sqrt(dx * dx + dz * dz);
        if (d > 2.f && d < 300.f) g_meter += d; // standing still (or being placed) never fills it
    }
    g_lastPos[0] = me->x; g_lastPos[1] = me->y; g_lastPos[2] = me->z; g_lastPosValid = true;

    bool ready = !g_pool.empty() && g_meter >= g_meterGoal && now - g_lastBattleEnd >= encounterMinGapSec;
    // Melty's test mode: fight right away, even from a room without enemies (on this world's battle maps).
    bool testGo = testmode::Active() && now - g_fieldSince > 3.0 && now - g_lastBattleEnd > 5.0 && testmode::WantEncounter();
    if (ready || (testGo && !g_pool.empty())) {
        // Fought here: at this room's battle spot if it has one, else where the party stands.
        g_arena = SpotForRoom(loc.world, loc.room);
        g_spotHere = g_arena >= 0;
        g_returnLoc = loc;
        g_returnPos[0] = me->x; g_returnPos[1] = me->y; g_returnPos[2] = me->z; g_returnPosValid = true;
        g_encounter = true;
        g_shatterStart = now;
        SetGroups((1u << groupPlayer) | (1u << groupParty) | (1u << groupBoss) | (1u << groupMob));
        Log("encounter in %02u/%02u (%zu enemies in the pool)%s", loc.world, loc.room, g_pool.size(), g_spotHere ? (std::string(", at spot ") + battleMaps[g_arena].key).c_str() : ", where the party stands");
        if (testmode::Active()) testmode::EncounterStarted();
        SetPhase(Phase::Shatter);
    } else if (testGo && g_pool.empty()) {
        // Melty's test mode in a room without enemies: travel to the nearest room of this world that has some.
        int m = kTestWorld ? NearestSpot(kTestWorld, kTestRoom) : NearestSpot(loc.world, loc.room);
        if (m < 0) return;
        g_arena = m; g_spotHere = false;
        g_returnLoc = loc;
        g_returnPos[0] = me->x; g_returnPos[1] = me->y; g_returnPos[2] = me->z; g_returnPosValid = true;
        g_encounter = true;
        g_shatterStart = now;
        Log("test: no enemies in %02u/%02u; travelling to %s (%s, btl %u)", loc.world, loc.room, battleMaps[m].key, battleMaps[m].roomName, battleMaps[m].btlProgram);
        if (testmode::Active()) testmode::EncounterStarted();
        SetPhase(Phase::Shatter);
    }
}

} // namespace

namespace battle {

void Init() {
    LoadProgress();
    std::random_device rd; g_rng.seed(rd());
    std::uniform_real_distribution<double> d(encounterMeterMin, encounterMeterMax);
    g_meterGoal = d(g_rng);
    Log("battle init: wins=%d meterGoal=%.0f", g_wins, g_meterGoal);
}

void Tick() {
    double now = NowSec();
    static std::vector<ActorInfo> actors;
    std::lock_guard<std::mutex> lk(g_lock);
    // A long gap between ticks means KH2 was paused (its menu was open).
    if (g_phase == Phase::PartyMenu && g_lastTick > 0 && now - g_lastTick > 0.4) g_menuSeen = true;
    g_lastTick = now;

    // Room loads: actors vanish, never touch them.
    if (!game::InField()) {
        if (g_phase == Phase::LoadingArena || g_phase == Phase::Returning) g_loadLeft = true;
        if (g_active && g_phase != Phase::LoadingArena && g_phase != Phase::Returning) {
            g_units.clear(); g_current = -1; g_active = false; g_staged = false;
            if (!g_encounter) SetPhase(Phase::Field);
        }
        g_lastPosValid = false;
        RestoreAbilities();
        g_pool.clear();
        game::SetBattleCamera(false);
        if (now - g_phaseStart > loadTimeoutSec && (g_phase == Phase::LoadingArena || g_phase == Phase::Returning)) {
            Log("load timeout, back to normal play");
            g_encounter = false; EndBattle(); SetPhase(Phase::Field);
        }
        return;
    }
    game::EnumActors(actors);

    // Cutscenes and scripted events: let everyone move, then resume.
    if (game::CutsceneOrEvent()) {
        RestoreAbilities();
        if (g_active || g_groupMask) UnfreezeAll();
        for (auto& p : g_pool) game::SetPos(p.actor, p.x, p.y + hideDepth, p.z); // pooled enemies stay out of the scene
        UpdateStage();
        return;
    }

    switch (g_phase) {
    case Phase::Field:
        TickField(actors, now);
        break;
    case Phase::Shatter:
        if (now - g_phaseStart < shatterSec) break;
        if (g_arena < 0 || g_spotHere) {
            g_shatterStart = -1;
            if (StartPoolBattle(actors)) { testmode::Pass("encounter"); BeginBattle(); }
            else { g_encounter = false; EndBattle(); SetPhase(Phase::Field); }
            break;
        } else {
            const BattleMap& a = battleMaps[g_arena];
            Location to{a.world, a.room, a.door, 0xFFFF, a.btlProgram, 0xFFFF};
            g_units.clear();
            UnfreezeAll();
            g_pool.clear();
            bool ok = game::RequestTransition(to);
            Log("battle map load %02u/%02u btl=%u: %s", to.world, to.room, to.btl, ok ? "requested" : "FAILED");
            g_loadLeft = false; g_testArena = ok; g_foesSince = -1;
            SetPhase(ok ? Phase::LoadingArena : Phase::Field);
            if (!ok) { g_encounter = false; EndBattle(); }
        }
        break;
    case Phase::LoadingArena:
        if (now - g_phaseStart > loadTimeoutSec) { Log("battle map load timed out; back to normal play"); g_encounter = false; EndBattle(); SetPhase(Phase::Field); break; }
        if (g_loadLeft && game::InField()) {
            Location l = game::Now();
            Log("battle map loaded: now %02u/%02u btl=%u (wanted btl=%u)", l.world, l.room, l.btl, battleMaps[g_arena].btlProgram);
            g_shatterStart = -1;
            SetPhase(Phase::AwaitFoes);
        } else if (now - g_phaseStart > 1.5 && !g_loadLeft && game::InField()) {
            Location l = game::Now();
            if (l.btl == battleMaps[g_arena].btlProgram && now - g_phaseStart > 3.0) { g_shatterStart = -1; Log("arena live: %02u/%02u btl=%u", l.world, l.room, l.btl); SetPhase(Phase::AwaitFoes); }
        }
        break;
    case Phase::AwaitFoes: {
        // The screen stays dark (the UI covers it) until the whole fight is in place, then it all appears at once.
        // Meanwhile Donald and Goofy are frozen, so their own AI can't start on the enemies appearing.
        const BattleMap& bm = battleMaps[g_arena];
        if (game::SafeGameplay()) SetGroups(1u << groupParty);
        if (game::SafeGameplay() && g_foesSince < 0) {
            // The battle map's enemies ambush whoever stands in their spawn area: stand the player there.
            for (auto& x : actors) if (x.type == 0) game::SetPos(x.actor, bm.triggerX, bm.triggerY, bm.triggerZ);
        }
        bool foes = false;
        for (auto& a : actors) if (IsFoe(a)) { foes = true; break; }
        if (foes && game::SafeGameplay()) {
            if (g_foesSince < 0) g_foesSince = now;
            if (now - g_foesSince >= 0.8) { // the rest of the group appears within a moment
                RefreshUnits(actors);
                testmode::Pass("encounter");
                BeginBattle();
            }
        } else if (now - g_phaseStart > 8.0) {
            Log("no foes appeared on battle map %s; returning", bm.key);
            StartReturn("no foes");
        }
        break; }
    case Phase::Returning: {
        g_pool.clear();
        Location cur = game::Now();
        bool there = cur.world == g_returnLoc.world && cur.room == g_returnLoc.room && cur.btl == g_returnLoc.btl;
        if (now - g_phaseStart > loadTimeoutSec) { Log("return timed out; back to normal play"); g_encounter = false; EndBattle(); SetPhase(Phase::Field); break; }
        if ((g_loadLeft && game::InField()) || (there && now - g_phaseStart > 1.0 && game::SafeGameplay())) {
            Location l = game::Now();
            if (g_returnPosValid && l.world == g_returnLoc.world && l.room == g_returnLoc.room)
                for (auto& x : actors) if (x.type == 0) game::SetPos(x.actor, g_returnPos[0], g_returnPos[1], g_returnPos[2]);
            Log("back in the field %02u/%02u btl=%u", l.world, l.room, l.btl);
            testmode::Pass("returned-to-field");
            g_encounter = false;
            EndBattle();
            SetPhase(Phase::Field);
        }
        break; }
    default:
        if (g_active) TickBattle(actors, now);
        else SetPhase(Phase::Field);
        break;
    }

    UpdateStage();
    char buf[160];
    Location l = game::Now();
    snprintf(buf, sizeof buf, "%02u/%02u btl=%u phase=%d units=%zu meter=%.0f/%.0f wins=%d", l.world, l.room, l.btl, (int)g_phase, g_units.size(), g_meter, g_meterGoal, g_wins);
    g_dbg = buf;
    static double lastState = 0;
    if (testmode::Active() && now - lastState > 1.0) {
        lastState = now;
        int partners = 0; for (auto& u : g_units) if (u.side == Side::Partner) ++partners;
        char js[512];
        snprintf(js, sizeof js, "{\"world\":%u,\"room\":%u,\"btl\":%u,\"phase\":%d,\"inBattle\":%d,\"encounter\":%d,\"enemies\":%d,\"partners\":%d,\"soraHp\":%d,\"soraMp\":%d,\"meter\":%.0f,\"partyTurns\":%d,\"enemyTurns\":%d,\"wins\":%d,\"inField\":%d,\"cutscene\":%d}",
                 l.world, l.room, l.btl, (int)g_phase, g_active ? 1 : 0, g_encounter ? 1 : 0, EnemyCount(), partners, game::RoxasHp(), game::SoraMp(), g_meter, g_partyTurns, g_enemyTurns, g_wins, game::InField() ? 1 : 0, game::CutsceneOrEvent() ? 1 : 0);
        testmode::WriteState(js);
    }
}

void GetSnapshot(UiSnapshot& s) {
    std::lock_guard<std::mutex> lk(g_lock);
    s = UiSnapshot{};
    s.battle = g_active || g_phase == Phase::Shatter || g_phase == Phase::LoadingArena || g_phase == Phase::AwaitFoes || g_phase == Phase::Returning;
    s.encounter = g_encounter; s.boss = g_boss;
    s.phase = g_phase; s.phaseStart = g_phaseStart;
    s.banner = g_banner;
    s.info = g_info; s.infoTime = g_infoTime;
    s.shatterStart = g_shatterStart;
    s.revealAt = g_revealAt;
    s.wave = g_wave; s.waveAt = g_waveAt;
    s.wavesKnown = 1;
    s.soraMp = game::SoraMp(); s.soraMaxMp = game::SoraMaxMp();
    s.testMode = testmode::Active();
    s.kh2Menu = g_kh2Menu;
    {
        double tnow = NowSec();
        g_popups.erase(std::remove_if(g_popups.begin(), g_popups.end(), [&](const HealPopup& p) { return tnow - p.at > 1.8; }), g_popups.end());
        for (auto& p : g_popups) for (int i = 0; i < (int)g_units.size(); ++i) if (g_units[i].actor == p.actor) s.popups.push_back({i, p.text, p.at, p.color});
    }
    for (auto& m : g_slots) {
        std::string nm = m;
        for (auto& u : g_units) if (u.side == Side::Partner && u.model.rfind(m, 0) == 0) nm = u.name;
        s.friends.push_back(nm);
    }
    s.debug = g_dbg;
    s.current = g_current;
    s.actionText = g_action;
    for (int i = 0; i < (int)g_units.size(); ++i) {
        const auto& u = g_units[i];
        UiUnit x; x.name = u.name; x.side = u.side; x.current = i == g_current; x.ko = u.ko(); x.defending = u.defending;
        x.targeted = (g_phase == Phase::Act || g_phase == Phase::EnemyAct) && i == g_act.target;
        x.hp = u.hp; x.maxHp = u.maxHp;
        if (u.side == Side::Sora) { x.mp = game::SoraMp(); x.maxMp = game::SoraMaxMp(); }
        else if (u.side == Side::Partner && u.status) {
            // A partner's MP, kept as KH2 keeps Sora's (status +0x180, its max +0x184).
            x.mp = Rd<int32_t>(u.status + 0x180); x.maxMp = Rd<int32_t>(u.status + 0x184);
            if (x.maxMp < x.mp || x.maxMp > 999) x.maxMp = std::max(x.mp, 100);
        }
        x.wx = u.x; x.wy = u.y; x.wz = u.z;
        x.height = u.side == Side::Partner ? 150.f : (u.type == 3 ? 400.f : 175.f);
        s.units.push_back(x);
    }
    Preview(s.ctbOrderUnit);
    bool menu = g_phase == Phase::PlayerMenu && g_current >= 0 && g_current < (int)g_units.size() && g_units[g_current].party();
    s.menuOpen = menu;
    if (menu) {
        const auto& me = g_units[g_current];
        bool sora = me.side == Side::Sora;
        if (!sora) {
            for (int i = 0; i < kMoves; ++i) if (MoveFor(i, me.model)) {
                const PartnerMove& m = partnerMoves[i];
                int st = AbilityState(m.abilityList, m.abilityId);
                if (st == 0) continue; // not learned yet
                bool usable = st == 2 && (!HealMove(i) || AnyoneHurt());
                std::string desc = st == 2 ? std::string(m.description) : std::string("Equip ") + m.name + " in the pause menu (Abilities) to use it in battle.";
                (m.technique ? s.techs : s.magic).push_back({i, m.name, desc, -1, m.targetEnemy, usable, true});
            }
        }
        bool anyMagic = !s.magic.empty();
        for (int i = 0; i < kMagic && sora; ++i) if (MagicCmd(i)) anyMagic = true;
        for (auto& c : commands) {
            if (!strcmp(c.who, sora ? "partner" : "sora")) continue;
            bool en = !c.encounterOnly || g_encounter;
            if (!strcmp(c.kind, "submenu_magic")) { if (!anyMagic) continue; }
            if (!strcmp(c.kind, "submenu_tech") && s.techs.empty()) continue;
            if (!strcmp(c.kind, "submenu_items") && !sora && ItemSlots(CharBlock(me)).empty()) continue;
            s.commands.push_back({c.key, c.label, c.kind, c.description, en});
        }
        if (sora) {
            for (int i = 0; i < kMagic; ++i) {
                int l = MagicLevel(i); if (!l) continue;
                s.magic.push_back({i, magic[i].names[l - 1], magic[i].description, MagicCost(i), magic[i].targetEnemy, MagicUsable(i)});
            }
        }
        for (auto& si : ItemSlots(CharBlock(me))) {
            int n = game::InventoryCount(items[si.row].saveOffset);
            s.items.push_back({si.row, items[si.row].name, items[si.row].description, n, si.pos, items[si.row].party});
        }
    }
}

void PostCommand(const BattleCommand& c) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_cmds.push_back(c);
}

}
