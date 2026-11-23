/* A stand-in for "MechAssault 2 Recomp Installer.exe" in a TEST install folder (never a real install): it only records
   how the game started it (command line, working folder, MA2_GAME_UPDATE) in fake-installer-ran.txt beside itself and
   exits. Used to test the game's Messages "Update now" (tests/installer/test_replies_120.py, the in-game run). */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, PWSTR c, int d)
{
    wchar_t self[MAX_PATH], out[MAX_PATH], cwd[MAX_PATH], upd[MAX_PATH] = L"";
    (void)a; (void)b; (void)c; (void)d;
    GetModuleFileNameW(NULL, self, MAX_PATH);
    wcscpy(out, self);
    wchar_t *s = wcsrchr(out, L'\\'); if (s) s[1] = 0;
    wcscat(out, L"fake-installer-ran.txt");
    GetCurrentDirectoryW(MAX_PATH, cwd);
    GetEnvironmentVariableW(L"MA2_GAME_UPDATE", upd, MAX_PATH);
    FILE *f = _wfopen(out, L"w, ccs=UTF-8");
    if (f) { fwprintf(f, L"cmd=%s\ncwd=%s\nMA2_GAME_UPDATE=%s\n", GetCommandLineW(), cwd, upd); fclose(f); }
    return 0;
}
