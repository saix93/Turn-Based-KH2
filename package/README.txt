Turn-Based Kingdom Hearts 2  (KINGDOM HEARTS II, fought like FINAL FANTASY X)
=============================================================================

KINGDOM HEARTS II with FINAL FANTASY X's battle rules. Every model, animation, spell, sound and
piece of music is KINGDOM HEARTS II's own; nothing from FINAL FANTASY X is used or needed.

- Random encounters: enemies no longer roam the field. Walking fills a hidden meter; when it
  fills, the screen shatters and a battle starts at that part of the world's battle spot.
- A CTB turn order (shown on the right): faster characters act more often, and only the one
  taking a turn moves. The party lines up facing the enemies with the camera behind them.
- Sora: Attack, Magic (your save's spells), Items (only those equipped on him), Defend,
  Party (swap a partner through KH2's own Party command; costs no turn) and Escape.
- Donald, Goofy and each world's partner take their own turns, and you choose their actions:
  Attack, Magic/Techniques, Items (their own equipped ones) and Defend.
- A knocked-out partner stays down until a Cure or an item revives him; everyone gets back up
  when the battle ends. If Sora falls, the battle is lost.
- Win or escape and you are put back where you were walking.

Needs: KINGDOM HEARTS -HD 1.5+2.5 ReMIX- (Steam). Single player.

Controls in battle: arrows / WASD / D-pad to choose, Enter / Space / E / A to confirm,
Backspace / Q / Esc / B to go back. F10 toggles a small debug line.

How it works: dinput8.dll sits next to the KH2 executable (KH2 loads DINPUT8.dll; calls are passed
on to Windows' own dinput8.dll). It only activates inside KINGDOM HEARTS II FINAL MIX.exe and checks
the game's code bytes first; on any other build it stays off. Log: %LOCALAPPDATA%\KH2FFXTB\ffxtb.log.
Uninstall: delete dinput8.dll from the game folder.

Source: https://github.com/saix93/Turn-Based-KH2
