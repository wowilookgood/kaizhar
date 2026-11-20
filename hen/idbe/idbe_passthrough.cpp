// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <cstdio>

#include "idbe_passthrough.h"

int (*hen_idbe_lookup)(const char* name, unsigned* ipv4) = nullptr;

namespace Idbe {

namespace {

int HexValue(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/// Exactly digits hex digits at the start of text; they are taken off it.
std::optional<std::uint64_t> TakeHex(std::string_view& text, std::size_t digits) {
    if (text.size() < digits) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < digits; ++i) {
        const int digit = HexValue(text[i]);
        if (digit < 0) {
            return std::nullopt;
        }
        value = (value << 4) | static_cast<std::uint64_t>(digit);
    }
    text.remove_prefix(digits);
    return value;
}

bool Take(std::string_view& text, std::string_view prefix) {
    if (text.substr(0, prefix.size()) != prefix) {
        return false;
    }
    text.remove_prefix(prefix.size());
    return true;
}

char Lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

std::string IconRequest::Path() const {
    char text[64];
    if (version) {
        std::snprintf(text, sizeof(text), "/icondata/%02X/%016llX-%u.idbe", directory,
                      static_cast<unsigned long long>(title_id), static_cast<unsigned>(*version));
    } else {
        std::snprintf(text, sizeof(text), "/icondata/%02X/%016llX.idbe", directory,
                      static_cast<unsigned long long>(title_id));
    }
    return text;
}

std::optional<IconRequest> ParsePath(std::string_view path) {
    IconRequest request;
    if (!Take(path, PathPrefix)) {
        return std::nullopt;
    }
    const auto directory = TakeHex(path, 2);
    if (!directory || !Take(path, "/")) {
        return std::nullopt;
    }
    request.directory = static_cast<std::uint8_t>(*directory);
    const auto title_id = TakeHex(path, 16);
    if (!title_id) {
        return std::nullopt;
    }
    request.title_id = *title_id;
    if (Take(path, "-")) {
        std::uint64_t version = 0;
        std::size_t digits = 0;
        while (digits < path.size() && path[digits] >= '0' && path[digits] <= '9') {
            version = version * 10 + static_cast<std::uint64_t>(path[digits] - '0');
            if (++digits > 10) {
                return std::nullopt;
            }
        }
        if (digits == 0 || version > 0xFFFFFFFFull) {
            return std::nullopt;
        }
        path.remove_prefix(digits);
        request.version = static_cast<std::uint32_t>(version);
    }
    if (path != ".idbe") {
        return std::nullopt;
    }
    return request;
}

std::optional<IconRequest> Classify(bool is_https, std::string_view host, int port,
                                    std::string_view path, bool method_is_get) {
    if (!is_https || port != Port || !method_is_get || host.size() != Host.size()) {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < host.size(); ++i) {
        if (Lower(host[i]) != Host[i]) {
            return std::nullopt;
        }
    }
    return ParsePath(path);
}

std::vector<std::pair<std::string, std::string>> RequestHeaders() {
    return {
        {"Host", std::string{Host}},
        {"User-Agent", std::string{UserAgent}},
        {"Accept", "*/*"},
        {"Connection", "close"},
    };
}

bool ResponseAccepted(int status, std::size_t body_size) {
    return status == 200 && body_size != 0 && body_size <= MaxBodySize;
}

Session::Decision Session::Begin(const IconRequest& request) {
    std::scoped_lock lock{mutex};
    Decision decision;
    const auto it = kept.find(request);
    if (it != kept.end()) {
        decision.action = it->second.empty() ? Action::KnownAbsent : Action::Cached;
        decision.body = it->second;
    } else if (sent >= MaxRequestsPerRun) {
        decision.action = Action::OverBudget;
    } else {
        decision.action = Action::Send;
        ++sent;
    }
    decision.sent = sent;
    return decision;
}

void Session::Answered(const IconRequest& request, int status, std::vector<std::uint8_t> body) {
    std::scoped_lock lock{mutex};
    if (ResponseAccepted(status, body.size())) {
        kept[request] = std::move(body);
    } else {
        kept[request].clear();
    }
}

void Session::Failed(const IconRequest&) {}

unsigned Session::Sent() const {
    std::scoped_lock lock{mutex};
    return sent;
}

Session& RunSession() {
    static Session session;
    return session;
}

std::string LogLine(const LogEntry& entry) {
    std::string line = "hen: idbe: GET https://";
    line += Host;
    line += entry.request.Path();
    line += " (the program asked for " + entry.asked_url + "): " + entry.outcome;
    if (entry.outcome == "sent" || entry.outcome.rfind("failed", 0) == 0) {
        line += "; to " + (entry.address.empty() ? std::string{"no address"} : entry.address);
        line += "; headers sent:";
        for (const auto& [name, value] : RequestHeaders()) {
            line += " [" + name + ": " + value + "]";
        }
        line += "; the program's own headers, not sent:";
        if (entry.applet_headers.empty()) {
            line += " none";
        }
        for (const auto& [name, value] : entry.applet_headers) {
            line += " [" + name + (entry.applet_header_values ? ": " + value : std::string{}) + "]";
        }
        line += "; client certificate withheld: " + entry.certificate_withheld;
    }
    line += "; status " + std::to_string(entry.status);
    line += ", " + std::to_string(entry.bytes) + " bytes";
    if (entry.bytes != 0 && entry.bytes != ExpectedBodySize) {
        line += " (documented size: " + std::to_string(ExpectedBodySize) + ")";
    }
    line += "; request " + std::to_string(entry.sent) + " of " +
            std::to_string(MaxRequestsPerRun) + " this run";
    return line;
}

} // namespace Idbe
