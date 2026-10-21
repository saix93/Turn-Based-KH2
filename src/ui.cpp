// Battle UI drawn over KH2, in KH2's own look (dark glass panels, silver frames, blue selection):
// whose turn it is (banner, marker over the battler, turn order), the command window over KH2's
// command menu, the party's HP/MP over KH2's gauges, the enemy list, and the encounter screen shatter.
#include "ui.h"
#include "battle.h"
#include "game.h"
#include "gen/sheets.h"
#include "imgui.h"
#include <Xinput.h>
#include <cmath>
#include <random>

using namespace sheets;

namespace {
ImFont* g_font = nullptr;
UiSnapshot S;

// ---- input (our own, so the menu works with keyboard and controller) ----
struct Pad { bool up, down, left, right, ok, back; };
Pad g_prev{}, g_now{};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn pXInput = nullptr;
bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
void ReadInput(HWND hwnd) {
    g_prev = g_now; g_now = {};
    if (GetForegroundWindow() != hwnd) return;
    g_now.up = Key(VK_UP) || Key('W'); g_now.down = Key(VK_DOWN) || Key('S');
    g_now.left = Key(VK_LEFT) || Key('A'); g_now.right = Key(VK_RIGHT) || Key('D');
    g_now.ok = Key(VK_RETURN) || Key(VK_SPACE) || Key('E');
    g_now.back = Key(VK_BACK) || Key('Q') || Key(VK_ESCAPE);
    if (!pXInput) { HMODULE m = LoadLibraryW(L"xinput1_4.dll"); if (m) pXInput = (XInputGetStateFn)GetProcAddress(m, "XInputGetState"); }
    if (pXInput) {
        for (DWORD i = 0; i < 4; ++i) {
            XINPUT_STATE st{}; if (pXInput(i, &st) != ERROR_SUCCESS) continue;
            WORD b = st.Gamepad.wButtons; SHORT ly = st.Gamepad.sThumbLY, lx = st.Gamepad.sThumbLX;
            g_now.up |= (b & XINPUT_GAMEPAD_DPAD_UP) || ly > 20000; g_now.down |= (b & XINPUT_GAMEPAD_DPAD_DOWN) || ly < -20000;
            g_now.left |= (b & XINPUT_GAMEPAD_DPAD_LEFT) || lx < -20000; g_now.right |= (b & XINPUT_GAMEPAD_DPAD_RIGHT) || lx > 20000;
            g_now.ok |= (b & XINPUT_GAMEPAD_A) != 0; g_now.back |= (b & XINPUT_GAMEPAD_B) != 0;
        }
    }
}
#define PRESSED(f) (g_now.f && !g_prev.f)

// ---- menu state ----
enum class Menu { Root, Magic, Techs, Items, ItemTarget, Party, Target } g_menu = Menu::Root;
int g_partyCur = 0, g_itemOnCur = 0;
// The party members an item can be used on (unit indices into S.units).
std::vector<int> ItemTargets() { std::vector<int> v; for (int i = 0; i < (int)S.units.size(); ++i) if (S.units[i].side != Side::Enemy && !S.units[i].ko) v.push_back(i); return v; }
int g_cur = 0, g_magicCur = 0, g_itemCur = 0, g_targetCur = 0;
Menu g_targetBack = Menu::Root;
CmdKind g_targetFor = CmdKind::Attack; int g_targetIndex = -1;
double g_menuOpenedAt = 0; int g_menuOwner = -2;

// ---- look ----
ImU32 Col(uint32_t rgb, float a = 1.f) { return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, (int)(std::clamp(a, 0.f, 1.f) * 255)); }
constexpr uint32_t kParty = 0x5AB4FF, kEnemy = 0xFF5A5A, kGold = 0xFFD44A, kText = 0xF2F6FF, kDim = 0x8A93AA;
uint32_t SideColor(Side s) { return s == Side::Enemy ? kEnemy : kParty; }
float g_sc = 1.f;
float Px(float v) { return v * g_sc; }

