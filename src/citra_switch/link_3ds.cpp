// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>
#include <cstring>
#include <string>


#include <fmt/format.h>

#include "citra_switch/guest_dns.h"
#include "citra_switch/link_3ds.h"
#include "common/file_util.h"
#include "common/logging/log.h"
#include "core/hle/service/am/am.h"
#include "core/hle/service/cfg/cfg.h"
#include "core/hle/service/fs/archive.h"
#include "core/hw/unique_data.h"
#include "core/system_titles.h"

namespace Link3ds {

namespace {

/// The loader's prefixes for the two setups (core/loader/loader.cpp).
constexpr const char* Old3dsPrefix = "articinio://";
constexpr const char* New3dsPrefix = "articinin://";
/// The server the emulated installer downloads the titles' contents from. The others it asks
/// (the update list, the tickets) are under the same domain.
constexpr const char* UpdateServer = "nus.cdn.c.shop.nintendowifi.net";

std::string dns_note;

void Say(const Progress& progress, const std::string& line) {
    std::printf("hen: link: %s\n", line.c_str());
    LOG_INFO(Frontend, "link: {}", line);
    if (progress) {
        progress(line);
    }
}

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// "host" or "host:port" with a port from 1 to 65535, nothing a path could hide in.
bool AddressFits(const std::string& address) {
    if (address.empty() || address.size() > 80) {
        return false;
    }
    const auto colon = address.find(':');
    const std::string host = address.substr(0, colon);
    if (host.empty()) {
        return false;
    }
    for (const char c : host) {
        const bool fits = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!fits) {
            return false;
        }
    }
    if (colon == std::string::npos) {
        return true;
    }
    const std::string port = address.substr(colon + 1);
    if (port.empty() || port.size() > 5) {
        return false;
    }
    unsigned value = 0;
    for (const char c : port) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value >= 1 && value <= 65535;
}

Core::SystemTitleSet TitleSet(Set set) {
    return set == Set::Old3ds ? Core::SystemTitleSet::Old3ds : Core::SystemTitleSet::New3ds;
}

const char* SetName(Set set) {
    return set == Set::Old3ds ? "Old 3DS" : "New 3DS";
}

} // namespace

State Read() {
    State state;
    // (the files are read again: a setup may have just written them)
    HW::UniqueData::InvalidateSecureData();
    state.console_linked = HW::UniqueData::IsFullConsoleLinked();
    const auto installed = Core::AreSystemTitlesInstalled();
    state.old3ds_installed = installed.first;
    state.new3ds_installed = installed.second;
    return state;
}

State ReadWithCounts(Count& old_set, Count& new_set) {
    State state;
    old_set = {};
    new_set = {};
    HW::UniqueData::InvalidateSecureData();
    state.console_linked = HW::UniqueData::IsFullConsoleLinked();
    Service::AM::HenNandTitles titles = Service::AM::HenReadNandTitles();
    const auto has = [&titles](u64 title_id) { return titles.HasMetadata(title_id); };
    const auto installed = Core::AreSystemTitlesInstalled(has);
    state.old3ds_installed = installed.first;
    state.new3ds_installed = installed.second;
    const auto& secure_info = HW::UniqueData::GetSecureInfoA();
    if (state.console_linked && secure_info.IsValid() && secure_info.body.region < Core::NUM_SYSTEM_TITLE_REGIONS) {
        for (const Set set : {Set::Old3ds, Set::New3ds}) {
            Count& count = set == Set::Old3ds ? old_set : new_set;
            for (const u64 title_id : Core::GetSystemTitleIds(TitleSet(set), secure_info.body.region)) {
                count.total++;
                count.installed += has(title_id) ? 1 : 0;
            }
        }
    }
#if HEN_DEV
    std::printf("hen: link: what is installed: %zu title folders in nand, %s%s; %zu answers needed the card\n",
                titles.folders.size(),
                titles.from_saved_list ? "the saved list of installed titles fits" : "no saved list: ",
                titles.from_saved_list ? "" : titles.why_no_list, titles.asked_card);
#endif
    return state;
}

