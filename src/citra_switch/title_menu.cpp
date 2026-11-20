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
#include <optional>
#include <utility>
#include <vector>

#include <dirent.h>

#include <fmt/format.h>

#include "citra_switch/cache_clear.h"
#include "citra_switch/link_3ds.h"
#include "citra_switch/online_menu.h"
#include "citra_switch/room_link.h"
#include "citra_switch/room_list.h"
#include "common/web_util.h"
#include "citra_switch/switch_menu.h"
#include "citra_switch/switch_platform.h"
#include "citra_switch/title_menu.h"
#include "citra_switch/tool_scan.h"
#include "common/file_util.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/file_sys/cia_container.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/fs/archive.h"
#include "core/hw/unique_data.h"
#include "core/loader/loader.h"
#include "core/system_titles.h"

extern "C" unsigned hen_cia_chunk_bytes; // hen/jit/hen_jit.c
extern "C" unsigned long long hen_cia_read_ns, hen_cia_write_ns, hen_cia_chunks;

namespace TitleMenu {

namespace {

constexpr const char* RomsDir = "/switch/Kaizhar/roms/";
constexpr const char* BundledLauncher = "romfs:/hbmenu.3dsx";
constexpr const char* Hints =
    "A  Start      X  Change screens      Y  Refresh      -  Multiplayer      L  Settings      +  Exit\n"
    "In a title: both sticks come back here; a click of the left stick does what Settings says";
constexpr const char* PortraitHints = "A  Start      X  Change screens      L  Settings\n"
                                      "Y  Refresh      -  Multiplayer      +  Exit\n"
                                      "In a title: both sticks come back here;\n"
                                      "a click of the left stick does what Settings says";

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
bool online_forced = false; ///< --online-lle
bool online_next = false;   ///< the Online play screen started something: back to it afterwards
std::vector<std::string> script; ///< --sequence (SetScript)
bool scripted = false;
size_t script_at = 0;
int script_layout = -1; ///< the saved arrangement when the script began

/// Where the menu's settings are kept: one small file of text per setting in the emulator's
/// config folder (the screens' arrangement is "hen-layout.txt"). A later setting takes a file
/// name of its own here, and a line in the Settings page's table (SettingsRows, below).
std::string SettingFile(const char* name) {
    return FileUtil::GetUserPath(FileUtil::UserPath::ConfigDir) + name;
}

/// A stored setting's text; nothing if it was never stored.
std::optional<std::string> StoredSetting(const char* name) {
    std::string text;
    if (FileUtil::ReadFileToString(true, SettingFile(name), text) > 0) {
        return text;
    }
    return std::nullopt;
}

void StoreSetting(const char* name, const std::string& text) {
    FileUtil::CreateFullPath(SettingFile(name));
    FileUtil::WriteStringToFile(true, SettingFile(name), text);
}

constexpr const char* LayoutSetting = "hen-layout.txt";

int SavedLayout() {
    if (saved_layout < 0) {
        saved_layout = 0;
        if (const auto text = StoredSetting(LayoutSetting)) {
            saved_layout = std::clamp(std::atoi(text->c_str()), 0, LayoutCount - 1);
        }
    }
    sw_menu_set_portrait(Layouts[saved_layout].upright, Layouts[saved_layout].flipped);
    return saved_layout;
}

void SaveLayout(int layout) {
    saved_layout = layout;
    sw_menu_set_portrait(Layouts[layout].upright, Layouts[layout].flipped);
    StoreSetting(LayoutSetting, std::to_string(layout));
}

/// A setting that is one of a few choices: its file holds the choice's index (the default's
/// when there is no file, so a card without the file behaves as if the setting did not exist).
/// New choices go at the end.
struct Option {
    const char* file;
    int fallback;
    std::vector<const char*> names;
    int chosen = -1;