void Text(ImDrawList* dl, float size, ImVec2 p, ImU32 c, const std::string& s) {
    dl->AddText(g_font, size, ImVec2(p.x + 1.5f, p.y + 1.5f), IM_COL32(0, 0, 0, 210), s.c_str());
    dl->AddText(g_font, size, p, c, s.c_str());
}
ImVec2 Measure(float size, const std::string& s) { return g_font->CalcTextSizeA(size, FLT_MAX, 0, s.c_str()); }
// KH2 panel: dark navy glass, silver frame, a lighter sheen along the top.
void Panel(ImDrawList* dl, ImVec2 a, ImVec2 b, float alpha = 0.95f, uint32_t accent = 0) {
    float r = Px(8);
    dl->AddRectFilled(a, b, Col(0x0B1226, alpha), r);
    dl->AddRectFilledMultiColor(ImVec2(a.x + 2, a.y + 2), ImVec2(b.x - 2, a.y + (b.y - a.y) * 0.45f), Col(0x2A3C6E, 0.55f * alpha), Col(0x2A3C6E, 0.55f * alpha), Col(0x0B1226, 0), Col(0x0B1226, 0));
    dl->AddRect(a, b, Col(0xC9D4EA, 0.9f * alpha), r, 0, Px(2));
    if (accent) dl->AddRectFilled(ImVec2(a.x + Px(3), a.y + Px(3)), ImVec2(a.x + Px(7), b.y - Px(3)), Col(accent, alpha), Px(2));
}
// A tab sitting on a panel's top edge, like KH2's "COMMANDS" plate.
void Tab(ImDrawList* dl, ImVec2 at, const std::string& label, uint32_t color) {
    float size = Px(22);
    ImVec2 ts = Measure(size, label);
    ImVec2 a(at.x, at.y - Px(16)), b(at.x + ts.x + Px(30), at.y + Px(16));
    dl->AddRectFilledMultiColor(a, b, Col(color, 0.95f), Col(color, 0.65f), Col(0x10204A, 0.95f), Col(0x10204A, 0.95f));
    dl->AddRect(a, b, Col(0xFFFFFF, 0.9f), Px(6), 0, Px(2));
    Text(dl, size, ImVec2(a.x + Px(15), a.y + (b.y - a.y - ts.y) / 2), Col(0xFFFFFF), label);
}
// Selected row: KH2's blue bar with a bright left edge.
void Selection(ImDrawList* dl, ImVec2 a, ImVec2 b, double now) {
    float pulse = 0.75f + 0.25f * (float)std::sin(now * 6.0);
    dl->AddRectFilledMultiColor(a, b, Col(0x3C8CFF, 0.95f * pulse), Col(0x1A3C9A, 0.5f), Col(0x1A3C9A, 0.5f), Col(0x3C8CFF, 0.95f * pulse));
    dl->AddRectFilled(a, ImVec2(a.x + Px(4), b.y), Col(0xFFFFFF, 0.95f));
}
void Bar(ImDrawList* dl, ImVec2 a, float w, float h, float f, uint32_t fill) {
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), Col(0x050810, 0.9f), h * 0.5f);
    if (f > 0) dl->AddRectFilled(a, ImVec2(a.x + w * std::clamp(f, 0.f, 1.f), a.y + h), Col(fill), h * 0.5f);
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), Col(0xC9D4EA, 0.6f), h * 0.5f, 0, 1.f);
}

// Screen areas: these two windows sit exactly over KH2's command menu and HP gauge, opaque, so
// KH2's own battle HUD stays hidden behind them.
// Over KH2's command menu and HP gauge; in test runs they sit higher, so recordings show KH2's own menu.
// Test runs raise the mod's windows (out of the way of KH2's own menu in the recordings) and show a debug line;
// off for a build that takes listing screenshots, so they show the layout players get.
constexpr bool kTestLayout = true;
float TestLift() { return S.testMode && kTestLayout ? 0.33f : 0.f; }
ImVec2 CmdA(float sw, float sh) { return ImVec2(sw * 0.012f, sh * (0.665f - TestLift())); }
ImVec2 CmdB(float sw, float sh) { return ImVec2(sw * 0.262f, sh * (0.985f - TestLift())); }
ImVec2 PartyA(float sw, float sh) { return ImVec2(sw * 0.67f, sh * (0.735f - TestLift())); }
ImVec2 PartyB(float sw, float sh) { return ImVec2(sw * 0.988f, sh * (0.985f - TestLift())); }

bool Project(const UiUnit& u, float above, float sw, float sh, ImVec2& out) {
    float x, y;
    if (!game::WorldToScreen(u.wx, u.wy - above, u.wz, sw, sh, x, y)) return false;
    out = ImVec2(x, y); return true;
}
std::vector<int> EnemyUnitIndices() { std::vector<int> v; for (int i = 0; i < (int)S.units.size(); ++i) if (S.units[i].side == Side::Enemy) v.push_back(i); return v; }
int AimedUnit() {
    if (!(S.menuOpen && g_menu == Menu::Target)) return -1;
    auto en = EnemyUnitIndices();
    return g_targetCur < (int)en.size() ? en[g_targetCur] : -1;
}

// ---- the encounter: the screen cracks into shards that spin away into darkness ----
void DrawShatter(ImDrawList* dl, float sw, float sh, double now) {
    if (S.shatterStart < 0) return;
    float t = (float)((now - S.shatterStart) / shatterSec);
    if (t < 0) return;
    float u = std::min(t, 1.f);
    dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, (int)(255 * std::min(1.f, u * 1.4f))));
    const int gx = 8, gy = 5;
    std::mt19937 r(7); std::uniform_real_distribution<float> U(-1, 1);
    for (int y = 0; y < gy; ++y) for (int x = 0; x < gx; ++x) for (int k = 0; k < 2; ++k) {
        float x0 = sw * x / gx, y0 = sh * y / gy, x1 = sw * (x + 1) / gx, y1 = sh * (y + 1) / gy;
        ImVec2 q[3] = {k ? ImVec2{x1, y1} : ImVec2{x0, y0}, ImVec2{x1, y0}, ImVec2{x0, y1}};
        float cx = (q[0].x + q[1].x + q[2].x) / 3, cy = (q[0].y + q[1].y + q[2].y) / 3;
        float dx = (cx - sw / 2) / sw, dy = (cy - sh / 2) / sh;
        float push = u * u * 1.6f, rot = U(r) * 3.f * u;
        float ox = dx * sw * push + U(r) * 40 * u, oy = dy * sh * push + sh * 0.4f * u * u;
        float cs = std::cos(rot), sn = std::sin(rot);
        ImVec2 w[3];
        for (int i = 0; i < 3; ++i) { float lx = q[i].x - cx, ly = q[i].y - cy; w[i] = ImVec2(cx + ox + lx * cs - ly * sn, cy + oy + lx * sn + ly * cs); }
        float a = 1.f - u;
        dl->AddTriangleFilled(w[0], w[1], w[2], IM_COL32(150, 190, 255, (int)(70 * a)));
        dl->AddTriangle(w[0], w[1], w[2], IM_COL32(230, 240, 255, (int)(230 * a)), 2.f);
    }
    if (t < 0.15f) dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(255, 255, 255, (int)(200 * (1 - t / 0.15f))));
}

