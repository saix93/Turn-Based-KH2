#pragma once
#include "common.h"

// Melty's test runs: while %LOCALAPPDATA%\Melty\test\<slug>.json exists, the mod starts an encounter
// by itself, plays Roxas's turns with magic, and appends "pass <check>" lines to the file it names.
namespace testmode {
void Init();
void Poll();                 // cheap; call every frame
bool Active();
bool WantEncounter();
void EncounterStarted();
void Pass(const char* check);
void WriteState(const std::string& json); // Melty test_look shows this
}
