// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdio>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include "citra_switch/online_menu.h"
#include "citra_switch/switch_menu.h"
#include "citra_switch/switch_platform.h"

namespace OnlineMenu {

namespace {

constexpr const char* Heading = "Online play (Pretendo)";

/// A page with "A  Continue". False: B.
enum class Page { Continue, Back, Exit };
Page Show(const std::string& heading, const std::string& text, const char* hints) {
    switch (sw_menu_page(heading.c_str(), text.c_str(), hints)) {
    case SW_MENU_SELECT:
        return Page::Continue;
    case SW_MENU_EXIT:
        return Page::Exit;
    default:
        return Page::Back;
    }
}

std::string Sets(const LinkState& state) {
    if (!state.linked) {
        return "No 3DS linked";
    }
    return fmt::format("3DS linked.   Old set {}/{}   New set {}/{}", state.old_installed, state.old_total,
                       state.new_installed, state.new_total);
}

/// Why the modules' switch can't be used; empty: it can.
std::string ModulesLack(const LinkState& state) {
    if (!state.linked) {
        return "link a 3DS first";
    }
    if (!state.old_complete) {
        return "Old 3DS set missing";
    }
    if (!state.new_complete) {
        return "New 3DS set missing";
    }
    return {};
}

/// "Link a 3DS", from the warnings to a program ready to start. Nothing started: outcome stays
/// empty.
void Link(const Backend& backend, const LinkState& state, Outcome& outcome) {
    // Which set: the Old 3DS one until it is complete, then the New 3DS one; with both there,
    // either again.
    bool new_set = state.old_complete;
    bool reinstall = false;
    if (state.old_complete && state.new_complete) {
        const sw_menu_item items[] = {{"Old 3DS set again", nullptr, false},
                                      {"New 3DS set again", nullptr, false}};
        int at = 0;
        for (;;) {
            const int action = sw_menu_list("Link a 3DS", items, 2, &at, "Both sets are complete",
                                            "The set is downloaded again.\n"
                                            "A  Choose      B  Back      +  Exit",
                                            "");
            if (action == SW_MENU_EXIT) {
                outcome.exit = true;
                return;
            }
            if (action == SW_MENU_BACK || action == SW_MENU_EXTRA) {
                return;
            }
            if (action == SW_MENU_SELECT) {
                break;
            }
        }
        new_set = at == 1;
        reinstall = true;
    }
    const std::string heading = new_set && !reinstall ? "Link a 3DS (set 2 of 2)" : "Link a 3DS";

    const std::string before = fmt::format("{}"
                                           "- Open the Artic Setup Tool on your 3DS and keep it open.\n"
                                           "- Your 3DS's ID files will be saved on this SD card.\n"
                                           "- Never go online on the 3DS and here at the same time.",
                                           new_set ? "New 3DS set.\n" : "");
    switch (Show(heading, before, "A  Continue      B  Back")) {
    case Page::Exit:
        outcome.exit = true;
        return;
    case Page::Back:
        return;
    default:
        break;
    }

    sw_menu_message(heading.c_str(), "Checking...", nullptr, -1.0f);
    Prepared prepared;
    std::string error;
    if (!backend.prepare(new_set, reinstall, prepared, error)) {
        if (Show("Couldn't start the setup", error, "A  Continue") == Page::Exit) {
            outcome.exit = true;
        }
        return;
    }
    std::string ready = new_set ? "On the next screen choose OK and accept.\n"
                                  "It comes back here when done. Keep the 3DS on."
                                : "On the next screen choose OK and accept.\n"
                                  "It does that twice (two sets), then comes back here. Keep the 3DS on.";
    if (!prepared.note.empty()) {
        ready += "\n" + prepared.note;
    }
    // (no way back from here: the check above has already removed what an earlier run of this
    // set left)
    for (;;) {
        const Page page = Show(heading, ready, "A  Start");
        if (page == Page::Exit) {
            outcome.exit = true;
            return;
        }
        if (page == Page::Continue) {
            break;
        }
    }

    // The 3DS is looked for last: the connection the search makes to it is the one the setup
    // uses, and the tool should not wait on it for long. Typing the address is the way out when
    // the 3DS isn't found; nothing is connected to a typed address before the setup itself.
    std::string address;
    while (address.empty()) {
        sw_menu_message(heading.c_str(), "Looking for the 3DS...", nullptr, -1.0f);
        std::string why;
        const std::vector<std::string> found = backend.find(backend.address(), why);
        bool typed = false;
        if (found.size() == 1) {
            address = found[0];
        } else {
            std::vector<sw_menu_item> rows;
            for (const std::string& one : found) {
                rows.push_back({one.c_str(), "3DS", false});
            }
            rows.push_back({"Search again", nullptr, false});
            rows.push_back({"Type the address", nullptr, false});
            const std::string info =
                !why.empty()    ? "Couldn't look: " + why
                : found.empty() ? "No 3DS found. Tool open? Same wifi?"
                                : fmt::format("{} found", found.size());
            const int count = static_cast<int>(rows.size());
            int at = 0;
            for (;;) {
                const int action = sw_menu_list(heading.c_str(), rows.data(), count, &at, info.c_str(),
                                                "Tool used before? Close and reopen it on the 3DS.\n"
                                                "A  Choose      B  Back      +  Exit",
                                                "");
                if (action == SW_MENU_EXIT || action == SW_MENU_BACK || action == SW_MENU_EXTRA) {
                    backend.drop();
                    outcome.exit = action == SW_MENU_EXIT;
                    return;
                }
                if (action == SW_MENU_SELECT) {
                    break;
                }
            }
            if (at < static_cast<int>(found.size())) {
                address = found[at];
            } else if (at == count - 1) {
                backend.drop();
                const auto text = backend.ask_address(backend.address());
                if (!text) {
                    continue;
                }
                address = *text;
                typed = true;
            } else {
                continue; // (search again; the search lets go of what it held)
            }
        }
        if (!backend.choose(address, typed, error)) {
            if (Show("Link a 3DS", error, "A  Continue") == Page::Exit) {
                outcome.exit = true;
                return;
            }
            address.clear();
        }
    }
    sw_menu_message(heading.c_str(), ("Connecting to " + address + "...").c_str(), nullptr, -1.0f);
    outcome.start_name = heading;
    outcome.start_path = prepared.program + address;
}

} // namespace

Outcome Run(const Backend& backend) {
    enum class Do { Link, Modules, Settings, Home, Unlink };
    Outcome outcome;
    int at = 0;
    bool read = true;
    // What the screen shows is worked out from hundreds of small files (seconds on a full card),
    // so it is kept for the program's run: looked at again only after a title was installed or
    // removed, and after an unlink here.
    static struct {
        bool have = false;
        unsigned changes = 0;
        LinkState state;
        std::string settings_path, home_path;
    } kept;
    LinkState state;
    std::string settings_path, home_path;
    for (;;) {
        if (read && (!kept.have || kept.changes != backend.title_changes())) {
            sw_menu_message(Heading, "Looking at what is set up...", nullptr, -1.0f);
            const uint64_t t0 = sw_time_ms();
            kept.state = backend.read();
            const uint64_t t1 = sw_time_ms();
            kept.settings_path = kept.state.linked ? backend.settings_path() : std::string{};
            const uint64_t t2 = sw_time_ms();
            kept.home_path = kept.state.linked ? backend.home_path() : std::string{};
#if HEN_DEV
            std::printf("hen: online menu: what is set up: %llu ms; System Settings' path %llu ms; HOME Menu's path %llu ms\n",
                        static_cast<unsigned long long>(t1 - t0), static_cast<unsigned long long>(t2 - t1),
                        static_cast<unsigned long long>(sw_time_ms() - t2));
#endif
            kept.changes = backend.title_changes();
            kept.have = true;
        }
        if (read) {
            state = kept.state;
            settings_path = kept.settings_path;
            home_path = kept.home_path;
        }
        const std::string lack = ModulesLack(state);
        const bool modules = backend.modules_on();
        const bool ready = lack.empty();
        if (read) {
            // The cursor starts on the next step: linking, then the modules, then the account.
            at = !ready ? 0 : !modules && !backend.modules_forced ? 1 : 2;
            read = false;
        }

        std::vector<Do> actions;
        std::vector<std::pair<std::string, std::string>> texts;
        std::vector<bool> dim;
        const auto add = [&](Do what, std::string title, std::string detail = {}, bool grey = false) {
            actions.push_back(what);
            texts.emplace_back(std::move(title), std::move(detail));
            dim.push_back(grey);
        };
        add(Do::Link, "Link a 3DS",
            !state.linked           ? "not linked"
            : !state.old_complete   ? "2 sets to go"
            : !state.new_complete   ? "1 set to go"
                                    : "done");
        add(Do::Modules, "Online system modules",
            backend.modules_forced ? "on (launch option)" : !lack.empty() ? lack : modules ? "on" : "off",
            !lack.empty() && !backend.modules_forced);
        if (!settings_path.empty()) {
            add(Do::Settings, "System Settings", "Pretendo ID", !ready);
        }
        if (!home_path.empty()) {
            add(Do::Home, "HOME Menu", "friend list", !ready);
        }
        if (state.linked) {
            add(Do::Unlink, "Unlink the 3DS");
        }

        std::string note;
        if (!state.linked) {
            note = "Needs your own 3DS, once.";
        } else if (!lack.empty()) {
            note = "Next: Link a 3DS.";
        } else if (!modules && !backend.modules_forced) {
            note = "Next: turn the online system modules on.";
        } else {
            note = "Next: install Nimbus (nimbus.cia in roms, its 3ds folder in sdmc), then System Settings.";
        }
        const std::string hints = note + "\nA  Choose      B  Back      +  Exit";
        std::vector<sw_menu_item> items;
        for (std::size_t i = 0; i < texts.size(); i++) {
            items.push_back({texts[i].first.c_str(), texts[i].second.c_str(), dim[i]});
        }
        const int count = static_cast<int>(items.size());
        at = std::clamp(at, 0, count - 1);
        const std::string info = Sets(state);
        const int action = sw_menu_list(Heading, items.data(), count, &at, info.c_str(), hints.c_str(), "");
        if (action == SW_MENU_EXIT) {
            outcome.exit = true;
            return outcome;
        }
        if (action == SW_MENU_BACK || action == SW_MENU_EXTRA) {
            return outcome;
        }
        if (action != SW_MENU_SELECT) {
            continue;
        }
        switch (actions[at]) {
        case Do::Link:
            Link(backend, state, outcome);
            if (outcome.exit || !outcome.start_path.empty()) {
                return outcome;
            }
            break;
        case Do::Modules:
            if (backend.modules_forced) {
                if (Show("Online system modules", "On for this start (--online-lle).", "A  Continue") ==
                    Page::Exit) {
                    outcome.exit = true;
                    return outcome;
                }
            } else if (!lack.empty()) {
                if (Show("Online system modules", "Not ready: " + lack + ".", "A  Continue") == Page::Exit) {
                    outcome.exit = true;
                    return outcome;
                }
            } else {
                backend.set_modules(!modules);
                if (!modules && !settings_path.empty()) {
                    at = 2; // (on: the account is next)
                }
            }
            break;
        case Do::Settings:
        case Do::Home:
            if (!ready) {
                if (Show(actions[at] == Do::Settings ? "System Settings" : "HOME Menu",
                         "Not ready: " + lack + ".\nLink a 3DS first.", "A  Continue") == Page::Exit) {
                    outcome.exit = true;
                    return outcome;
                }
                at = 0;
                break;
            }
            if (actions[at] == Do::Home) {
                outcome.start_name = "HOME Menu";
                outcome.start_path = home_path;
                return outcome;
            }
            outcome.start_name = "System Settings";
            outcome.start_path = settings_path;
            return outcome;
        case Do::Unlink:
            switch (Show("Unlink the 3DS?",
                         "The account and friend list made here are removed.\n"
                         "The system software stays.",
                         "A  Unlink      B  Keep")) {
            case Page::Exit:
                outcome.exit = true;
                return outcome;
            case Page::Continue:
                sw_menu_message(Heading, "Unlinking...", nullptr, -1.0f);
                backend.unlink();
                kept.have = false;
                read = true;
                at = 0;
                break;
            default:
                break;
            }
            break;
        }
    }
}

} // namespace OnlineMenu