// ---- whose turn ----
// Over the acting battler: a bobbing arrow in its side's colour, with its name. Over the target: a
// turning gold diamond.
// A heal the mod applied itself: a green ring at the feet and the amount rising over the head, fading out.
void DrawPopups(ImDrawList* dl, float sw, float sh, double now) {
    for (auto& p : S.popups) {
        double t = now - p.at;
        if (t < 0 || t > 1.8 || p.unit < 0 || p.unit >= (int)S.units.size()) continue;
        const UiUnit& u = S.units[p.unit];
        float a = (float)std::min(1.0, std::min(t * 6.0, (1.8 - t) * 2.5));
        ImVec2 feet, head;
        if (Project(u, 0.f, sw, sh, feet)) {
            float r = Px(26) + Px(40) * (float)std::min(1.0, t / 0.6);
            dl->AddEllipse(feet, ImVec2(r, r * 0.35f), Col(p.color, 0.8f * a), 0.f, 32, Px(3));
            dl->AddEllipseFilled(feet, ImVec2(r * 0.8f, r * 0.28f), Col(p.color, 0.18f * a));
        }
        if (Project(u, u.height + 20.f, sw, sh, head)) {
            float size = Px(30);
            ImVec2 ts = Measure(size, p.text);
            ImVec2 at(head.x - ts.x / 2, head.y - Px(40) * (float)std::min(1.0, t / 1.2) - ts.y);
            Text(dl, size, ImVec2(at.x + Px(2), at.y + Px(2)), Col(0x000000, 0.7f * a), p.text);
            Text(dl, size, at, Col(p.color, a), p.text);
        }
    }
}

void DrawMarkers(ImDrawList* dl, float sw, float sh, double now) {
    int aimed = AimedUnit();
    for (int i = 0; i < (int)S.units.size(); ++i) {
        const UiUnit& u = S.units[i];
        ImVec2 p;
        if (u.current && !u.ko && Project(u, u.height, sw, sh, p)) {
            float bob = (float)std::sin(now * 5.0) * Px(6);
            float s = Px(18);
            ImVec2 tip(p.x, p.y - Px(8) + bob);
            uint32_t c = SideColor(u.side);
            dl->AddTriangleFilled(ImVec2(tip.x - s, tip.y - s * 1.3f), ImVec2(tip.x + s, tip.y - s * 1.3f), tip, Col(c));
            dl->AddTriangle(ImVec2(tip.x - s, tip.y - s * 1.3f), ImVec2(tip.x + s, tip.y - s * 1.3f), tip, Col(0xFFFFFF), Px(2.5f));
            float size = Px(22);
            ImVec2 ts = Measure(size, u.name);
            ImVec2 a(tip.x - ts.x / 2 - Px(10), tip.y - s * 1.3f - ts.y - Px(12)), b(tip.x + ts.x / 2 + Px(10), tip.y - s * 1.3f - Px(4));
            dl->AddRectFilled(a, b, Col(0x0B1226, 0.85f), Px(6));
            dl->AddRect(a, b, Col(c), Px(6), 0, Px(2));
            Text(dl, size, ImVec2(a.x + Px(10), a.y + Px(4)), Col(kText), u.name);
        }
        if ((u.targeted || i == aimed) && !u.current && Project(u, u.height + 20, sw, sh, p)) {
            float a = (float)now * 3.f, r = Px(16);
            ImVec2 q[4];
            for (int k = 0; k < 4; ++k) q[k] = ImVec2(p.x + std::cos(a + k * 1.5708f) * r, p.y + std::sin(a + k * 1.5708f) * r * 0.5f);
            dl->AddQuadFilled(q[0], q[1], q[2], q[3], Col(kGold, 0.85f));
            dl->AddQuad(q[0], q[1], q[2], q[3], Col(kEnemy), Px(2.5f));
        }
    }
}

