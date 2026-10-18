#pragma once
#include "common.h"

enum class Phase { Field, Shatter, LoadingArena, AwaitFoes, TurnIntro, PlayerMenu, Act, EnemyAct, Settle, Victory, Returning, GameOver, Hold, Regroup, PartyMenu };
enum class Side { Sora, Partner, Enemy };

struct UiUnit {
    std::string name;
    Side side = Side::Enemy;
    bool current = false, ko = false, defending = false, targeted = false;
    int hp = 0, maxHp = 0, mp = 0, maxMp = 0;
    float wx = 0, wy = 0, wz = 0;   // feet
    float height = 170;             // marker height above the feet
};
struct UiMagic { int sheetIndex; std::string name, description; int mp; bool enemyTarget; bool usable; bool partnerMove = false; };
struct UiItem { int sheetIndex; std::string name, description; int count; int slot = 0; bool party = false; };
struct UiCommand { std::string key, label, kind, description; bool enabled; };

struct UiSnapshot {
    bool battle = false, encounter = false, boss = false;
    Phase phase = Phase::Field;
    double phaseStart = 0;
    std::string banner;            // "Victory!", ...
    int current = -1;              // index into units of whoever's turn it is
    std::string actionText;        // what the current battler is doing ("Fire", "Attack")
    std::vector<UiUnit> units;
    std::vector<int> ctbOrderUnit; // upcoming turns, indexes into units
    int soraMp = 0, soraMaxMp = 0;
    std::vector<UiCommand> commands; // the current party member's menu
    std::vector<UiMagic> magic;      // the acting character's Magic list
    std::vector<UiMagic> techs;      // a partner's Techniques list
    std::vector<UiItem> items;
    std::vector<std::string> friends; // the partners KH2's Party command lists, in its order
    struct Popup { int unit; std::string text; double at; uint32_t color; };
    std::vector<Popup> popups;        // heals the mod applied itself: a glow and a number over who got them
    double shatterStart = -1;
    double revealAt = -1;
    int wave = 1, wavesKnown = 1;  // enemy waves: this one, and how many the battle map is known to have
    double waveAt = -1;            // a new wave just arrived          // a battle map's fight has just appeared: fade in from black
    bool menuOpen = false;         // a party member is choosing a command
    std::string info;              // one-line status
    double infoTime = 0;
    bool testMode = false;
    bool kh2Menu = false;           // KH2's own command menu is in use: leave its corner uncovered
    std::string debug;
};

enum class CmdKind { Attack, Magic, Item, Defend, Escape, Party, Move };  // Move: a partner's own magic or technique
struct BattleCommand { CmdKind kind; int index; int target; int on = -1; }; // on: the party member an item is used on (unit index)

namespace battle {
void Init();
void Tick();                         // game thread, once per KH2 frame
void GetSnapshot(UiSnapshot& out);   // render thread
void PostCommand(const BattleCommand& c);
}