    int Get() {
        if (chosen < 0) {
            const auto text = StoredSetting(file);
            const int count = static_cast<int>(names.size());
            const int stored = text ? std::atoi(text->c_str()) : fallback;
            chosen = stored >= 0 && stored < count ? stored : fallback;
        }
        return chosen;
    }
    std::string Name() {
        return names[Get()];
    }
    bool Next() {
        chosen = (Get() + 1) % static_cast<int>(names.size());
        StoreSetting(file, std::to_string(chosen));
        return true;
    }
};

// (the order of StickClick's names is TitleMenu::StickAction's)
Option StickClick{"hen-stick-click.txt", 0, {"Next screen arrangement", "Swap screens", "Mute", "Nothing"}};
Option PointerTime{"hen-pointer-time.txt", 1, {"1.5 seconds", "3 seconds", "6 seconds", "10 seconds"}};
Option MouseSpeed{"hen-mouse-speed.txt", 1, {"Slow (0.6x)", "Normal", "Fast (1.5x)", "Very fast (2x)"}};
Option StickSpeed{"hen-stick-speed.txt", 1,
                  {"Slow (across in 1 second)", "Normal (0.7 seconds)", "Fast (0.5 seconds)", "Very fast (0.35 seconds)"}};
Option PointerSize{"hen-pointer-size.txt", 0, {"Normal", "Large (1.5x)", "Very large (2.5x)"}};
// (the order is renderer_vulkan.cpp's, DrawCursor)
Option PointerStyle{"hen-pointer-style.txt", 0,
                    {"Cross that inverts the picture", "White cross with a dark edge", "Ring with a dot",
                     "Arrow", "Black and white crosshair"}};

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
                    "The .cia can be deleted.");
    } else if (status == Status::ErrorEncrypted || IsEncrypted(entry.path)) {
        ShowMessage("Couldn't install", name,
                    "Encrypted. Only decrypted .cia files work.");
    } else {
        ShowMessage("Couldn't install", name,
                    status == Status::ErrorInvalid ? "The file isn't a valid .cia."
                                                   : "The file couldn't be read.");
    }
}

/// A line of text from the person: the system's keyboard, or the menu's own grid of characters
/// where that can't be shown (it then comes back at once, without text). allowed: what the grid
/// offers besides letters and digits.
std::optional<std::string> AskText(const char* title, const char* prompt, const std::string& initial,
                                   unsigned max_length, const char* allowed) {
    std::vector<char> buffer(max_length * 4 + 4);
    const auto asked = std::chrono::steady_clock::now();
    int result = sw_keyboard(prompt, initial.c_str(), max_length, buffer.data(), buffer.size());
    const auto took = std::chrono::steady_clock::now() - asked;
    if (result == 0 && took >= std::chrono::milliseconds(700)) {
        return std::nullopt; // (closed by the person)
    }
    if (result <= 0) {
        std::printf("hen: menu: the system's keyboard didn't show (%d): the menu's own\n", result);
        result = sw_menu_text(title, prompt, initial.c_str(), max_length, allowed, buffer.data(),
                              static_cast<unsigned>(buffer.size()));
        if (result <= 0) {
            return std::nullopt;
        }
    }
    return std::string(buffer.data());
}

/// Joins a room from the Multiplayer screen, asking for the password if the room wants one.
void JoinFromMenu(const std::string& address, u16 port, std::string password) {
    auto& system = Core::System::GetInstance();
    for (int attempt = 0; attempt < 3; attempt++) {
        sw_menu_message("Multiplayer", fmt::format("Joining {} port {}...", address, port).c_str(),
                        nullptr, -1.0f);
        if (RoomLink::JoinRoom(system, address, port, password)) {
            return;
        }
        const std::string why = RoomLink::GetStatus().last;
        if (why != "wrong password") {
            ShowMessage("Couldn't join", fmt::format("{} port {}: {}", address, port, why), "");
            return;
        }
        const auto typed = AskText("Room password", "This room has a password", "", 32, "._-!?@#");
        if (!typed) {
            return;
        }
        password = *typed;
    }
}