// Top centre: whose turn it is, and the move being made.
void DrawTurnBanner(ImDrawList* dl, float sw, float sh, double now) {
    if (S.current < 0 || S.current >= (int)S.units.size()) return;
    const UiUnit& u = S.units[S.current];
    uint32_t c = SideColor(u.side);
    bool intro = S.phase == Phase::TurnIntro;
    float k = intro ? std::min(1.f, (float)(now - S.phaseStart) * 6.f) : 1.f;
    std::string who = u.name;
    for (auto& ch : who) ch = (char)toupper((unsigned char)ch);
    std::string title = who + (u.side == Side::Enemy ? "'S ATTACK" : "'S TURN");
    float size = Px(intro ? 40 : 30);
    ImVec2 ts = Measure(size, title);
    float w = ts.x + Px(80), h = ts.y + Px(18);
    ImVec2 a((sw - w) / 2, Px(18)), b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, Col(0x0B1226, 0.9f * k), h / 2);
    dl->AddRect(a, b, Col(c, k), h / 2, 0, Px(3));
    Text(dl, size, ImVec2(a.x + (w - ts.x) / 2, a.y + (h - ts.y) / 2), Col(u.side == Side::Enemy ? 0xFFD0D0 : 0xE6F2FF, k), title);
    if ((S.phase == Phase::Act || S.phase == Phase::EnemyAct) && !S.actionText.empty()) {
        float s2 = Px(26);
        ImVec2 t2 = Measure(s2, S.actionText);
        ImVec2 c0((sw - t2.x) / 2 - Px(22), b.y + Px(8)), c1((sw + t2.x) / 2 + Px(22), b.y + Px(8) + t2.y + Px(10));
        Panel(dl, c0, c1, 0.9f);
        Text(dl, s2, ImVec2(c0.x + Px(22), c0.y + Px(5)), Col(kGold), S.actionText);
    }
}

void DrawHelp(ImDrawList* dl, float sw, float sh, const std::string& help) {
    if (help.empty()) return;
    float size = Px(22);
    ImVec2 ts = Measure(size, help);
    float top = Px(84);
    ImVec2 a((sw - ts.x) / 2 - Px(20), top), b((sw + ts.x) / 2 + Px(20), top + ts.y + Px(12));
    Panel(dl, a, b, 0.88f);
    Text(dl, size, ImVec2(a.x + Px(20), a.y + Px(6)), Col(kText), help);
}

// Right side: who acts next. The one acting now is bigger, gold-framed and labelled NOW.
void DrawTurnOrder(ImDrawList* dl, float sw, float sh) {
    if (S.ctbOrderUnit.empty()) return;
    float w = Px(220), x1 = sw - Px(18), x0 = x1 - w, y = Px(96);
    Text(dl, Px(18), ImVec2(x0 + Px(4), y - Px(26)), Col(0xC9D4EA), "TURN ORDER");
    if (S.wave > 1 || S.wavesKnown > 1) {
        // Enemy waves: which one this is (and how many the battle map is known to bring).
        std::string w = "WAVE " + std::to_string(S.wave) + (S.wavesKnown > 1 && S.wave <= S.wavesKnown ? " / " + std::to_string(S.wavesKnown) : "");
        ImVec2 ts = Measure(Px(18), w);
        Text(dl, Px(18), ImVec2(x1 - ts.x - Px(4), y - Px(26)), Col(kGold), w);
    }
    for (size_t i = 0; i < S.ctbOrderUnit.size(); ++i) {
        int ui = S.ctbOrderUnit[i];
        if (ui < 0 || ui >= (int)S.units.size()) continue;
        const UiUnit& u = S.units[ui];
        bool now = i == 0;
        float h = Px(now ? 40 : 28);
        ImVec2 a(now ? x0 - Px(24) : x0, y), b(x1, y + h);
        Panel(dl, a, b, now ? 0.97f : 0.82f, SideColor(u.side));
        if (now) dl->AddRect(a, b, Col(kGold), Px(8), 0, Px(2.5f));
        float size = Px(now ? 24 : 19);
        Text(dl, size, ImVec2(a.x + Px(16), a.y + (h - size) / 2 - Px(1)), Col(now ? kGold : kText), u.name);
        if (now) { ImVec2 ts = Measure(Px(16), "NOW"); Text(dl, Px(16), ImVec2(b.x - ts.x - Px(10), a.y + (h - ts.y) / 2), Col(0xFFFFFF), "NOW"); }
        y += h + Px(5);
    }
}

// Bottom right, over KH2's gauges: every party member's HP and MP; whoever acts is lit up.
void DrawParty(ImDrawList* dl, float sw, float sh, double now) {
    ImVec2 a = PartyA(sw, sh), b = PartyB(sw, sh);
    Panel(dl, a, b, 1.f);
    Tab(dl, ImVec2(a.x + Px(14), a.y), "PARTY", kParty);
    std::vector<const UiUnit*> party;
    for (auto& u : S.units) if (u.side == Side::Sora) party.push_back(&u);
    for (auto& u : S.units) if (u.side == Side::Partner) party.push_back(&u);
    float top = a.y + Px(24), rowH = std::min(Px(62), (b.y - top - Px(8)) / std::max<size_t>(1, party.size()));
    for (size_t i = 0; i < party.size(); ++i) {
        const UiUnit& u = *party[i];
        ImVec2 r0(a.x + Px(10), top + i * rowH), r1(b.x - Px(10), top + (i + 1) * rowH - Px(4));
        if (u.current) Selection(dl, r0, r1, now);
        std::string label = u.name + (u.ko ? "  KO" : "") + (u.defending ? "  Guard" : "");
        Text(dl, Px(22), ImVec2(r0.x + Px(14), r0.y + Px(4)), Col(u.ko ? kDim : (u.current ? 0xFFFFFF : kText)), label);
        float bx = r0.x + (r1.x - r0.x) * 0.42f, bw = (r1.x - bx) - Px(84);
        float f = u.maxHp > 0 ? (float)u.hp / u.maxHp : 0;
        Bar(dl, ImVec2(bx, r0.y + Px(8)), bw, Px(10), f, f <= 0.25f ? 0xFFB040 : 0x6CE07A);
        char hp[32]; snprintf(hp, sizeof hp, "%d HP", std::max(0, u.hp));
        Text(dl, Px(18), ImVec2(bx + bw + Px(10), r0.y + Px(2)), Col(kText), hp);
        if (u.maxMp > 0) {
            Bar(dl, ImVec2(bx, r0.y + Px(30)), bw, Px(8), u.ko ? 0.f : (float)u.mp / u.maxMp, 0x4A9CFF);
            char mp[32]; snprintf(mp, sizeof mp, "%d MP", std::max(0, u.mp));
            Text(dl, Px(17), ImVec2(bx + bw + Px(10), r0.y + Px(24)), Col(0xA8CCFF), mp);
        }
    }
}

