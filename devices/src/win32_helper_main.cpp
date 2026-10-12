// axklib_helper.exe: the process rawdisk launches elevated (UAC) to do raw disk I/O for a
// non-elevated app. GUI subsystem so no console window flashes. Ship it next to your .exe
// (or set ElevationOptions::helper_path), ideally installed under Program Files and signed.
#include "rawdisk/block_device.hpp"

#include <windows.h>

#include <shellapi.h>

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Elevated process: never load DLLs from the application directory or the current
    // directory (DLL planting). Only System32 for anything loaded from here on.
    ::SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    ::SetDllDirectoryW(L"");

    int argc = 0;
    LPWSTR *argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    const int rc = rawdisk::helper_main(argc, argv);
    ::LocalFree(argv);
    return rc;
}
