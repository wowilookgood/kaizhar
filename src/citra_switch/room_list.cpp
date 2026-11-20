// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <algorithm>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <httplib.h>
#include <json.hpp>

#include "citra_switch/room_list.h"
#include "citra_switch/tls/ca_bundle.h"
#include "common/logging/log.h"
#include "common/settings.h"
#include "common/web_util.h"
#include "network/room.h"

namespace RoomList {

namespace {

/// The lobby protocol's version, sent as a header with every request (web_backend.cpp).
constexpr const char* ApiVersion = "1";
constexpr int ConnectSeconds = 6;
constexpr int TransferSeconds = 6;
/// A lobby with thousands of rooms would still be far below this.
constexpr std::size_t LongestAnswer = 4 * 1024 * 1024;

std::string Trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

/// A value that may be missing or of another type in a lobby's answer.
template <typename T>
T Field(const nlohmann::json& object, const char* key, T fallback) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    try {
        return it->get<T>();
    } catch (const nlohmann::json::exception&) {
        return fallback;
    }
}

/// Text for a menu drawn with a small font: no control characters, a bounded length.
std::string Shown(std::string text, std::size_t longest) {
    for (char& c : text) {
        if (static_cast<unsigned char>(c) < 0x20) {
            c = ' ';
        }
    }
    text = Trim(text);
    if (text.size() > longest) {
        // Not inside a UTF-8 sequence.
        std::size_t end = longest;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
            end--;
        }
        text.resize(end);
    }
    return text;
}

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
/// Why the server's certificate was refused, when that is something the user can act on.
std::string CertificateFailure(long verify_result) {
    switch (verify_result) {
    case X509_V_ERR_CERT_NOT_YET_VALID:
    case X509_V_ERR_CERT_HAS_EXPIRED:
        return "the lobby's certificate is outside its dates: is the console's clock right?";
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        return "the lobby's certificate is from an authority this build doesn't know "
               "(/switch/Kaizhar/cacert.pem can add one)";
    case X509_V_ERR_HOSTNAME_MISMATCH:
        return "the lobby's certificate is for another name";
    default:
        return fmt::format("the lobby's certificate was refused ({})",
                           X509_verify_cert_error_string(verify_result));
    }
}
#endif

Result Failure(std::string text) {
    std::printf("hen: room list: %s\n", text.c_str());
    LOG_ERROR(Network, "room list: {}", text);
    Result result;
    result.error = std::move(text);
    return result;
}

} // namespace

Result Parse(const std::string& json_text) {
    Result result;
    nlohmann::json root;
    try {
        root = nlohmann::json::parse(json_text);
    } catch (const nlohmann::json::exception&) {
        return Failure("the lobby's answer is not a room list");
    }
    const auto rooms = root.find("rooms");
    if (!root.is_object() || rooms == root.end() || !rooms->is_array()) {
        return Failure("the lobby's answer is not a room list");
    }

    for (const auto& room : *rooms) {
        if (!room.is_object()) {
            continue;
        }
        Entry entry;
        entry.address = Trim(Field<std::string>(room, "address", ""));
        entry.port = Field<u16>(room, "port", 0);
        if (entry.address.empty() || entry.port == 0) {
            continue;
        }
        if (Field<u32>(room, "netVersion", 0) != Network::network_version) {
            result.other_version++;
            continue;
        }
        entry.name = Shown(Field<std::string>(room, "name", ""), 60);
        entry.description = Shown(Field<std::string>(room, "description", ""), 200);
        entry.owner = Shown(Field<std::string>(room, "owner", ""), 40);
        entry.max_players = Field<u32>(room, "maxPlayers", 0);
        entry.has_password = Field<bool>(room, "hasPassword", false);
        entry.preferred_game = Shown(Field<std::string>(room, "preferredGameName", ""), 60);
        // (the rooms' word for "no game named")
        if (entry.preferred_game == "%none%") {
            entry.preferred_game.clear();
        }
        entry.preferred_game_id = Field<u64>(room, "preferredGameId", 0);
        const auto players = room.find("players");
        if (players != room.end() && players->is_array()) {
            for (const auto& player : *players) {
                if (player.is_object()) {
                    entry.nicknames.push_back(Shown(Field<std::string>(player, "nickname", ""), 20));
                }
            }
        }
        entry.players = static_cast<u32>(entry.nicknames.size());
        if (entry.name.empty()) {
            entry.name = fmt::format("{}:{}", entry.address, entry.port);
        }
        result.rooms.push_back(std::move(entry));
    }

    std::stable_sort(result.rooms.begin(), result.rooms.end(),
                     [](const Entry& a, const Entry& b) { return a.players > b.players; });
    result.ok = true;
    return result;
}