// Left of the party window: the enemies, with HP bars as KH2 shows them on lock-on.
void DrawEnemies(ImDrawList* dl, float sw, float sh, double now) {
    auto en = EnemyUnitIndices();
    if (en.empty()) return;
    int aimed = AimedUnit();
    float rowH = Px(32), w = Px(270);
    ImVec2 b(PartyA(sw, sh).x - Px(14), PartyB(sw, sh).y), a(b.x - w, b.y - Px(28) - rowH * en.size());
    Panel(dl, a, b, 0.95f);
    Tab(dl, ImVec2(a.x + Px(14), a.y), "ENEMIES", kEnemy);
    for (size_t i = 0; i < en.size(); ++i) {
        const UiUnit& u = S.units[en[i]];
        ImVec2 r0(a.x + Px(8), a.y + Px(22) + i * rowH), r1(b.x - Px(8), r0.y + rowH - Px(4));
        if (en[i] == aimed || u.targeted) Selection(dl, r0, r1, now);
        Text(dl, Px(19), ImVec2(r0.x + Px(12), r0.y + Px(3)), Col(u.current ? kGold : kText), u.name);
        float f = u.maxHp > 0 ? (float)u.hp / u.maxHp : 0;
        Bar(dl, ImVec2(r0.x + (r1.x - r0.x) * 0.55f, r0.y + Px(10)), (r1.x - r0.x) * 0.42f, Px(8), f, 0xE05050);
    }
}

