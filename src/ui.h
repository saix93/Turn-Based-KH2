#pragma once
#include "common.h"
namespace ui {
void Setup();          // fonts and style, once ImGui exists
void Frame(HWND hwnd); // build this frame's battle UI
}
namespace render { bool InstallHooks(); }
