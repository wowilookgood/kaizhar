// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <array>
#include <cstring>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <dirent.h>

#include <fmt/format.h>

#include "citra_switch/switch_menu.h"
#include "citra_switch/switch_platform.h"
#include "citra_switch/title_menu.h"
#include "common/file_util.h"
#include "common/settings.h"
#include "core/file_sys/cia_container.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/loader/loader.h"

extern "C" unsigned hen_cia_chunk_bytes; // hen/jit/hen_jit.c
extern "C" unsigned long long hen_cia_read_ns, hen_cia_write_ns, hen_cia_chunks;

namespace TitleMenu {

namespace {

constexpr const char* RomsDir = "/switch/Kaizhar/roms/";
constexpr const char* BundledLauncher = "romfs:/hbmenu.3dsx";
constexpr const char* Hints =
    "A  Start      X  Change screens      Y  Refresh      +  Exit      "
    "In a title: press both sticks to come back here";
constexpr const char* PortraitHints = "A  Start      X  Change screens\n"
                                      "Y  Refresh      +  Exit\n"
                                      "In a title: press both sticks to come back here";

struct Layout {
    const char* name;
    Settings::LayoutOption option;
    bool upright;
    bool flipped = false; ///< upright, turned half a turn: the picture's top at the display's right
};
// (hen-layout.txt holds an index into this: new entries go at the end.)
constexpr Layout Layouts[] = {
    {"Landscape, top screen above the bottom one", Settings::LayoutOption::Default, false},
    {"Landscape, large top screen", Settings::LayoutOption::LargeScreen, false},
    {"Landscape, side by side", Settings::LayoutOption::SideScreen, false},
    {"Portrait, left side of the console up", Settings::LayoutOption::Default, true},
    {"Portrait, right side of the console up", Settings::LayoutOption::Default, true, true},
};
constexpr int LayoutCount = static_cast<int>(std::size(Layouts));

struct Entry {
    std::string name;
    std::string detail;
    std::string path;
    bool install = false; ///< a .cia to install
};

int saved_layout = -1;
int selected = 0;

std::string LayoutFile() {
    return FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + "hen-layout.txt";
}

int SavedLayout() {
    if (saved_layout < 0) {
        saved_layout = 0;
        std::string text;
        if (FileUtil::ReadFileToString(true, LayoutFile(), text) > 0) {
            saved_layout = std::clamp(std::atoi(text.c_str()), 0, LayoutCount - 1);
        }
    }
    sw_menu_set_portrait(Layouts[saved_layout].upright, Layouts[saved_layout].flipped);
    return saved_layout;
}

void SaveLayout(int layout) {
    saved_layout = layout;
    sw_menu_set_portrait(Layouts[layout].upright, Layouts[layout].flipped);
    FileUtil::CreateFullPath(LayoutFile());
    FileUtil::WriteStringToFile(true, LayoutFile(), std::to_string(layout));
}

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string Extension(const std::string& name) {
    const auto dot = name.rfind('.');
    return dot == std::string::npos ? std::string{} : Lower(name.substr(dot));
}

/// The title's own name (from its icon data), or nothing.
std::string TitleOf(const std::string& path) {
    std::string title;
    if (const auto loader = Loader::GetLoader(path)) {
        if (loader->ReadTitle(title) != Loader::ResultStatus::Success) {
            title.clear();
        }
    }
    std::replace(title.begin(), title.end(), '\n', ' ');
    return title;
}

std::vector<std::string> FilesIn(const std::string& dir) {
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            if (e->d_name[0] != '.') {
                names.emplace_back(e->d_name);
            }
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string InstalledPath(u64 title_id) {
    const std::string path =
        Service::AM::GetTitleContentPath(Service::FS::MediaType::SDMC, title_id);
    return !path.empty() && FileUtil::Exists(path) ? path : std::string{};
}

std::vector<Entry> Scan() {
    std::vector<Entry> entries;
    for (const std::string& name : FilesIn(RomsDir)) {
        const std::string path = RomsDir + name;
        const std::string ext = Extension(name);
        if (ext == ".3ds" || ext == ".cci" || ext == ".cxi" || ext == ".app" || ext == ".3dsx") {
            std::string title = TitleOf(path);
            entries.push_back({title.empty() ? name : title, name, path});
        } else if (ext == ".cia") {
            FileSys::CIAContainer container;
            FileUtil::IOFile file(path, "rb");
            if (container.Load(&file) != Loader::ResultStatus::Success) {
                continue;
            }
            if (InstalledPath(container.GetTitleMetadata().GetTitleID()).empty()) {
                entries.push_back({"Install " + name, "not installed yet", path, true});
            }
        }
    }
    // Applications installed on the emulated SD card.
    const std::string titles =
        Service::AM::GetMediaTitlePath(Service::FS::MediaType::SDMC) + "00040000/";
    for (const std::string& name : FilesIn(titles)) {
        if (name.size() != 8) {
            continue;
        }
        const u64 title_id = 0x0004000000000000ull | std::strtoull(name.c_str(), nullptr, 16);
        const std::string path = InstalledPath(title_id);
        if (!path.empty()) {
            std::string title = TitleOf(path);
            entries.push_back({title.empty() ? fmt::format("{:016X}", title_id) : title,
                               "installed", path});
        }
    }
    // Homebrew on the emulated SD card, where the 3DS keeps it: /3ds/x.3dsx and /3ds/x/x.3dsx.
    const std::string homebrew = FileUtil::GetUserPath(FileUtil::UserPath::SDMCDir) + "3ds/";
    for (const std::string& name : FilesIn(homebrew)) {
        std::vector<std::string> found;
        if (Extension(name) == ".3dsx") {
            found.push_back(homebrew + name);
        } else {
            for (const std::string& inner : FilesIn(homebrew + name + "/")) {
                if (Extension(inner) == ".3dsx") {
                    found.push_back(homebrew + name + "/" + inner);
                }
            }
        }
        for (const std::string& path : found) {
            const std::string file = path.substr(path.rfind('/') + 1);
            std::string title = TitleOf(path);
            entries.push_back({title.empty() ? file : title, "homebrew", path});
        }
    }
    if (FileUtil::Exists(BundledLauncher)) {
        entries.push_back({"3DS Homebrew Launcher", "bundled", BundledLauncher});
    }
    return entries;
}

/// Whether a .cia's first content is encrypted: by the title key (it then doesn't start with an
/// NCCH header) or by NCCH encryption (the header's "no crypto" flag is clear). The emulator
/// refuses both, with a general failure.
bool IsEncrypted(const std::string& path) {
    FileSys::CIAContainer container;
    FileUtil::IOFile file(path, "rb");
    if (!file.IsOpen() || container.Load(&file) != Loader::ResultStatus::Success) {
        return false;
    }
    std::array<u8, 0x200> header{};
    if (!file.Seek(container.GetContentOffset(0), SEEK_SET) ||
        file.ReadBytes(header.data(), header.size()) != header.size()) {
        return false;
    }
    if (std::memcmp(header.data() + 0x100, "NCCH", 4) != 0) {
        return true;
    }
    return (header[0x188 + 7] & 0x04) == 0;
}

void Install(const Entry& entry) {
    const std::string name = entry.path.substr(entry.path.rfind('/') + 1);
    int shown = -1;
    sw_menu_message("Installing", name.c_str(), "Starting...", 0.0f);
    const u64 size = FileUtil::GetSize(entry.path);
    const auto started = std::chrono::steady_clock::now();
    const auto status =
        Service::AM::InstallCIA(entry.path, [&](std::size_t done, std::size_t total) {
            const int percent = total ? static_cast<int>(done * 100 / total) : 0;
            if (percent != shown) {
                shown = percent;
                sw_menu_message("Installing", name.c_str(), fmt::format("{}%", percent).c_str(),
                                percent / 100.0f);
            }
        });
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("hen: menu: install %s: status %u, %.0f MiB in %.1f s (%.1f MiB/s); %llu pieces of "
                "%u KiB: reading the .cia %.1f s, decrypting and writing the title %.1f s\n",
                entry.path.c_str(), static_cast<unsigned>(status), size / 1048576.0, seconds,
                seconds > 0 ? size / 1048576.0 / seconds : 0.0, hen_cia_chunks,
                (hen_cia_chunk_bytes ? hen_cia_chunk_bytes : 0x1000000u) / 1024, hen_cia_read_ns / 1e9,
                hen_cia_write_ns / 1e9);
    using Status = Service::AM::InstallStatus;
    if (status == Status::Success) {
        ShowMessage("Installed", name,
                    fmt::format("It is in the list now ({:.0f} s). The .cia in roms is no longer needed.",
                                seconds));
    } else if (status == Status::ErrorEncrypted || IsEncrypted(entry.path)) {
        ShowMessage("Couldn't install", name,
                    "The file is encrypted. Kaizhar installs decrypted .cia files only.");
    } else {
        ShowMessage("Couldn't install", name,
                    status == Status::ErrorInvalid ? "The file isn't a valid .cia."
                                                   : "The file couldn't be read.");
    }
}

} // namespace

void ApplySavedLayout(int override_layout) {
    const Layout& layout =
        Layouts[override_layout >= 0 && override_layout < LayoutCount ? override_layout : SavedLayout()];
    Settings::values.layout_option = layout.option;
    Settings::values.upright_screen = layout.upright;
    sw_set_orientation(layout.upright, layout.flipped);
}

void ShowMessage(const std::string& heading, const std::string& line1, const std::string& line2) {
    if (!sw_menu_open()) {
        return;
    }
    SavedLayout();
    sw_menu_message(heading.c_str(), line1.c_str(), (line2 + "   (A: continue)").c_str(), -1.0f);
    sw_menu_wait_button();
}

std::optional<Choice> Run() {
    if (!sw_menu_open()) {
        return std::nullopt;
    }
    SavedLayout();
    sw_menu_message("Kaizhar", "Looking for titles...", nullptr, -1.0f);
    std::vector<Entry> entries = Scan();
    for (;;) {
        std::vector<sw_menu_item> items;
        items.reserve(entries.size());
        for (const Entry& entry : entries) {
            items.push_back({entry.name.c_str(), entry.detail.c_str()});
        }
        const int count = static_cast<int>(items.size());
        selected = std::clamp(selected, 0, std::max(count - 1, 0));
        const int action = sw_menu_frame("Kaizhar", items.data(), count, &selected,
                                         Layouts[SavedLayout()].name,
                                         Layouts[SavedLayout()].upright ? PortraitHints : Hints);
        switch (action) {
        case SW_MENU_SELECT: {
            const Entry entry = entries[selected];
            if (entry.install) {
                Install(entry);
                entries = Scan();
                break;
            }
            ApplySavedLayout();
            sw_menu_message(entry.name.c_str(), "Starting...", nullptr, -1.0f);
            sw_menu_close();
            return Choice{entry.name, entry.path};
        }
        case SW_MENU_OPTION:
            SaveLayout((SavedLayout() + 1) % LayoutCount);
            break;
        case SW_MENU_REFRESH:
            entries = Scan();
            break;
        case SW_MENU_EXIT:
            sw_menu_close();
            return std::nullopt;
        default:
            break;
        }
    }
}

} // namespace TitleMenu
