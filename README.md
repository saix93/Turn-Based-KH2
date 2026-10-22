# Turn-Based Kingdom Hearts 2

**KINGDOM HEARTS II with turn-based battles, inspired by FINAL FANTASY X's combat.** Random encounters, a visible turn order and one action per turn.

A mod for the Steam version of *KINGDOM HEARTS -HD 1.5+2.5 ReMIX-*.

> **Note:** boss battles have not been tested with the turn-based system yet. Random encounters are what has been tested.

![Sora's turn in the Beast's Castle ballroom, with the turn order, party and enemy windows](docs/beasts-castle.jpg)

| | |
| --- | --- |
| ![The Land of Dragons: Rapid Thrusters attack in turn](docs/land-of-dragons.jpg) | ![Beast's own turn, choosing Attack or Defend](docs/partner-turn.jpg) |

## What it plays like

- **Random encounters.** Enemies no longer roam the field. Walking fills a hidden encounter meter, and when it fills the screen shatters and a battle starts. Each part of a world has its own battle spot, so you fight where you are.
- **A turn order.** The order of the next turns is listed on the right, and faster characters act more often. Only the one taking a turn moves; everyone else waits in their place.
- **Formation and camera.** Sora and his two partners line up facing the enemy line, and the camera sits behind the party. Whoever acts steps forward, makes a real KH2 move, then walks back.
- **Sora's commands:**
  - **Attack:** his real Keyblade strike.
  - **Magic:** the spells your save has (Fire, Blizzard, Thunder, Cure, Magnet, Reflect), cast with KH2's own spells and MP.
  - **Items:** only the items equipped on Sora, used through KH2's own menu, on one target or the whole party.
  - **Defend:** takes half damage, and the next turn comes sooner.
  - **Party:** swaps a partner through KH2's own Party command, for example Donald out and Auron in. It costs no turn.
  - **Escape:** runs from the fight.
- **Partners you control.** Donald, Goofy and each world's own partner (Beast, Auron and others) take their own turns, and you pick their actions.
  - Their actions are Attack, Magic or Techniques, Items and Defend.
  - **Donald:** Fire, Blizzard, Thunder and Cure.
  - **Goofy:** Goofy Bash, Goofy Tornado and Goofy Turbo.
  - Partners use only the items equipped on them.
- **Knock-outs.** A partner who falls stays down in KH2's own downed pose until a Cure or an item revives him. Partners get back up when the battle ends. If Sora falls, the battle is lost.
- **After the fight.** Win or escape and you're put back where you were walking.

### Items

| Item | Effect |
| --- | --- |
| Potion / Hi-Potion | Restores a third / two thirds of one character's HP |
| Ether | Restores a third of one character's MP |
| Elixir | Fully restores one character's HP and MP |
| Mega-Potion / Mega-Ether | Restores a third of the whole party's HP / MP |
| Megalixir | Fully restores the whole party's HP and MP |

## Controls (in battle)

| | Keyboard | Gamepad |
| --- | --- | --- |
| Choose | Arrow keys / WASD | D-pad / left stick |
| Confirm | Enter / Space / E | A |
| Back | Backspace / Q / Esc | B |

## Requirements

- *KINGDOM HEARTS -HD 1.5+2.5 ReMIX-*, the **Steam** version (the mod checks the game's code and stays off on any other build).
- Single player, Windows.

## Building

You need:
- **Visual Studio 2022 or later** (Community is free) with the **"Desktop development with C++"** workload. It brings the MSVC x64 compiler and the Windows SDK (DirectX 11/12).
- **Python 3**, on your `PATH`.
- **Git**.

1. Clone the repo **with its submodules** (Dear ImGui and MinHook):
   ```
   git clone --recursive https://github.com/saix93/Turn-Based-KH2.git
   cd Turn-Based-KH2
   ```
   If you already cloned it without `--recursive`, run `git submodule update --init`.
2. Build it from PowerShell or a command prompt:
   ```
   powershell -ExecutionPolicy Bypass -File build.ps1 -Version 1.0.0
   ```
   `build.ps1` does everything:
   - generates `src/gen/sheets.h` from the JSON in `sheets/`;
   - finds Visual Studio by itself (through `vswhere`) and loads its x64 environment;
   - compiles the mod, Dear ImGui and MinHook into one DLL. The C runtime is linked statically, so no Visual C++ redistributable is needed.
3. The result is `dist/TurnBasedKH2-1.0.0/dinput8.dll`.

`pack.sh <version>` (from Git Bash) does the same build, also stamps the version into the mod, and zips the DLL with `steam_appid.txt`, the readme and the licenses.

## Installing

1. Copy `dinput8.dll` into the game's folder, next to `KINGDOM HEARTS II FINAL MIX.exe`. In a default Steam install that's `steamapps\common\KINGDOM HEARTS -HD 1.5+2.5 ReMIX-\`.
2. Start the game as usual from Steam, pick KINGDOM HEARTS II and load a save.

**Optional:** copy `package/steam_appid.txt` to the same folder if you want to start `KINGDOM HEARTS II FINAL MIX.exe` directly. Without it, Steam restarts the game through its launcher.

**Uninstall:** delete `dinput8.dll` from the game folder.

## How it works

`dinput8.dll` is a proxy. KH2 loads `dinput8.dll` at startup, and Windows looks in the game's folder before `System32`, so it finds this one first. The mod loads Windows' real `dinput8.dll` itself and passes every DirectInput call on to it.
- **Where it runs:** only inside `KINGDOM HEARTS II FINAL MIX.exe`, and only after it has checked the game's code bytes. On any other build, or in the other games of the collection, it stays off.
- **What it does:** it hooks KH2's functions with [MinHook](https://github.com/TsudaKageyu/minhook) and drives KH2's own actors, commands and menus. The hooks cover the per-frame entity loop, the command executor, the freeze check, input, the camera, and the frame being presented. It draws its windows with Dear ImGui (DirectX 11/12).
- **Its data:** the battle rules come from the JSON sheets in `sheets/`: commands, magic, items, partner moves, battle spots per room, and timings and camera settings. They are compiled into the DLL.
- **Log:** `%LOCALAPPDATA%\TurnBasedKH2\turnbased.log` (the previous session is kept as `turnbased.prev.log`). F10 shows a small debug line.

## Project layout

| Folder | What's in it |
| --- | --- |
| `src/` | The mod: battle state machine (`battle.cpp`), game access and hooks (`game.cpp`), rendering (`render.cpp`), UI (`ui.cpp`), automated test mode (`testmode.cpp`) |
| `sheets/` | The battle data, as JSON |
| `tools/` | Research and sheet-generation scripts |
| `package/` | Files shipped with each release (readme, licenses, `steam_appid.txt`) |
| `third_party/` | Dear ImGui and MinHook (git submodules) |

## Credits and license

- Mod code: MIT (see `package/LICENSE.txt`).
- [Dear ImGui](https://github.com/ocornut/imgui) by Omar Cornut (MIT) and [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu (BSD-2-Clause). See `package/THIRD_PARTY_LICENSES.txt`.
- [OpenKH](https://github.com/OpenKH/OpenKh) documentation was used for KH2's object and file formats.
- KINGDOM HEARTS and FINAL FANTASY are © Disney and Square Enix. This is an unofficial fan mod. It ships none of the games' files, and you need your own copy of the game.
