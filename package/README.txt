Roxas's Summer: Turn-Based  (KINGDOM HEARTS II, played with FINAL FANTASY X's battle ideas)
=========================================================================================

KINGDOM HEARTS II with FINAL FANTASY X's battle *concepts*. Every model, animation, spell, sound
and piece of music is KINGDOM HEARTS II's own; nothing from FINAL FANTASY X is used or needed.

- No enemies roam the field. Walking fills a hidden encounter meter; when it fills, the screen
  shatters and a battle starts. Win or escape and you are put back where you were walking.
- Every fight runs on a CTB turn order (shown on the right). Sora and his partners line up facing
  the enemies, the camera sits behind the party, and only the one taking a turn moves. After acting,
  everyone walks back to their own place.
- Sora: Attack (steps up and makes his real Keyblade attack), Magic (the spells your save has, cast
  with KH2's own Fire/Blizzard/Thunder/Cure/Magnet/Reflect), Items, Defend (Guard), Party, Escape.
- Donald and Goofy take their own turns and you choose their actions: Attack, Ability (their own
  move or spell), Defend.
- Party opens KH2's menu, where Party swaps a partner when the world has one to swap in.

Needs: KINGDOM HEARTS -HD 1.5+2.5 ReMIX (Steam build).

Controls in battle: arrows / WASD / D-pad to choose, Enter / Space / E / A to confirm,
Backspace / Q / B to go back. F10 toggles a small debug line.

How it works: dinput8.dll sits next to the KH2 executable (KH2 loads DINPUT8.dll; calls are passed
on to Windows' own dinput8.dll). It only activates inside KINGDOM HEARTS II FINAL MIX.exe and checks
the game's code bytes first; on any other build it stays off. Log: %LOCALAPPDATA%\KH2FFXTB\ffxtb.log.
Uninstall: delete dinput8.dll from the game folder.