// Bottom left, over KH2's command menu: the acting party member's commands.
void DrawCommands(ImDrawList* dl, float sw, float sh, double now, std::string& help) {
    ImVec2 a = CmdA(sw, sh), b = CmdB(sw, sh);
    Panel(dl, a, b, 1.f);
    const UiUnit* cur = (S.current >= 0 && S.current < (int)S.units.size()) ? &S.units[S.current] : nullptr;
    if (!S.menuOpen) {
        g_menuOwner = -2;
        Tab(dl, ImVec2(a.x + Px(14), a.y), "COMMANDS", kParty);
        if (cur && S.phase != Phase::Victory) {
            std::string s = cur->side == Side::Enemy ? cur->name + " is attacking" : cur->name + (S.actionText.empty() ? "" : ": " + S.actionText);
            Text(dl, Px(22), ImVec2(a.x + Px(22), a.y + Px(34)), Col(cur->side == Side::Enemy ? 0xFFB0B0 : kDim), s);
        }
        return;
    }
    if (g_menuOwner != S.current) { g_menuOwner = S.current; g_menu = Menu::Root; g_cur = 0; g_menuOpenedAt = now; }
    std::string who = cur ? cur->name : "";
    for (auto& ch : who) ch = (char)toupper((unsigned char)ch);
    Tab(dl, ImVec2(a.x + Px(14), a.y), who, kParty);

    auto en = EnemyUnitIndices();
    int n = (int)S.commands.size();
    if (n == 0) return;
    g_cur = std::clamp(g_cur, 0, n - 1);
    bool armed = now - g_menuOpenedAt > 0.25; // ignore the press that ended the previous turn
    if (armed) {
        if (g_menu == Menu::Root) {
            if (PRESSED(up)) g_cur = (g_cur + n - 1) % n;
            if (PRESSED(down)) g_cur = (g_cur + 1) % n;
            if (PRESSED(ok) && S.commands[g_cur].enabled) {
                const std::string& k = S.commands[g_cur].kind;
                if (k == "attack") { if (en.empty()) battle::PostCommand({CmdKind::Attack, 0, -1}); else { g_targetFor = CmdKind::Attack; g_targetIndex = -1; g_targetBack = Menu::Root; g_menu = Menu::Target; g_targetCur = 0; } }
                else if (k == "submenu_magic") { g_menu = Menu::Magic; g_magicCur = 0; }
                else if (k == "submenu_tech") { g_menu = Menu::Techs; g_magicCur = 0; }
                else if (k == "submenu_items") { g_menu = Menu::Items; g_itemCur = 0; }
                else if (k == "defend") battle::PostCommand({CmdKind::Defend, 0, -1});
                else if (k == "party") { g_menu = Menu::Party; g_partyCur = 0; }
                else if (k == "escape") battle::PostCommand({CmdKind::Escape, 0, -1});
            }
        } else if (g_menu == Menu::Magic || g_menu == Menu::Techs) {
            const auto& list = g_menu == Menu::Magic ? S.magic : S.techs;
            int m = (int)list.size();
            if (m) {
                if (PRESSED(up)) g_magicCur = (g_magicCur + m - 1) % m;
                if (PRESSED(down)) g_magicCur = (g_magicCur + 1) % m;
                g_magicCur = std::min(g_magicCur, m - 1);
                const UiMagic& sel = list[g_magicCur];
                CmdKind kind = sel.partnerMove ? CmdKind::Move : CmdKind::Magic;
                if (PRESSED(ok) && sel.usable) {
                    if (sel.enemyTarget && !en.empty()) { g_targetFor = kind; g_targetIndex = sel.sheetIndex; g_targetBack = g_menu; g_menu = Menu::Target; g_targetCur = 0; }
                    else battle::PostCommand({kind, sel.sheetIndex, -1});
                }
            }
            if (PRESSED(back)) g_menu = Menu::Root;
        } else if (g_menu == Menu::Items) {
            int m = (int)S.items.size();
            if (m) {
                if (PRESSED(up)) g_itemCur = (g_itemCur + m - 1) % m;
                if (PRESSED(down)) g_itemCur = (g_itemCur + 1) % m;
                g_itemCur = std::min(g_itemCur, m - 1);
                if (PRESSED(ok) && S.items[g_itemCur].count > 0) {
                    // A party item is for everyone; any other asks who it is for.
                    if (S.items[g_itemCur].party) battle::PostCommand({CmdKind::Item, S.items[g_itemCur].sheetIndex, S.items[g_itemCur].slot});
                    else { g_menu = Menu::ItemTarget; g_itemOnCur = 0; }
                }
            }
            if (PRESSED(back)) g_menu = Menu::Root;
        } else if (g_menu == Menu::ItemTarget) {
            auto on = ItemTargets();
            int m = (int)on.size();
            if (m && g_itemCur < (int)S.items.size()) {
                if (PRESSED(up)) g_itemOnCur = (g_itemOnCur + m - 1) % m;
                if (PRESSED(down)) g_itemOnCur = (g_itemOnCur + 1) % m;
                g_itemOnCur = std::min(g_itemOnCur, m - 1);
                if (PRESSED(ok)) battle::PostCommand({CmdKind::Item, S.items[g_itemCur].sheetIndex, S.items[g_itemCur].slot, on[g_itemOnCur]});
            }
            if (PRESSED(back)) g_menu = Menu::Items;
        } else if (g_menu == Menu::Party) {
            int m = (int)S.friends.size();
            if (m) {
                if (PRESSED(up)) g_partyCur = (g_partyCur + m - 1) % m;
                if (PRESSED(down)) g_partyCur = (g_partyCur + 1) % m;
                g_partyCur = std::min(g_partyCur, m - 1);
                if (PRESSED(ok)) battle::PostCommand({CmdKind::Party, g_partyCur, -1});
            }
            if (PRESSED(back)) g_menu = Menu::Root;
        } else if (g_menu == Menu::Target) {
            int m = (int)en.size();
            if (m == 0) g_menu = Menu::Root;
            else {
                if (PRESSED(up) || PRESSED(left)) g_targetCur = (g_targetCur + m - 1) % m;
                if (PRESSED(down) || PRESSED(right)) g_targetCur = (g_targetCur + 1) % m;
                g_targetCur = std::min(g_targetCur, m - 1);
                if (PRESSED(back)) g_menu = g_targetFor == CmdKind::Attack ? Menu::Root : g_targetBack;
                if (PRESSED(ok)) battle::PostCommand({g_targetFor, g_targetIndex, en[g_targetCur]});
            }
        }
    }

    float top = a.y + Px(26), rowH = std::min(Px(40), (b.y - top - Px(8)) / n);
    for (int i = 0; i < n; ++i) {
        ImVec2 r0(a.x + Px(10), top + i * rowH), r1(b.x - Px(10), top + (i + 1) * rowH - Px(3));
        if (i == g_cur) {
            if (g_menu == Menu::Root) Selection(dl, r0, r1, now);
            else dl->AddRectFilled(r0, r1, Col(0x2A3C6E, 0.8f), Px(4));
        }
        Text(dl, Px(24), ImVec2(r0.x + Px(18), r0.y + (rowH - Px(28)) / 2), Col(S.commands[i].enabled ? kText : kDim), S.commands[i].label);
    }
    if (g_menu == Menu::Root) help = S.commands[g_cur].description;

    // Sub windows open to the right of the command window, like KH2's.
    auto subWindow = [&](int rows, const char* title) {
        ImVec2 c(b.x + Px(10), b.y - (rows * Px(38) + Px(34))), d(c.x + Px(330), b.y);
        Panel(dl, c, d, 1.f);
        Tab(dl, ImVec2(c.x + Px(14), c.y), title, kParty);
        return c;
    };
    bool listShown = g_menu == Menu::Magic || g_menu == Menu::Techs || (g_menu == Menu::Target && g_targetFor != CmdKind::Attack);
    Menu listMenu = g_menu == Menu::Target ? g_targetBack : g_menu;
    if (listShown) {
        const auto& list = listMenu == Menu::Techs ? S.techs : S.magic;
        int m = (int)list.size();
        ImVec2 c = subWindow(std::max(1, m), listMenu == Menu::Techs ? "TECHNIQUES" : "MAGIC");
        for (int i = 0; i < m; ++i) {
            const UiMagic& s = list[i];
            ImVec2 r0(c.x + Px(10), c.y + Px(26) + i * Px(38)), r1(c.x + Px(320), r0.y + Px(35));
            if (i == g_magicCur) { if (g_menu != Menu::Target) Selection(dl, r0, r1, now); else dl->AddRectFilled(r0, r1, Col(0x2A3C6E, 0.8f), Px(4)); }
            Text(dl, Px(23), ImVec2(r0.x + Px(18), r0.y + Px(4)), Col(s.usable ? kText : kDim), s.name);
            std::string cost = s.mp < 0 ? "" : s.mp >= 0xFF ? "All MP" : std::to_string(s.mp) + " MP";
            ImVec2 ts = Measure(Px(19), cost);
            Text(dl, Px(19), ImVec2(r1.x - ts.x - Px(10), r0.y + Px(7)), Col(s.usable ? 0xA8CCFF : kDim), cost);
        }
        if (g_menu != Menu::Target && g_magicCur < m) help = list[g_magicCur].description;
        if (!m) Text(dl, Px(22), ImVec2(c.x + Px(20), c.y + Px(30)), Col(kDim), "Nothing to use");
    } else if (g_menu == Menu::Items || g_menu == Menu::ItemTarget) {
        int m = (int)S.items.size();
        ImVec2 c = subWindow(std::max(1, m), "ITEMS");
        for (int i = 0; i < m; ++i) {
            const UiItem& it = S.items[i];
            ImVec2 r0(c.x + Px(10), c.y + Px(26) + i * Px(38)), r1(c.x + Px(320), r0.y + Px(35));
            if (i == g_itemCur) Selection(dl, r0, r1, now);
            uint32_t c = it.count > 0 ? kText : kDim;   // none left: dimmed, can't be chosen
            Text(dl, Px(23), ImVec2(r0.x + Px(18), r0.y + Px(4)), Col(c), it.name);
            std::string cnt = "x" + std::to_string(it.count);
            ImVec2 ts = Measure(Px(20), cnt);
            Text(dl, Px(20), ImVec2(r1.x - ts.x - Px(10), r0.y + Px(6)), Col(c), cnt);
        }
        if (g_itemCur < m) help = S.items[g_itemCur].count > 0 ? S.items[g_itemCur].description : std::string("None left in the inventory");
        if (!m) Text(dl, Px(22), ImVec2(c.x + Px(20), c.y + Px(30)), Col(kDim), "No items");
        if (g_menu == Menu::ItemTarget) {
            // Who it is for: a small list beside the items.
            auto on = ItemTargets();
            int n2 = (int)on.size();
            ImVec2 c2(c.x + Px(340), c.y), d2(c2.x + Px(240), c2.y + Px(34) + std::max(1, n2) * Px(38));
            Panel(dl, c2, d2, 1.f);
            Tab(dl, ImVec2(c2.x + Px(14), c2.y), "USE ON", kParty);
            for (int i = 0; i < n2; ++i) {
                ImVec2 r0(c2.x + Px(10), c2.y + Px(26) + i * Px(38)), r1(c2.x + Px(230), r0.y + Px(35));
                if (i == g_itemOnCur) Selection(dl, r0, r1, now);
                const UiUnit& t = S.units[on[i]];
                Text(dl, Px(23), ImVec2(r0.x + Px(18), r0.y + Px(4)), Col(kText), t.name);
                std::string hp = std::to_string(std::max(0, t.hp)) + "/" + std::to_string(t.maxHp);
                ImVec2 ts = Measure(Px(18), hp);
                Text(dl, Px(18), ImVec2(r1.x - ts.x - Px(8), r0.y + Px(7)), Col(kDim), hp);
            }
            help = "Choose who to use it on";
        }
    } else if (g_menu == Menu::Party) {
        // KH2's Party command: the partner chosen leaves, and this world's partner takes his place.
        int m = (int)S.friends.size();
        ImVec2 c = subWindow(std::max(1, m), "SWAP OUT");
        for (int i = 0; i < m; ++i) {
            ImVec2 r0(c.x + Px(10), c.y + Px(26) + i * Px(38)), r1(c.x + Px(320), r0.y + Px(35));
            if (i == g_partyCur) Selection(dl, r0, r1, now);
            Text(dl, Px(23), ImVec2(r0.x + Px(18), r0.y + Px(4)), Col(kText), S.friends[i]);
        }
        help = "This partner leaves the battle and this world's partner takes his place";
    }
    if (g_menu == Menu::Target) help = g_targetFor == CmdKind::Attack ? "Choose an enemy to attack" : "Choose a target";
}