/// The rooms a lobby server lists (multiplayer.txt: lobby=...), to join one. False: leave the app.
bool BrowseRooms(const std::string& lobby, const std::string& host) {
    const std::string heading = "Rooms at " + host;
    int at = 0;
    for (;;) {
        // (the request blocks: up to about 12 s for a server that doesn't answer)
        sw_menu_message(heading.c_str(), "Asking for the rooms...", nullptr, -1.0f);
        const RoomList::Result list = RoomList::Fetch(lobby);
        if (!list.ok) {
            ShowMessage("No room list", list.error, "");
            return true;
        }
        std::vector<std::string> titles, details;
        for (const RoomList::Entry& room : list.rooms) {
            titles.push_back(room.name);
            std::string detail = fmt::format("{}/{}", room.players, room.max_players);
            if (!room.preferred_game.empty()) {
                detail += "   " + room.preferred_game;
            }
            if (room.has_password) {
                detail += "   [password]";
            }
            details.push_back(std::move(detail));
        }
        std::vector<sw_menu_item> items;
        for (std::size_t i = 0; i < titles.size(); i++) {
            items.push_back({titles[i].c_str(), details[i].c_str()});
        }
        std::string info = fmt::format("{} room(s)", list.rooms.size());
        if (list.other_version != 0) {
            info += fmt::format("; {} more are for another version", list.other_version);
        }
        const int count = static_cast<int>(items.size());
        bool again = false;
        while (!again) {
            at = std::clamp(at, 0, std::max(count - 1, 0));
            std::string note;
            if (count > 0) {
                const RoomList::Entry& room = list.rooms[at];
                note = fmt::format("{} port {}", room.address, room.port);
                for (std::size_t i = 0; i < room.nicknames.size(); i++) {
                    note += (i ? ", " : ": ") + room.nicknames[i];
                }
                if (note.size() > 160) {
                    note.resize(157);
                    note += "...";
                }
                note += "\n";
            }
            const std::string hints = note + "A  Join      Y  Refresh      B  Back      +  Exit";
            const int action = sw_menu_list(heading.c_str(), items.data(), count, &at, info.c_str(), hints.c_str(),
                                            "No rooms to join.");
            if (action == SW_MENU_EXIT) {
                return false;
            }
            if (action == SW_MENU_BACK || action == SW_MENU_EXTRA) {
                return true;
            }
            if (action == SW_MENU_REFRESH) {
                again = true;
            } else if (action == SW_MENU_SELECT) {
                const RoomList::Entry& room = list.rooms[at];
                std::string password;
                if (room.has_password) {
                    const auto typed = AskText("Room password", "This room has a password", "", 32, "._-!?@#");
                    if (!typed) {
                        continue;
                    }
                    password = *typed;
                }
                JoinFromMenu(room.address, room.port, password);
                if (RoomLink::GetStatus().connected) {
                    return true;
                }
            }
        }
    }
}

/// The rooms found on the local network, to join one. False: leave the app.
bool FindRooms() {
    int at = 0;
    for (;;) {
        sw_menu_message("Rooms on this network", "Looking for rooms...", nullptr, -1.0f);
        std::string note;
        const std::vector<RoomLink::Found> rooms = RoomLink::FindRooms(note);
        std::vector<std::string> titles, details;
        for (const RoomLink::Found& room : rooms) {
            titles.push_back(room.described ? room.name : room.address);
            details.push_back(room.described ? fmt::format("{}/{}   {} port {}", room.members, room.slots,
                                                           room.address, room.port)
                                             : fmt::format("port {}", room.port));
        }
        std::vector<sw_menu_item> items;
        for (std::size_t i = 0; i < titles.size(); i++) {
            items.push_back({titles[i].c_str(), details[i].c_str()});
        }
        const int count = static_cast<int>(items.size());
        const std::string info = !note.empty() ? "Couldn't look: " + note : fmt::format("{} room(s) found", count);
        const std::string empty =
            "No room found on this network.";
        bool again = false;
        while (!again) {
            at = std::clamp(at, 0, std::max(count - 1, 0));
            const int action = sw_menu_list("Rooms on this network", items.data(), count, &at, info.c_str(),
                                            "Over the internet: Join a room by address.\n"
                                            "A  Join      Y  Look again      B  Back      +  Exit",
                                            empty.c_str());
            if (action == SW_MENU_EXIT) {
                return false;
            }
            if (action == SW_MENU_BACK || action == SW_MENU_EXTRA) {
                return true;
            }
            if (action == SW_MENU_REFRESH) {
                again = true;
            } else if (action == SW_MENU_SELECT) {
                JoinFromMenu(rooms[at].address, rooms[at].port, {});
                if (RoomLink::GetStatus().connected) {
                    return true;
                }
            }
        }
    }
}

