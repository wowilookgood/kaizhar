// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "citra_switch/cache_clear.h"
#include "common/file_util.h"

namespace Caches {

namespace {

constexpr const char* UserDir = "/switch/Kaizhar/";
constexpr const char* PrivateBase = "sdmc:/switch/Kaizhar/ouncevk"; ///< (as the driver wants it)
constexpr const char* PrivateDir = "/switch/Kaizhar/ouncevk/shader-cache/";
constexpr const char* SharedDir = "/config/ouncevk/shader-cache/";
/// Its presence: the shared folder's files were offered to the private one once (or the
/// private one was cleared on purpose). Not in shader-cache: the driver tidies that folder.
constexpr const char* MigratedMark = "/switch/Kaizhar/ouncevk/copied-from-shared";

bool driver_cache_shared = false;

bool EndsWith(const std::string& text, const char* end) {
    const std::string suffix{end};
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// The entries of a directory: names first, then the work, so that nothing is removed from a
/// directory while it is being listed.
std::vector<std::string> NamesIn(const std::string& directory) {
    std::vector<std::string> names;
    FileUtil::ForeachDirectoryEntry(nullptr, directory,
                                    [&names](u64*, const std::string&, const std::string& name) {
                                        names.push_back(name);
                                        return true;
                                    });
    return names;
}

/// Removes the files in a directory (and below it, if deep); the directories stay.
void RemoveFilesIn(const std::string& directory, bool deep, Tally& tally) {
    if (!FileUtil::Exists(directory)) {
        return;
    }
    for (const std::string& name : NamesIn(directory)) {
        const std::string path = directory + name;
        if (FileUtil::IsDirectory(path)) {
            if (deep) {
                RemoveFilesIn(path + "/", true, tally);
            }
            continue;
        }
        const u64 size = FileUtil::GetSize(path);
        if (FileUtil::Delete(path) && !FileUtil::Exists(path)) {
            tally.files++;
            tally.bytes += size;
        } else {
            tally.failed++;
            if (tally.first_failed.empty()) {
                tally.first_failed = path;
            }
        }
    }
}

void Mark() {
    FileUtil::CreateFullPath(MigratedMark);
    if (std::FILE* file = std::fopen(MigratedMark, "w")) {
        std::fputs("The shared driver cache was copied here once, or this one was cleared.\n", file);
        std::fclose(file);
    }
}

} // namespace

void SetupDriverCache(bool shared, bool cold) {
    driver_cache_shared = shared;
    if (shared) {
        std::printf("hen: driver cache: the shared folder %s (--shared-driver-cache)\n", SharedDir);
        if (cold) {
            Tally tally;
            for (const std::string& name : NamesIn(SharedDir)) {
                if (EndsWith(name, ".bin")) { // (as --cold-driver always did)
                    const std::string path = std::string{SharedDir} + name;
                    const u64 size = FileUtil::GetSize(path);
                    if (FileUtil::Delete(path)) {
                        tally.files++;
                        tally.bytes += size;
                    }
                }
            }
            std::printf("hen: --cold-driver: removed %u file(s) from %s\n", tally.files, SharedDir);
        }
        return;
    }
    // (the driver reads the variable when its device is made, and adds "shader-cache")
    setenv("MESA_SHADER_CACHE_DIR", PrivateBase, 1);
    FileUtil::CreateFullPath(PrivateDir);
    if (cold) {
        // (a directed cold run: this folder emptied, the shared one neither read nor touched;
        // no copy from it, and no mark: an ordinary start later may still take it over)
        Tally tally;
        RemoveFilesIn(PrivateDir, false, tally);
        std::printf("hen: --cold-driver: removed %u file(s) from %s%s\n", tally.files, PrivateDir,
                    tally.failed != 0 ? " (some could not be removed)" : "");
        // (one line about the copy on every start where its state could have changed)
        if (!FileUtil::Exists(MigratedMark)) {
            std::printf("hen: driver cache: Kaizhar's own folder %s; the one-time copy from %s was skipped "
                        "(a cold start) and is still due: the next start without --cold-driver looks again\n",
                        PrivateDir, SharedDir);
        }
        return;
    }
    if (FileUtil::Exists(MigratedMark)) {
        return;
    }
    bool have_store = false;
    for (const std::string& name : NamesIn(PrivateDir)) {
        have_store |= EndsWith(name, ".bin");
    }
    unsigned copied = 0, failed = 0;
    u64 bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    if (!have_store) {
        for (const std::string& name : NamesIn(SharedDir)) {
            if (!EndsWith(name, ".bin") && !EndsWith(name, ".idx")) {
                continue;
            }
            const std::string from = std::string{SharedDir} + name;
            if (FileUtil::IsDirectory(from)) {
                continue;
            }
            if (FileUtil::Copy(from, std::string{PrivateDir} + name)) {
                copied++;
                bytes += FileUtil::GetSize(from);
            } else {
                failed++;
                FileUtil::Delete(std::string{PrivateDir} + name); // (no half file for the driver)
            }
        }
    }
    Mark();
    std::printf("hen: driver cache: Kaizhar's own folder %s; first start with it: %u file(s), %llu KiB "
                "copied from %s in %lld ms%s (the shared files are left as they are)\n",
                PrivateDir, copied, static_cast<unsigned long long>(bytes >> 10), SharedDir,
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count()),
                failed != 0 ? ", some could not be copied" : have_store ? " (it had a cache already)" : "");
}

const char* DriverDir() {
    return driver_cache_shared ? SharedDir : PrivateDir;
}

Tally ClearAll() {
    Tally tally;
    // The shader and pipeline cache files: the shader directory in use, and the test one. Only
    // ever inside the user folder.
    const std::string user{UserDir};
    std::string shaders = FileUtil::GetUserPath(FileUtil::UserPath::ShaderDir);
    if (shaders.rfind("sdmc:", 0) == 0) {
        shaders.erase(0, 5);
    }
    if (shaders.size() > user.size() && shaders.compare(0, user.size(), user) == 0 &&
        shaders.find("shaders") != std::string::npos) {
        RemoveFilesIn(shaders, true, tally);
    } else {
        std::printf("hen: caches: the shader directory %s is not in %s: left alone\n", shaders.c_str(), UserDir);
    }
    for (const char* directory : {"shaders/", "shaders-cold/"}) {
        if (user + directory != shaders) {
            RemoveFilesIn(user + directory, true, tally);
        }
    }
    // The lists of compiled blocks.
    RemoveFilesIn(user + "jit/", false, tally);
    RemoveFilesIn(user + "jit-fresh/", false, tally);
    // The driver's cache: Kaizhar's own folder only. With --shared-driver-cache there is no
    // such folder in use and the shared one is not ours to empty.
    if (!driver_cache_shared) {
        RemoveFilesIn(PrivateDir, false, tally);
        Mark(); // (so that the next start does not fill it from the shared folder again)
    }
    std::printf("hen: caches cleared: %u file(s), %llu KiB removed, %u could not be removed%s%s%s\n",
                tally.files, static_cast<unsigned long long>(tally.bytes >> 10), tally.failed,
                tally.failed != 0 ? " (first: " : "", tally.first_failed.c_str(), tally.failed != 0 ? ")" : "");
    if (driver_cache_shared) {
        std::printf("hen: caches cleared: the driver's cache was left (--shared-driver-cache: it is shared)\n");
    }
    return tally;
}

} // namespace Caches