void DrawCenterBanner(ImDrawList* dl, float sw, float sh, double now) {
    bool show = S.phase == Phase::Victory || S.phase == Phase::GameOver;
    if (!show || S.banner.empty()) return;
    float a = std::min(1.f, (float)(now - S.phaseStart) * 4.f);
    float size = Px(64);
    ImVec2 ts = Measure(size, S.banner);
    float y = sh * 0.36f;
    dl->AddRectFilledMultiColor(ImVec2(0, y - Px(16)), ImVec2(sw, y + ts.y + Px(16)), Col(0x0B1226, 0), Col(0x0B1226, 0), Col(0x0B1226, 0.85f * a), Col(0x0B1226, 0.85f * a));
    Text(dl, size, ImVec2((sw - ts.x) / 2, y), Col(kGold, a), S.banner);
}

// A new wave of enemies: a big banner for a moment.
void DrawWaveBanner(ImDrawList* dl, float sw, float sh, double now) {
    if (S.waveAt < 0 || now - S.waveAt > 1.8) return;
    float a = (float)std::min(1.0, std::min((now - S.waveAt) * 5.0, (1.8 - (now - S.waveAt)) * 3.0));
    std::string w = "WAVE " + std::to_string(S.wave);
    float size = Px(60);
    ImVec2 ts = Measure(size, w);
    float y = sh * 0.36f;
    dl->AddRectFilledMultiColor(ImVec2(0, y - Px(16)), ImVec2(sw, y + ts.y + Px(16)), Col(0x0B1226, 0), Col(0x0B1226, 0), Col(0x0B1226, 0.85f * a), Col(0x0B1226, 0.85f * a));
    Text(dl, size, ImVec2((sw - ts.x) / 2, y), Col(kEnemy, a), w);
}

