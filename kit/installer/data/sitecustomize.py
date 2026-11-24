# Packed as python/Lib/sitecustomize.py (installer-exe/pack_installer.py): every Python the installer starts runs it first.
# 1.2.1 Linux (Wine/Proton): Python 3.12+ copies files with the Windows API CopyFile2, which Wine 9 does not implement (the
# process aborts with "unimplemented function KERNEL32.dll.CopyFile2"). Under Wine shutil is made to copy the portable way
# instead. On Windows nothing changes (Windows' ntdll has no wine_get_version).
def _wine_copyfile():
    try:
        import ctypes, _winapi
        ctypes.WinDLL('ntdll').wine_get_version          # AttributeError on Windows
    except Exception:
        return
    try:
        del _winapi.CopyFile2                            # shutil.copy2 then uses copyfile + copystat, as before 3.12
    except AttributeError:
        pass


_wine_copyfile()
del _wine_copyfile
