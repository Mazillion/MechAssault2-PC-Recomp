@echo off
rem Builds the MA2 Scenario Editor and its format tests with MSVC (Visual Studio 2019+ or Build Tools).
rem   build.cmd           editor + tests
rem   build.cmd tests     format tests only
rem   build.cmd clean     remove obj\ and out\
rem Output: out\MA2 Scenario Editor.exe, out\fmt_test.exe
setlocal EnableExtensions
set "ROOT=%~dp0"
cd /d "%ROOT%"
if /i "%~1"=="clean" (
  if exist obj rmdir /s /q obj
  if exist out rmdir /s /q out
  exit /b 0
)
if not defined VCINSTALLDIR call :vcvars || exit /b 1
rem Dear ImGui 1.92.5 is shared with the game source.
set "IMGUI=%ROOT%..\kit\source\game\vendor\imgui-1.92.5"
if not exist "%IMGUI%\imgui.h" (echo Dear ImGui not found at %IMGUI% & exit /b 1)
for %%D in (obj obj\models obj\core obj\app obj\imgui obj\tests out) do if not exist %%D mkdir %%D

rem /DNDEBUG: no assert() text (it would carry build paths); /Brepro + /PDBALTPATH keep paths and time stamps out of the exe.
rem   Release check (tools/privacy_scan.py --binaries): the exe must not contain the build folder's path.
rem IMGUI_ENABLE_TEST_ENGINE: ImGui's item hooks, used by --automation (src/app/automation.cpp) to find items by label.
set "CFLAGS=/nologo /std:c++20 /O2 /EHsc /MT /utf-8 /W3 /wd4996 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS /DIMGUI_ENABLE_TEST_ENGINE /DNDEBUG /MP /Z7 /Brepro"

rem The short git id goes into the About box and the crash log ("dev" when git is not available).
set "GIT=dev"
for /f %%G in ('git rev-parse --short HEAD 2^>nul') do set "GIT=%%G"
if defined MA2SE_GIT_ID set "GIT=%MA2SE_GIT_ID%"
set "CFLAGS=%CFLAGS% /DMA2SE_GIT=\"%GIT%\""

set "MODELS=%ROOT%models"
rem MA2SE_EXE=out\other.exe links the editor elsewhere (when out\MA2 Scenario Editor.exe is running).
if not defined MA2SE_EXE set "MA2SE_EXE=out\MA2 Scenario Editor.exe"
echo [core]
cl %CFLAGS% /c src\core\*.cpp /I"%MODELS%" /Foobj\core\ || exit /b 1
lib /nologo /out:obj\ma2core.lib obj\core\*.obj || exit /b 1

echo [models]
cl %CFLAGS% /c "%MODELS%\src\*.cpp" /Foobj\models\ || exit /b 1
lib /nologo /out:obj\ma2models.lib obj\models\*.obj || exit /b 1

echo [tests]
cl %CFLAGS% /c tests\fmt_test.cpp /Foobj\tests\ || exit /b 1
link /nologo /Brepro /out:out\fmt_test.exe obj\tests\fmt_test.obj obj\ma2core.lib obj\ma2models.lib user32.lib shell32.lib ole32.lib || exit /b 1
if /i "%~1"=="tests" exit /b 0

echo [imgui]
if not exist obj\imgui\test-engine-hooks.txt (
  cl %CFLAGS% /c "%IMGUI%\imgui.cpp" "%IMGUI%\imgui_draw.cpp" "%IMGUI%\imgui_tables.cpp" "%IMGUI%\imgui_widgets.cpp" "%IMGUI%\backends\imgui_impl_win32.cpp" "%IMGUI%\backends\imgui_impl_dx11.cpp" "%IMGUI%\misc\cpp\imgui_stdlib.cpp" /I"%IMGUI%" /Foobj\imgui\ || exit /b 1
  echo built with IMGUI_ENABLE_TEST_ENGINE> obj\imgui\test-engine-hooks.txt
)

echo [resources]
rc /nologo /fo obj\editor.res res\editor.rc || exit /b 1

echo [app]
cl %CFLAGS% /c src\app\*.cpp /I"%IMGUI%" /I"%IMGUI%\backends" /I"%IMGUI%\misc\cpp" /I"%MODELS%" /Foobj\app\ || exit /b 1
link /nologo /Brepro /DEBUG:FULL /PDBALTPATH:%%_PDB%% /OPT:REF /OPT:ICF /subsystem:windows /out:"%MA2SE_EXE%" obj\app\*.obj obj\editor.res obj\imgui\*.obj obj\ma2core.lib obj\ma2models.lib ^
  d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib ole32.lib comdlg32.lib dbghelp.lib dwmapi.lib imm32.lib winmm.lib advapi32.lib || exit /b 1
echo built "%MA2SE_EXE%"
exit /b 0

:vcvars
rem vswhere ships with every Visual Studio 2017+ and Build Tools install. pushd avoids the ")" of
rem "Program Files (x86)" inside the for /f command.
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (echo Visual Studio / Build Tools not found: install the "Desktop development with C++" workload & exit /b 1)
pushd "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq tokens=*" %%i in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
popd
if not defined VSDIR (echo No Visual Studio with the MSVC x64 tools found & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