void DrawInfo(ImDrawList* dl, float sw, float sh, double now) {
    if (S.info.empty() || now - S.infoTime > 2.5) return;
    float size = Px(24);
    ImVec2 ts = Measure(size, S.info);
    ImVec2 a((sw - ts.x) / 2 - Px(22), sh * 0.55f), b((sw + ts.x) / 2 + Px(22), sh * 0.55f + ts.y + Px(14));
    Panel(dl, a, b, 0.92f);
    Text(dl, size, ImVec2(a.x + Px(22), a.y + Px(7)), Col(kText), S.info);
}
bool g_debug = false; bool g_f10 = false;
}

namespace ui {
void Setup() {
    ImGuiIO& io = ImGui::GetIO();
    const char* fonts[] = {"C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf"};
    for (auto p : fonts) if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) { g_font = io.Fonts->AddFontFromFileTTF(p, 32.f); if (g_font) break; }
    if (!g_font) g_font = io.Fonts->AddFontDefault();
}

void Frame(HWND hwnd) {
    double now = NowSec();
    battle::GetSnapshot(S);
    ReadInput(hwnd);
    bool f10 = Key(VK_F10) && GetForegroundWindow() == hwnd;
    if (f10 && !g_f10) g_debug = !g_debug;
    g_f10 = f10;
    ImGuiIO& io = ImGui::GetIO();
    float sw = io.DisplaySize.x, sh = io.DisplaySize.y;
    if (sw <= 0 || sh <= 0) return;
    g_sc = sh / 1080.f;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    DrawShatter(dl, sw, sh, now);
    bool inBattle = S.battle && !S.units.empty() && S.phase != Phase::Shatter && S.phase != Phase::LoadingArena && S.phase != Phase::Returning &&
                    S.phase != Phase::AwaitFoes && S.phase != Phase::GameOver && S.phase != Phase::PartyMenu;
    if (inBattle) {
        std::string help;
        bool victory = S.phase == Phase::Victory;
        if (!victory) { DrawMarkers(dl, sw, sh, now); DrawTurnOrder(dl, sw, sh); DrawEnemies(dl, sw, sh, now); }
        DrawPopups(dl, sw, sh, now);
        if (!S.kh2Menu) { DrawParty(dl, sw, sh, now); DrawCommands(dl, sw, sh, now, help); }
        if (!victory) { DrawTurnBanner(dl, sw, sh, now); if (S.menuOpen) DrawHelp(dl, sw, sh, help); }
        DrawInfo(dl, sw, sh, now);
        DrawWaveBanner(dl, sw, sh, now);
    }
    if (S.phase == Phase::Victory && S.encounter && S.revealAt > 0) {
        // Leaving a battle map: fade to black under the victory banner.
        float f = (float)std::clamp((now - S.phaseStart - 0.8) / 1.0, 0.0, 1.0);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, (int)(255 * f)));
    }
    DrawCenterBanner(dl, sw, sh, now);
    // Travelling to a battle map: black from the shatter until the fight is in place, then a short fade in.
    if (S.encounter && (S.phase == Phase::LoadingArena || S.phase == Phase::AwaitFoes))
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, 255));
    else if (S.revealAt > 0 && now - S.revealAt < 0.7) {
        // Black until the reveal time, then a short fade in.
        double f = std::clamp(1.0 - (now - S.revealAt) / 0.7, 0.0, 1.0);
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, (int)(255 * f)));
    }
    if (S.phase == Phase::Returning && now - S.phaseStart < 1.2)
        dl->AddRectFilled(ImVec2(0, 0), ImVec2(sw, sh), IM_COL32(0, 0, 0, (int)(255 * std::min(1.0, (1.2 - (now - S.phaseStart)) / 0.4))));
    if (g_debug || (S.testMode && kTestLayout)) {
        std::string d = "KH2 turn-based " TBKH2_VERSION " | " + S.debug + (S.testMode ? " | TEST" : "");
        Text(dl, Px(15), ImVec2(8, sh - Px(22)), IM_COL32(200, 255, 200, 220), d);
    }
}
}
