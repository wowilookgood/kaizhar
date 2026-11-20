// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// The one request of the emulated console that is let through to Nintendo in normal play: the
/// Friend List's download of a title's name and icon,
///   https://idbe-ctr.cdn.nintendo.net/icondata/<2 hex>/<16 hex title ID>[-<version>].idbe
/// (https://www.3dbrew.org/wiki/IDBE). Everything here is plain decisions and bookkeeping with
/// no network code, so that it is tested on the host (hen/idbe/test): which requests qualify,
/// what is sent, how many, and what is kept. The HTTP service does the sending
/// (hen/idbe/README.md has the lines to add).
namespace Idbe {

inline constexpr std::string_view Host = "idbe-ctr.cdn.nintendo.net";
inline constexpr std::string_view PathPrefix = "/icondata/";
inline constexpr int Port = 443;

/// 3dbrew: 2 bytes and 0x36D0 encrypted bytes. Other sizes are passed on too (and logged), up
/// to MaxBodySize; beyond it the transfer is dropped.
inline constexpr std::size_t ExpectedBodySize = 2 + 0x36D0;
inline constexpr std::size_t MaxBodySize = 64 * 1024;

/// Requests that may go out in one run of the program (the cache answers the rest).
inline constexpr unsigned MaxRequestsPerRun = 64;

inline constexpr int ConnectTimeoutSeconds = 5;
inline constexpr int ReadTimeoutSeconds = 5;

/// Until a request of the applet has been read in a console log, this is all that is sent.
inline constexpr std::string_view UserAgent = "Mozilla/5.0";

struct IconRequest {
    std::uint8_t directory = 0; ///< "%02X", 0x10 in every documented request
    std::uint64_t title_id = 0;
    std::optional<std::uint32_t> version;

    /// The path as the applet's format strings write it; what is sent, whatever the spelling
    /// of the request was.
    std::string Path() const;

    auto operator<=>(const IconRequest&) const = default;
};

/// "/icondata/10/0004000000030800.idbe" or ".../0004000000030800-1040.idbe" and nothing else:
/// no query, no fragment, no further segment, no sign, at most ten digits of version.
std::optional<IconRequest> ParsePath(std::string_view path);

/// The request qualifies: HTTPS, port 443, GET, exactly Host (any letter case, no trailing
/// dot, no sibling name) and a path ParsePath accepts. Anything else is not ours and takes the
/// service's usual way (with Pretendo's rules: a name that does not exist).
std::optional<IconRequest> Classify(bool is_https, std::string_view host, int port,
                                    std::string_view path, bool method_is_get);

/// The complete header set of the request, in order.
std::vector<std::pair<std::string, std::string>> RequestHeaders();

/// A response is handed to the applet only when the server said 200 and the body is not empty
/// and not above MaxBodySize.
bool ResponseAccepted(int status, std::size_t body_size);

/// What is known about the titles asked for in this run. Safe from several request threads.
class Session {
public:
    enum class Action {
        Send,       ///< not asked yet: one request may go out (it is counted now)
        Cached,     ///< answered before in this run: body holds the file
        KnownAbsent,///< the server was asked in this run and had no usable file
        OverBudget, ///< MaxRequestsPerRun requests went out already
    };
    struct Decision {
        Action action = Action::Send;
        std::vector<std::uint8_t> body;
        unsigned sent = 0; ///< requests counted so far, this one included
    };

    Decision Begin(const IconRequest& request);
    /// The server answered. An accepted response is kept; any other answer is kept as "absent".
    void Answered(const IconRequest& request, int status, std::vector<std::uint8_t> body);
    /// Nothing came back (no address, no connection, timeout, dropped for its size): not kept,
    /// so a later request may try again while the budget lasts.
    void Failed(const IconRequest& request);

    unsigned Sent() const;

private:
    mutable std::mutex mutex;
    std::map<IconRequest, std::vector<std::uint8_t>> kept; ///< empty: absent
    unsigned sent = 0;
};

Session& RunSession();

struct LogEntry {
    IconRequest request;
    std::string asked_url;  ///< the URL as the applet gave it
    std::string address;    ///< where the name resolved to, and by whom; empty: not resolved
    /// The applet's own headers (not sent).
    std::vector<std::pair<std::string, std::string>> applet_headers;
    bool applet_header_values = false; ///< print their values too (dev builds)
    /// "none", "the 3DS family's default (ClCertA)" or "one the program set"
    std::string certificate_withheld = "none";
    std::string outcome; ///< "cache", "over budget", "known absent", "sent", "failed: ..."
    int status = 0;
    std::size_t bytes = 0;
    unsigned sent = 0;
};

/// One line (no newline) for the log of a dev and of a release build.
std::string LogLine(const LogEntry& entry);

} // namespace Idbe

/// Set once by the frontend at start (never cleared): the address of a name for this request,
/// found as the setup finds Nintendo's update servers (the console's DNS, then a name server
/// asked directly). 1: *ipv4 holds the address in network byte order. Null: the system's own
/// lookup is used.
extern "C" int (*hen_idbe_lookup)(const char* name, unsigned* ipv4);
