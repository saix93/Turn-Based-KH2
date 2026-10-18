# Builds dist/KH2xFFX-<version>/dinput8.dll with MSVC (x64). Runs the sheet preflight first.
param([string]$Version = "1.0.0")
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
python "$root\tools\sheets.py" gen
if ($LASTEXITCODE -ne 0) { throw "sheet preflight failed" }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
$obj = "$root\build\obj"; $out = "$root\dist\KH2xFFX-$Version"
New-Item -ItemType Directory -Force $obj, $out | Out-Null

$tp = "$root\third_party"
$src = @(
  "$root\src\dllmain.cpp", "$root\src\game.cpp", "$root\src\battle.cpp",
  "$root\src\render.cpp", "$root\src\ui.cpp", "$root\src\testmode.cpp",
  "$tp\imgui\imgui.cpp", "$tp\imgui\imgui_draw.cpp", "$tp\imgui\imgui_tables.cpp", "$tp\imgui\imgui_widgets.cpp",
  "$tp\imgui\backends\imgui_impl_win32.cpp", "$tp\imgui\backends\imgui_impl_dx11.cpp", "$tp\imgui\backends\imgui_impl_dx12.cpp",
  "$tp\minhook\src\buffer.c", "$tp\minhook\src\hook.c", "$tp\minhook\src\trampoline.c", "$tp\minhook\src\hde\hde64.c"
) | ForEach-Object { "`"$_`"" }
$inc = "/I`"$root\src`" /I`"$tp\imgui`" /I`"$tp\imgui\backends`" /I`"$tp\minhook\include`""
$cl = "cl /nologo /LD /O2 /MT /EHa /std:c++20 /W3 /DNDEBUG /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS $inc /Fo`"$obj\\`" " + ($src -join " ") +
      " /link /DEF:`"$root\src\dinput8.def`" /OUT:`"$out\dinput8.dll`" d3d11.lib d3d12.lib dxgi.lib user32.lib advapi32.lib"
cmd /c "`"$vcvars`" >nul && $cl"
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Remove-Item "$out\dinput8.lib", "$out\dinput8.exp" -ErrorAction SilentlyContinue
Write-Host "built $out\dinput8.dll"