Count Installed(Set set) {
    Count count;
    const auto& secure_info = HW::UniqueData::GetSecureInfoA();
    if (!secure_info.IsValid() || secure_info.body.region >= Core::NUM_SYSTEM_TITLE_REGIONS) {
        return count;
    }
    for (const u64 title_id : Core::GetSystemTitleIds(TitleSet(set), secure_info.body.region)) {
        count.total++;
        if (FileUtil::Exists(Service::AM::GetTitleMetadataPath(Service::FS::MediaType::NAND,
                                                               title_id, false))) {
            count.installed++;
        }
    }
    return count;
}

bool UpdateServersBlocked() {
    return GuestDns::Resolve(UpdateServer).how != GuestDns::How::System;
}

bool AddressOk(const std::string& address) {
    return AddressFits(Trim(address));
}

bool IsSetupProgram(const std::string& program) {
    return program.rfind(Old3dsPrefix, 0) == 0 || program.rfind(New3dsPrefix, 0) == 0;
}

std::string DnsNote() {
    return dns_note;
}

Set SetOf(const std::string& program) {
    return program.rfind(New3dsPrefix, 0) == 0 ? Set::New3ds : Set::Old3ds;
}

bool Begin(const std::string& address_given, Set set, bool reinstall, std::string& program,
           std::string& error, const Progress& progress) {
    program.clear();
    error.clear();
    const std::string address = Trim(address_given);
    if (!address.empty() && !AddressFits(address)) {
        error = "Not an address. Example: 192.168.1.30";
        return false;
    }
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    error = "This build has no HTTPS.";
    return false;
#endif

    const State state = Read();
    if (set == Set::New3ds && !state.old3ds_installed) {
        error = "The Old 3DS set comes first.";
        return false;
    }

    const bool again = set == Set::Old3ds ? state.old3ds_installed : state.new3ds_installed;
    if (again && !reinstall) {
        error = fmt::format("The {} set is already there.", SetName(set));
        return false;
    }

    Say(progress, "Checking that Nintendo's update servers can be found...");
    const GuestDns::Answer found = GuestDns::Resolve(UpdateServer);
    if (found.how == GuestDns::How::None) {
        error = "Nintendo's update servers can't be found.\nCheck the internet connection.";
        std::printf("hen: link: %s: the console's DNS says %s, and no name server asked directly answered\n",
                    UpdateServer, found.system.c_str());
        return false;
    }
    dns_note.clear();
    if (found.how == GuestDns::How::Own) {
        dns_note = fmt::format("DNS: asking {} for this setup only.", found.server);
        std::printf("hen: link: the console's DNS says %s for %s: the emulated console's names are asked of %s "
                    "during this setup\n",
                    found.system.c_str(), UpdateServer, found.server.c_str());
        Say(progress, dns_note);
    }

    // As the desktop does before every setup: titles of this set left by an earlier, unfinished
    // run would make the emulated console think it is up to date.
    if (again) {
        Say(progress, fmt::format("Removing the installed {} system titles...", SetName(set)));
    }
    Core::UninstallSystemFiles(TitleSet(set));

    program = (set == Set::Old3ds ? Old3dsPrefix : New3dsPrefix) + address;
    if (!address.empty()) {
        Say(progress, fmt::format("Connecting to the Artic Setup Tool at {} ({} setup)...", address,
                                  SetName(set)));
    }
    return true;
}

bool End(Set set, Core::System::ResultStatus last_status, const std::string& status_details,
         std::string& error, const Progress& progress) {
    error.clear();
    const State state = Read();
    const Count count = Installed(set);
    const bool complete = set == Set::Old3ds ? state.old3ds_installed : state.new3ds_installed;

    if (count.total != 0) {
        Say(progress, fmt::format("Console {}. {} system titles: {} of {}.",
                                  state.console_linked ? "linked" : "not linked", SetName(set),
                                  count.installed, count.total));
    }
    if (complete && state.console_linked) {
        Say(progress, fmt::format("{} setup complete.", SetName(set)));
        return true;
    }

    if (!state.console_linked) {
        // Nothing or not everything was copied in step 1.
        if (last_status == Core::System::ResultStatus::ErrorArticDisconnected) {
            error = "Couldn't connect to the 3DS.\nClose and reopen the setup tool, then try again.";
            const std::string details = Trim(status_details);
            if (!details.empty()) {
                error += "\n(" + details + ")";
            }
        } else {
            error = "The 3DS's files couldn't be copied.\nSee the tool's screen on the 3DS.";
        }
        return false;
    }
    if (last_status == Core::System::ResultStatus::ErrorArticDisconnected) {
        error = fmt::format("Connection lost at {} of {}.\nLink again.", count.installed, count.total);
    } else if (count.installed == 0) {
        error = "Nothing was downloaded.\nLink again and choose OK on the update screen.";
    } else {
        error = fmt::format("Stopped at {} of {}.\nLink again.", count.installed, count.total);
    }
    return false;
}

