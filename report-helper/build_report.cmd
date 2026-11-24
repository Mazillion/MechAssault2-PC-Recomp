@echo off
rem Builds "MA2 Report.exe", the problem-report helper the game, its crash and freeze boxes and the
rem Start menu entry "Report a problem (MechAssault 2 Expanded)" start (see report.c).
rem The version comes from ..\VERSION; the relay address from the MA2_RELAY_URL variable
rem (tools\build_release.py sets it from kit\installer\data\report.json; unset or empty = Save only).
rem Needs the MSVC x64 tools (a "x64 Native Tools" prompt, or Visual Studio found with vswhere).
rem /Brepro and /PDBALTPATH keep build paths and time stamps out of the exe.
setlocal
cd /d "%~dp0"
if not defined VCINSTALLDIR call :vcvars || exit /b 1
set PUBVER=
for /f "usebackq tokens=* delims=" %%a in ("%~dp0..\VERSION") do if not defined PUBVER set "PUBVER=%%a"
if not defined PUBVER (echo VERSION is missing or empty & exit /b 1)
rem A preview build (1.2.3-preview.4) keeps the numbers of 1.2.3 in the version resource.
for /f "tokens=1 delims=-" %%s in ("%PUBVER%") do set PUBNUM=%%s
for /f "tokens=1-3 delims=." %%x in ("%PUBNUM%") do (set V1=%%x& set V2=%%y& set V3=%%z)
if not defined V3 set V3=0
> version.h echo #define MA2_VER_COMMA %V1%,%V2%,%V3%,0
>> version.h echo #define MA2_VER_STR "%PUBVER%"
>> version.h echo #define MA2_VER_FILE "%PUBNUM%.0"
> relay.h echo /* written by build_report.cmd */
if defined MA2_RELAY_URL >> relay.h echo #define MA2_RELAY_URL "%MA2_RELAY_URL%"
rc /nologo /fo report.res report.rc || exit /b 1
cl /nologo /O2 /MT /W3 /Brepro /DUNICODE /D_UNICODE report.c report.res /Fe:"MA2 Report.exe" /link /SUBSYSTEM:WINDOWS /Brepro /PDBALTPATH:%%_PDB%% user32.lib gdi32.lib comctl32.lib shell32.lib winhttp.lib advapi32.lib bcrypt.lib || exit /b 1
del report.obj report.res
echo REPORT OK (version %PUBVER%)
exit /b 0

:vcvars
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (echo Visual Studio / Build Tools not found: install the "Desktop development with C++" workload & exit /b 1)
pushd "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
for /f "usebackq tokens=*" %%i in (`.\vswhere.exe -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
popd
if not defined VSDIR (echo No Visual Studio with the MSVC x64 tools found & exit /b 1)
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
exit /b 0