/// The installed System Settings of the linked console's region (a system application on the
/// emulated console's own storage); empty: not there.
std::string SystemSettingsPath() {
    const auto& secure_info = HW::UniqueData::GetSecureInfoA();
    // Japan, America, Europe, Australia (Europe's), China, Korea, Taiwan.
    static constexpr u32 Low[Core::NUM_SYSTEM_TITLE_REGIONS] = {0x20000, 0x21000, 0x22000, 0x22000,
                                                               0x26000, 0x27000, 0x28000};
    if (!secure_info.IsValid() || secure_info.body.region >= Core::NUM_SYSTEM_TITLE_REGIONS) {
        return {};
    }
    const std::string path = Service::AM::GetTitleContentPath(
        Service::FS::MediaType::NAND, 0x0004001000000000ull | Low[secure_info.body.region]);
    return !path.empty() && FileUtil::Exists(path) ? path : std::string{};
}

std::string HomeMenuPath() {
    const auto& secure_info = HW::UniqueData::GetSecureInfoA();
    if (!secure_info.IsValid() || secure_info.body.region >= Core::NUM_SYSTEM_TITLE_REGIONS) {
        return {};
    }
    const std::string path = Core::GetHomeMenuNcchPath(secure_info.body.region);
    return !path.empty() && FileUtil::Exists(path) ? path : std::string{};
}

/// The Online play screen (online_menu.h) over the emulator. False: leave the app. start: what
/// the person chose to start there, if anything.
bool Online(std::optional<Choice>& start) {
    OnlineMenu::Backend backend;
    backend.read = [] {
        OnlineMenu::LinkState state;
        // (one look at the title folder's names and the saved list, not three walks over
        // every system title)
        Link3ds::Count old_set, new_set;
        const Link3ds::State now = Link3ds::ReadWithCounts(old_set, new_set);
        state.linked = now.console_linked;
        state.old_complete = now.old3ds_installed;
        state.new_complete = now.new3ds_installed;
        if (now.console_linked) {
            state.old_installed = old_set.installed;
            state.old_total = old_set.total;
            state.new_installed = new_set.installed;
            state.new_total = new_set.total;
        }
        return state;
    };
    backend.address = [] { return RoomLink::LinkAddress(); };
    backend.find = [](const std::string& remembered, std::string& note) {
        return ToolScan::Find(remembered, note);
    };
    backend.ask_address = [](const std::string& initial) {
        return AskText("Link a 3DS", "The address on the 3DS's screen", initial, 64,
                       ".:-");
    };
    backend.prepare = [](bool new_set, bool reinstall, OnlineMenu::Prepared& prepared, std::string& error) {
        auto& system = Core::System::GetInstance();
        Link3ds::FixCountry(system);
        if (!Link3ds::Begin("", new_set ? Link3ds::Set::New3ds : Link3ds::Set::Old3ds, reinstall,
                            prepared.program, error)) {
            return false;
        }
        prepared.note = Link3ds::DnsNote();
        return true;
    };
    backend.choose = [](const std::string& address, bool typed, std::string& error) {
        if (!Link3ds::AddressOk(address)) {
            error = "Not an address. Example: 192.168.1.30";
            return false;
        }
        if (typed) {
            ToolScan::Drop();
        } else {
            ToolScan::Keep(address);
        }
        if (address != RoomLink::LinkAddress()) {
            RoomLink::SetSetting("link_address", address);
        }
        std::printf("hen: link: linking from %s (%s)\n", address.c_str(), typed ? "typed" : "found by the search");
        return true;
    };
    backend.drop = [] { ToolScan::Drop(); };
    backend.unlink = [] {
        Link3ds::Unlink();
        if (RoomLink::OnlineLle()) {
            RoomLink::SetSetting("online_lle", "0");
        }
        Settings::values.enable_required_online_lle_modules = online_forced;
    };
    backend.modules_on = [] { return Settings::values.enable_required_online_lle_modules.GetValue(); };
    backend.set_modules = [](bool on) {
        Settings::values.enable_required_online_lle_modules = on;
        RoomLink::SetSetting("online_lle", on ? "1" : "0");
        std::printf("hen: online: the system modules for online play: %s (the Online play screen)\n",
                    on ? "the console's own code" : "the emulator's stand-ins");
    };
    backend.modules_forced = online_forced;
    backend.title_changes = [] { return Service::AM::HenTitleChanges(); };
    backend.settings_path = [] { return SystemSettingsPath(); };
    backend.home_path = [] { return HomeMenuPath(); };

    const OnlineMenu::Outcome outcome = OnlineMenu::Run(backend);
    if (!outcome.start_path.empty()) {
        start = Choice{outcome.start_name, outcome.start_path};
        online_next = true;
    }
    return !outcome.exit;
}