Result Fetch(const std::string& lobby_url) {
    if (Trim(lobby_url).empty()) {
        return Failure("no lobby is set (multiplayer.txt: lobby=https://...)");
    }
    const Common::URLInfo url = Common::SplitUrl(Trim(lobby_url));
    if (url.host.empty()) {
        return Failure(fmt::format("not a lobby address: {}", lobby_url));
    }

    // (a lobby given as https://host/some/base keeps its base path)
    std::string path = url.path;
    while (!path.empty() && path.back() == '/') {
        path.pop_back();
    }
    path += "/lobby";

    std::unique_ptr<httplib::ClientImpl> client;
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLClient* tls = nullptr;
    if (url.is_https) {
        auto tls_client = std::make_unique<httplib::SSLClient>(url.host, url.port);
        const std::string& authorities = CaBundle::Pem();
        tls_client->load_ca_cert_store(authorities.data(), authorities.size());
        tls_client->enable_server_certificate_verification(true);
        tls = tls_client.get();
        client = std::move(tls_client);
    }
#else
    if (url.is_https) {
        return Failure("this build has no HTTPS: the lobby has to be an http:// address");
    }
#endif
    if (!client) {
        client = std::make_unique<httplib::ClientImpl>(url.host, url.port);
    }
    client->set_connection_timeout(ConnectSeconds);
    client->set_read_timeout(TransferSeconds);
    client->set_write_timeout(TransferSeconds);
    client->set_follow_location(false);
    client->set_keep_alive(false);

    const httplib::Headers headers{{"api-version", ApiVersion}, {"Accept", "application/json"}};
    std::string body;
    bool too_long = false;
    httplib::Result answer{nullptr, httplib::Error::Unknown};
    try {
        answer = client->Get(path, headers, [&](const char* data, std::size_t length) {
            if (body.size() + length > LongestAnswer) {
                too_long = true;
                return false;
            }
            body.append(data, length);
            return true;
        });
    } catch (const std::exception& e) {
        return Failure(fmt::format("asking {} failed: {}", url.host, e.what()));
    }

    if (too_long) {
        return Failure("the lobby's answer is too long");
    }
    if (!answer) {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        if (tls && (answer.error() == httplib::Error::SSLServerVerification ||
                    answer.error() == httplib::Error::SSLConnection)) {
            const long verify_result = tls->get_openssl_verify_result();
            if (verify_result != X509_V_OK) {
                return Failure(CertificateFailure(verify_result));
            }
            if (answer.error() == httplib::Error::SSLServerVerification) {
                return Failure("the lobby's certificate is for another name");
            }
        }
#endif
        switch (answer.error()) {
        case httplib::Error::Connection:
            return Failure(fmt::format("{} can't be reached (no network, or no such name)", url.host));
        case httplib::Error::ConnectionTimeout:
        case httplib::Error::Read:
            return Failure(fmt::format("{} did not answer in time", url.host));
        case httplib::Error::SSLConnection:
            return Failure(fmt::format("no secure connection to {}", url.host));
        default:
            return Failure(fmt::format("asking {} failed: {}", url.host,
                                       httplib::to_string(answer.error())));
        }
    }
    if (answer->status >= 300 && answer->status < 400) {
        return Failure(fmt::format("the lobby has moved to {}",
                                   answer->get_header_value("Location")));
    }
    if (answer->status != 200) {
        return Failure(fmt::format("the lobby answered with status {}", answer->status));
    }

    Result result = Parse(body);
    if (result.ok) {
        std::printf("hen: room list: %zu rooms from %s (%u of another version)\n",
                    result.rooms.size(), url.host.c_str(), result.other_version);
    }
    return result;
}

} // namespace RoomList