bool FixCountry(Core::System& system, const Progress& progress) {
    const auto& secure_info = HW::UniqueData::GetSecureInfoA();
    if (!secure_info.IsValid()) {
        return false;
    }
    const u32 region = secure_info.body.region;
    const auto cfg = Service::CFG::GetModule(system);
    if (!cfg || Service::CFG::Module::IsValidRegionCountry(region, cfg->GetCountryCode())) {
        return false;
    }
    // Japan, United States, United Kingdom, Australia, China, South Korea, Taiwan.
    static constexpr u8 UsualCountry[Core::NUM_SYSTEM_TITLE_REGIONS] = {1, 49, 110, 65, 160, 136, 128};
    if (region >= Core::NUM_SYSTEM_TITLE_REGIONS) {
        return false;
    }
    cfg->SetCountryCode(UsualCountry[region]);
    cfg->UpdateConfigNANDSavegame();
    Say(progress, "The emulated console's country did not fit the linked console's region: "
                  "set to the region's usual one.");
    return true;
}

bool Run(Core::System& system, const std::string& address, Set set, bool reinstall,
         const RunTitle& run_title, std::string& error, const Progress& progress) {
    FixCountry(system, progress);
    std::string program;
    if (!Begin(address, set, reinstall, program, error, progress)) {
        return false;
    }
    RunResult result = run_title(program);
    // A console linked just now, with a country that stopped step 2: once more.
    if (FixCountry(system, progress)) {
        Say(progress, "Connecting again...");
        result = run_title(program);
    }
    return End(set, result.status, result.details, error, progress);
}

void Fresh(Core::System& system) {
    const State before = Read();
    const Count old_set = Installed(Set::Old3ds), new_set = Installed(Set::New3ds);
    const auto cfg = Service::CFG::GetModule(system);
    const unsigned country = cfg ? cfg->GetCountryCode() : 0;
    std::printf("hen: link: fresh: before: console %s, Old 3DS set %u of %u, New 3DS set %u of %u, country %u\n",
                before.console_linked ? "linked" : "not linked", old_set.installed, old_set.total,
                new_set.installed, new_set.total, country);
    // The system titles of both sets (the emulated console's own storage: nand/.../title).
    Core::UninstallSystemFiles(Core::SystemTitleSet::Old3ds);
    Core::UninstallSystemFiles(Core::SystemTitleSet::New3ds);
    // The console's identity: otp.bin, SecureInfo_A, LocalFriendCodeSeed_B and the account,
    // installer and friends savegames (what Unlink removes), and movable.sed, which it keeps.
    const std::string files[] = {HW::UniqueData::GetOTPPath(), HW::UniqueData::GetSecureInfoAPath(),
                                 HW::UniqueData::GetLocalFriendCodeSeedBPath(),
                                 HW::UniqueData::GetMovablePath()};
    for (const std::string& file : files) {
        std::printf("hen: link: fresh: %s: %s\n", file.c_str(), FileUtil::Exists(file) ? "removed" : "was not there");
    }
    HW::UniqueData::UnlinkConsole();
    FileUtil::Delete(HW::UniqueData::GetMovablePath());
    HW::UniqueData::InvalidateSecureData();
    // The country as a new card has it: the United States.
    if (cfg && country != 49) {
        cfg->SetCountryCode(49);
        cfg->UpdateConfigNANDSavegame();
    }
    const State after = Read();
    std::printf("hen: link: fresh: after: console %s, sets installed %d %d, country %u (was %u)\n",
                after.console_linked ? "LINKED STILL" : "not linked", after.old3ds_installed ? 1 : 0,
                after.new3ds_installed ? 1 : 0, cfg ? cfg->GetCountryCode() : 0u, country);
}

void Unlink() {
    HW::UniqueData::UnlinkConsole();
    HW::UniqueData::InvalidateSecureData();
}

} // namespace Link3ds
