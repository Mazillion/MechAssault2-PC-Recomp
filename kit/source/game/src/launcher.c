#define UNICODE
#define _UNICODE
#include <windows.h>
#include <wchar.h>
#include <stdio.h>

/* A GUI entry point in the game folder. Explorer does not start a shell, and
 * the game writes its own logs when it has no redirected diagnostic streams. */
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE previous, PWSTR arguments, int show)
{
    wchar_t directory[MAX_PATH], executable[MAX_PATH], command[32768];
    STARTUPINFOW startup = { sizeof(startup) };
    /* Explicitly detach standard handles as well as the console. Launching
     * from a tool with redirected handles must still use the game's log file. */
    startup.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION child = {0};
    (void)instance; (void)previous; (void)show;
    if (!GetModuleFileNameW(NULL, directory, MAX_PATH)) return 1;
    wchar_t *slash = wcsrchr(directory, L'\\');
    if (!slash) return 1;
    *slash = 0;
    if (swprintf_s(executable, MAX_PATH, L"%s\\PC Release\\MechAssault 2.exe", directory) < 0)
        return 1;
    if (GetFileAttributesW(executable) == INVALID_FILE_ATTRIBUTES &&
        swprintf_s(executable, MAX_PATH, L"%s\\build\\mechassault2_recomp.exe", directory) < 0)
        return 1;
    if (GetFileAttributesW(executable) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(NULL, L"The game executable is missing from the PC Release and build folders.\n"
            L"Build mechassault2_recomp, then launch MechAssault 2 again.",
            L"MechAssault 2", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (swprintf_s(command, 32768, L"\"%s\" %s", executable, arguments ? arguments : L"") < 0)
        return 1;
    if (!CreateProcessW(executable, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, directory, &startup, &child)) {
        wchar_t error[256];
        swprintf_s(error, 256, L"Windows could not start MechAssault 2 (error %lu).", GetLastError());
        MessageBoxW(NULL, error, L"MechAssault 2", MB_OK | MB_ICONERROR);
        return 1;
    }
    CloseHandle(child.hThread);
    wchar_t pid_path[32768];
    DWORD pid_length=GetEnvironmentVariableW(L"RECOMP_LAUNCH_PID_FILE",pid_path,32768);
    if(pid_length && pid_length<32768) {
        FILE *pid_file=_wfopen(pid_path,L"w");
        if(pid_file) { fprintf(pid_file,"%lu\n",child.dwProcessId);fclose(pid_file); }
    }
    CloseHandle(child.hProcess);
    return 0;
}
