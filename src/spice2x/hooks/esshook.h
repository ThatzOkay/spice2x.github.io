#pragma once

#include <windows.h>

namespace hooks::ess {

    // redirects file access of ess.dll to the drives it expects (E:, F:) into dev/vfs/drive_<letter>
    // if those drives do not exist on this machine; see misc/otaupdate.h for applying staged updates
    void init(HMODULE module);
}