/// The Multiplayer screen. False: leave the app. start: something the Online play screen is to
/// start (the screen is then left).
bool Multiplayer(std::optional<Choice>& start) {
    enum class Do { Find, Host, StopHost, Join, Browse, Leave, Retry, Forget, Recent, Nickname, Online };
    // (a lobby server only if the user has named one: there is no public one to offer)
    const std::string lobby = RoomLink::LobbyUrl();
    const std::string lobby_host = lobby.empty() ? std::string{} : Common::SplitUrl(lobby).host;
    auto& system = Core::System::GetInstance();
    int at = 0;
    for (;;) {
        const RoomLink::Status status = RoomLink::GetStatus();
        const std::vector<RoomLink::Recent> recents = RoomLink::Recents();
        std::vector<std::pair<Do, int>> actions;
        std::vector<std::string> titles, details;
        const auto add = [&](Do what, std::string title, std::string detail = {}, int index = 0) {
            actions.emplace_back(what, index);
            titles.push_back(std::move(title));
            details.push_back(std::move(detail));
        };
        std::string info, note;
        if (status.hosting) {
            add(Do::StopHost, "Stop hosting");
            info = status.own_address.empty()
                       ? "Hosting, but this console has no network address"
                       : fmt::format("Hosting. Others join {} port {}", status.own_address, status.host_port);
        } else if (status.connected) {
            add(Do::Leave, "Leave the room");
            info = fmt::format("In \"{}\" at {} port {}", status.room_name, status.address, status.port);
        } else {
            add(Do::Find, "Find rooms on this network");
            if (status.wanted) {
                add(Do::Retry, fmt::format("Try {} again", status.address));
                add(Do::Forget, fmt::format("Forget {}", status.address));
                info = fmt::format("Not in a room. Retries {} when a title starts", status.address);
            } else {
                info = "Not in a room";
            }
            add(Do::Host, "Host a room on this console");
            add(Do::Join, "Join a room by address");
            if (!lobby_host.empty()) {
                add(Do::Browse, "Browse rooms at " + lobby_host);
            }
            for (std::size_t i = 0; i < recents.size(); i++) {
                add(Do::Recent, fmt::format("Join {} again", recents[i].address),
                    fmt::format("port {}{}", recents[i].port, recents[i].password.empty() ? "" : ", password"),
                    static_cast<int>(i));
            }
            add(Do::Nickname, "Nickname", status.nickname);
        }
        add(Do::Online, "Online play (Pretendo)", "over the internet");
        if (status.connected && !status.members.empty()) {
            note = fmt::format("In the room ({}):", status.members.size());
            for (std::size_t i = 0; i < status.members.size(); i++) {
                note += (i ? ", " : " ") + status.members[i];
            }
        } else if (status.hosting) {
            note = fmt::format("{} in the room; this console is not ({})", status.host_members,
                               status.last.empty() ? "joining" : status.last);
        } else if (!status.last.empty()) {
            note = "Last time: " + status.last;
        } else {
            note = "Local wireless play goes through the room.";
        }
        if (note.size() > 240) {
            note.resize(237);
            note += "...";
        }
        const std::string hints = note + "\nA  Choose      B  Back      +  Exit";
        std::vector<sw_menu_item> items;
        for (std::size_t i = 0; i < titles.size(); i++) {
            items.push_back({titles[i].c_str(), details[i].c_str()});
        }
        const int count = static_cast<int>(items.size());
        at = std::clamp(at, 0, count - 1);
        const int action = sw_menu_list("Multiplayer", items.data(), count, &at, info.c_str(), hints.c_str(), "");
        if (action == SW_MENU_EXIT) {
            return false;
        }
        if (action == SW_MENU_BACK || action == SW_MENU_EXTRA) {
            return true;
        }
        if (action != SW_MENU_SELECT) {
            continue;
        }
        const auto [what, index] = actions[at];
        switch (what) {
        case Do::Host:
            sw_menu_message("Multiplayer", "Opening a room...", nullptr, -1.0f);
            if (!RoomLink::Host(system)) {
                const RoomLink::Status now = RoomLink::GetStatus();
                ShowMessage(now.hosting ? "Room open, but couldn't join it" : "Couldn't host",
                            now.last, "");
            }
            at = 0;
            break;
        case Do::StopHost:
            sw_menu_message("Multiplayer", "Closing the room...", nullptr, -1.0f);
            RoomLink::StopHosting();
            break;
        case Do::Leave:
        case Do::Forget:
            RoomLink::LeaveRoom();
            break;
        case Do::Retry:
            JoinFromMenu(status.address, status.port, {});
            at = 0;
            break;
        case Do::Recent:
            JoinFromMenu(recents[index].address, recents[index].port, recents[index].password);
            at = 0;
            break;
        case Do::Find:
            if (!FindRooms()) {
                return false;
            }
            at = 0;
            break;
        case Do::Browse:
            if (!BrowseRooms(lobby, lobby_host)) {
                return false;
            }
            at = 0;
            break;
        case Do::Join: {
            const auto typed = AskText("Join a room", "The host's address (add :port if it isn't 24872)",
                                       recents.empty() ? std::string{} : recents[0].address, 64, ".:-_");
            if (!typed) {
                break;
            }
            std::string address;
            u16 port = 0;
            if (!RoomLink::ParseAddress(*typed, address, port)) {
                ShowMessage("Couldn't join", "\"" + *typed + "\" isn't an address",
                            "Like 192.168.1.20 or 192.168.1.20:24872.");
                break;
            }
            JoinFromMenu(address, port, {});
            at = 0;
            break;
        }
        case Do::Nickname: {
            const auto typed = AskText("Nickname", "4 to 20 letters, digits, spaces or . _ -", status.nickname,
                                       20, "._-");
            if (typed && !RoomLink::SetNickname(*typed)) {
                ShowMessage("Nickname not changed", "\"" + *typed + "\" doesn't fit",
                            "Rooms take 4 to 20 letters, digits, spaces or . _ -");
            }
            break;
        }
        case Do::Online:
            if (!Online(start)) {
                return false;
            }
            if (start) {
                return true;
            }
            break;
        }
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

const char* NextArrangement() {
    SaveLayout((SavedLayout() + 1) % LayoutCount);
    ApplySavedLayout();
    return Layouts[SavedLayout()].name;
}

StickAction ApplyInputSettings() {
    constexpr unsigned shown_ms[] = {1500, 3000, 6000, 10000};
    constexpr float mouse_speed[] = {0.6f, 1.0f, 1.5f, 2.0f}, stick_travel_s[] = {1.0f, 0.7f, 0.5f, 0.35f},
                    size[] = {1.0f, 1.5f, 2.5f};
    sw_pointer_settings(shown_ms[PointerTime.Get()], mouse_speed[MouseSpeed.Get()], stick_travel_s[StickSpeed.Get()],
                        size[PointerSize.Get()], PointerStyle.Get());
    return static_cast<StickAction>(StickClick.Get());
}

void ShowPage(const std::string& heading, const std::string& text) {
    if (!sw_menu_open()) {
        return;
    }
    SavedLayout();
    sw_menu_page(heading.c_str(), text.c_str(), "A  Continue");
}

void Busy(const std::string& heading, const std::string& line, const std::function<void()>& work) {
    const bool opened = sw_menu_open();
    if (opened) {
        SavedLayout();
        sw_menu_message(heading.c_str(), line.c_str(), nullptr, -1.0f);
    }
    work();
    if (opened) {
        sw_menu_close();
    }
}

void SetScript(std::vector<std::string> matches) {
    script = std::move(matches);
    scripted = true;
    script_at = 0;
}

void SetOnlineForced(bool forced) {
    online_forced = forced;
}

void ShowMessage(const std::string& heading, const std::string& line1, const std::string& line2) {
    if (!sw_menu_open()) {
        return;
    }
    SavedLayout();
    sw_menu_message(heading.c_str(), line1.c_str(), (line2 + "   (A: continue)").c_str(), -1.0f);
    sw_menu_wait_button();
}

namespace {
/// "Clear caches", from the Settings page (so no title is running). False: the menu could not be
/// shown again, or the system asked the app to close.
bool ClearCaches() {
    switch (sw_menu_page("Clear caches?",
                         "Removes the shader caches and the lists of compiled code that Kaizhar has built.\n"
                         "Saves are not touched. The next start of each game will stutter while it rebuilds them.",
                         "A  Clear      B  Keep")) {
    case SW_MENU_EXIT:
        sw_menu_close();
        return false;
    case SW_MENU_SELECT:
        break;
    default:
        return true;
    }
    sw_menu_message("Clear caches", "Clearing...", nullptr, -1.0f);
    // (the menu draws with the driver, which holds its cache file open: closed for the moment)
    sw_menu_close();
    const Caches::Tally tally = Caches::ClearAll();
    if (!sw_menu_open()) {
        return false;
    }
    SavedLayout();
    const std::string removed = fmt::format("Removed {} file{}, {:.1f} MiB.", tally.files, tally.files == 1 ? "" : "s",
                                            static_cast<double>(tally.bytes) / (1024.0 * 1024.0));
    if (tally.failed == 0) {
        ShowMessage("Caches cleared", removed, "");
    } else {
        std::string name = tally.first_failed;
        if (name.size() > 60) {
            name = "..." + name.substr(name.size() - 57);
        }
        ShowMessage("Caches partly cleared",
                    removed + fmt::format(" {} could not be removed.", tally.failed), "First: " + name);
    }
    return true;
}

/// One line of the Settings page. A later setting is one more entry in SettingsRows.
struct SettingRow {
    const char* label;
    std::string (*value)(); ///< the text at the right: what is set now; null: none
    bool (*choose)();       ///< A on the line; false: leave the app. null: a line to read only
};

bool NextLayout() {
    SaveLayout((SavedLayout() + 1) % LayoutCount);
    return true;
}

const SettingRow SettingsRows[] = {
    {"Screens", [] { return std::string(Layouts[SavedLayout()].name); }, NextLayout},
    {"Left stick click", [] { return StickClick.Name(); }, [] { return StickClick.Next(); }},
    {"Pointer stays for", [] { return PointerTime.Name(); }, [] { return PointerTime.Next(); }},
    {"Joy-Con mouse speed", [] { return MouseSpeed.Name(); }, [] { return MouseSpeed.Next(); }},
    {"Stick pointer speed", [] { return StickSpeed.Name(); }, [] { return StickSpeed.Next(); }},
    {"Pointer size", [] { return PointerSize.Name(); }, [] { return PointerSize.Next(); }},
    {"Pointer style", [] { return PointerStyle.Name(); }, [] { return PointerStyle.Next(); }},
    {"Clear caches", [] { return std::string("to judge a first start again"); }, ClearCaches},
    // ("2126.2-kaizhar3", with " dev" after it in a development build)
    {"Version", [] { return std::string(SW_VERSION_TEXT + sizeof("Kaizhar")); }, nullptr},
};

/// The Settings page, from the title list (so no title is running: "Clear caches" needs that).
/// False: leave the app (the menu is closed then, or could not be shown again).
bool SettingsPage() {
    constexpr int count = static_cast<int>(std::size(SettingsRows));
    int at = 0;
    for (;;) {
        std::array<std::string, count> values;
        std::array<sw_menu_item, count> items{};
        for (int i = 0; i < count; i++) {
            if (SettingsRows[i].value) {
                values[i] = SettingsRows[i].value();
            }
            items[i] = {SettingsRows[i].label, values[i].c_str(), SettingsRows[i].choose == nullptr};
        }
        const int action = sw_menu_list("Settings", items.data(), count, &at, nullptr,
                                        "A  Choose      B  Back      +  Exit", "");
        if (action == SW_MENU_EXIT) {
            sw_menu_close();
            return false;
        }
        if (action == SW_MENU_BACK || action == SW_MENU_SETTINGS) {
            return true;
        }
        if (action == SW_MENU_SELECT && SettingsRows[at].choose && !SettingsRows[at].choose()) {
            return false;
        }
    }
}
} // namespace

std::optional<Choice> Run() {
    if (!sw_menu_open()) {
        return std::nullopt;
    }
    SavedLayout();
    const auto start = [](const Choice& choice) {
        ApplySavedLayout();
        sw_menu_message(choice.name.c_str(), "Starting...", nullptr, -1.0f);
        sw_menu_close();
        return choice;
    };
    // Back from something the Online play screen started (a setup, System Settings): there again.
    if (online_next) {
        online_next = false;
        std::optional<Choice> chosen;
        if (!Online(chosen)) {
            sw_menu_close();
            return std::nullopt;
        }
        if (chosen) {
            return start(*chosen);
        }
    }
    sw_menu_message("Kaizhar", "Looking for titles...", nullptr, -1.0f);
    std::vector<Entry> entries = Scan();
    if (scripted) {
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return std::tolower(c); });
            return text;
        };
        if (script_layout < 0) {
            script_layout = SavedLayout();
        }
        if (script_at < script.size()) {
            const std::string want = lower(script[script_at++]);
            for (const Entry& entry : entries) {
                if (!entry.install && lower(entry.path + "\n" + entry.name).find(want) != std::string::npos) {
                    std::printf("hen: sequence: title %zu of %zu: the list picks %s (%s)\n", script_at, script.size(),
                                entry.name.c_str(), entry.path.c_str());
                    return start(Choice{entry.name, entry.path});
                }
            }
            std::printf("hen: sequence: nothing in the list has '%s': leaving\n", want.c_str());
        } else {
            std::printf("hen: sequence: done\n");
        }
        if (SavedLayout() != script_layout) {
            SaveLayout(script_layout);
            std::printf("hen: sequence: saved arrangement put back to %d\n", script_layout);
        }
        sw_menu_close();
        return std::nullopt;
    }
    for (;;) {
        std::vector<sw_menu_item> items;
        items.reserve(entries.size());
        for (const Entry& entry : entries) {
            items.push_back({entry.name.c_str(), entry.detail.c_str()});
        }
        const int count = static_cast<int>(items.size());
        selected = std::clamp(selected, 0, std::max(count - 1, 0));
        // A multiplayer room, if one is set up: one more line above the hints.
        std::string hints = Layouts[SavedLayout()].upright ? PortraitHints : Hints;
        const std::string room = RoomLink::StatusLine();
        if (!room.empty()) {
            hints = room + "\n" + hints;
        }
        const int action = sw_menu_frame("Kaizhar", items.data(), count, &selected,
                                         Layouts[SavedLayout()].name, hints.c_str());
        switch (action) {
        case SW_MENU_SELECT: {
            const Entry entry = entries[selected];
            if (entry.install) {
                Install(entry);
                entries = Scan();
                break;
            }
            return start(Choice{entry.name, entry.path});
        }
        case SW_MENU_OPTION:
            SaveLayout((SavedLayout() + 1) % LayoutCount);
            break;
        case SW_MENU_SETTINGS:
            // ("Clear caches" is there, and the page only here: never while a title runs)
            if (!SettingsPage()) {
                return std::nullopt;
            }
            break;
        case SW_MENU_REFRESH:
            entries = Scan();
            break;
        case SW_MENU_EXTRA: {
            std::optional<Choice> chosen;
            if (!Multiplayer(chosen)) {
                sw_menu_close();
                return std::nullopt;
            }
            if (chosen) {
                return start(*chosen);
            }
            // (Nimbus may have been put on the card meanwhile, or a set installed)
            break;
        }
        case SW_MENU_EXIT:
            sw_menu_close();
            return std::nullopt;
        default:
            break;
        }
    }
}

} // namespace TitleMenu
