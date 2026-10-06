#pragma once

#include <cstddef>
#include <filesystem>

namespace ota {

    enum class ApplyStatus {
        NothingStaged,
        Applied,
        Failed,
    };

    struct ApplyResult {
        ApplyStatus status = ApplyStatus::NothingStaged;
        size_t files = 0;
    };

    /*
     * Applies updates staged by ess.dll (see hooks/esshook.cpp) the way the cabinet's select.exe
     * does it on boot: dev/vfs/drive_f/<model>-001/update/ex holds an update.qsv manifest and the
     * files it lists. All files are verified first and nothing is touched if any of them is bad.
     * Files that get replaced are backed up to dev/vfs/drive_d/update-backup/<timestamp>.
     *
     * This replaces game files, so it has to run before any game DLL is loaded.
     */
    ApplyResult apply_staged_updates(const std::filesystem::path &game_dir);
}
