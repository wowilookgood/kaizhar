// Host-side test of hen/idbe/idbe_passthrough.{h,cpp}. Build and run: ./run.sh
// No network: only the decisions (which requests qualify, what is sent, cache and budget).

#include <cstdio>
#include <string>

#include "../idbe_passthrough.h"

static int failures = 0;
static int checks = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        ++checks;                                                                                  \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::printf("FAILED line %d: %s\n", __LINE__, #condition);                             \
        }                                                                                          \
    } while (0)

static void Paths() {
    using Idbe::ParsePath;
    auto plain = ParsePath("/icondata/10/0004000000030800.idbe");
    CHECK(plain && plain->directory == 0x10 && plain->title_id == 0x0004000000030800ull &&
          !plain->version);
    CHECK(plain && plain->Path() == "/icondata/10/0004000000030800.idbe");

    auto versioned = ParsePath("/icondata/10/000400000F700E00-1040.idbe");
    CHECK(versioned && versioned->title_id == 0x000400000F700E00ull && versioned->version &&
          *versioned->version == 1040);
    CHECK(versioned && versioned->Path() == "/icondata/10/000400000F700E00-1040.idbe");

    // Lower case is read, upper case is sent.
    auto lower = ParsePath("/icondata/1a/000400000f700e00-0.idbe");
    CHECK(lower && lower->directory == 0x1A && lower->version && *lower->version == 0);
    CHECK(lower && lower->Path() == "/icondata/1A/000400000F700E00-0.idbe");

    CHECK(ParsePath("/icondata/10/0004000000030800-4294967295.idbe").has_value());
    CHECK(!ParsePath("/icondata/10/0004000000030800-4294967296.idbe"));
    CHECK(!ParsePath("/icondata/10/0004000000030800-12345678901.idbe"));

    const char* refused[] = {
        "",
        "/",
        "/icondata/",
        "/icondata/10/",
        "icondata/10/0004000000030800.idbe",
        "/icondata/10/0004000000030800",
        "/icondata/10/0004000000030800.idbe?x=1",
        "/icondata/10/0004000000030800.idbe#a",
        "/icondata/10/0004000000030800.idbe/",
        "/icondata/10/0004000000030800.IDBE",
        "/icondata/10/000400000003080.idbe",
        "/icondata/10/00040000000308000.idbe",
        "/icondata/10/000400000003080G.idbe",
        "/icondata/1/0004000000030800.idbe",
        "/icondata/100/0004000000030800.idbe",
        "/icondata/10/0004000000030800-.idbe",
        "/icondata/10/0004000000030800--1.idbe",
        "/icondata/10/0004000000030800-+1.idbe",
        "/icondata/10/0004000000030800-1a.idbe",
        "/icondata/10/../10/0004000000030800.idbe",
        "/icondata/10/%30004000000030800.idbe",
        "/icondata//10/0004000000030800.idbe",
        "/other/10/0004000000030800.idbe",
        "/icondata/10/0004000000030800.idbe ",
    };
    for (const char* path : refused) {
        ++checks;
        if (ParsePath(path)) {
            ++failures;
            std::printf("FAILED: accepted \"%s\"\n", path);
        }
    }
}

static void Hosts() {
    using Idbe::Classify;
    const char* path = "/icondata/10/0004000000030800.idbe";
    CHECK(Classify(true, "idbe-ctr.cdn.nintendo.net", 443, path, true).has_value());
    CHECK(Classify(true, "IDBE-CTR.cdn.Nintendo.NET", 443, path, true).has_value());
    CHECK(!Classify(false, "idbe-ctr.cdn.nintendo.net", 443, path, true)); // plain HTTP
    CHECK(!Classify(true, "idbe-ctr.cdn.nintendo.net", 80, path, true));
    CHECK(!Classify(true, "idbe-ctr.cdn.nintendo.net", 8443, path, true));
    CHECK(!Classify(true, "idbe-ctr.cdn.nintendo.net", 443, path, false)); // POST, HEAD...
    CHECK(!Classify(true, "idbe-ctr.cdn.nintendo.net", 443, "/icondata/x", true));
    const char* others[] = {
        "idbe-ctr.cdn.nintendo.net.",       "idbe-wup.cdn.nintendo.net",
        "idbe-ctr.cdn.pretendo.cc",         "x.idbe-ctr.cdn.nintendo.net",
        "idbe-ctr.cdn.nintendo.net.evil.x", "idbe-ctr.cdn.nintendo.ne",
        "idbe-ctrxcdn.nintendo.net",        "cdn.nintendo.net",
        "nus.cdn.c.shop.nintendowifi.net",  "idbe-ctr.cdn.nintendo.net:443",
        "idbe-ctr.cdn.nintendo.net@evil.x", "",
    };
    for (const char* host : others) {
        ++checks;
        if (Classify(true, host, 443, path, true)) {
            ++failures;
            std::printf("FAILED: host \"%s\" qualified\n", host);
        }
    }
}

static void Headers() {
    const auto headers = Idbe::RequestHeaders();
    CHECK(headers.size() == 4);
    CHECK(headers[0].first == "Host" && headers[0].second == "idbe-ctr.cdn.nintendo.net");
    CHECK(headers[1].first == "User-Agent");
    CHECK(headers[2].first == "Accept" && headers[2].second == "*/*");
    CHECK(headers[3].first == "Connection" && headers[3].second == "close");
    for (const auto& [name, value] : headers) {
        CHECK(name != "Cookie" && name != "Authorization" && name.rfind("X-", 0) != 0);
        CHECK(value.find('\r') == std::string::npos && value.find('\n') == std::string::npos);
    }

    CHECK(Idbe::ResponseAccepted(200, Idbe::ExpectedBodySize));
    CHECK(Idbe::ResponseAccepted(200, Idbe::MaxBodySize));
    CHECK(!Idbe::ResponseAccepted(200, Idbe::MaxBodySize + 1));
    CHECK(!Idbe::ResponseAccepted(200, 0));
    CHECK(!Idbe::ResponseAccepted(404, 100));
    CHECK(!Idbe::ResponseAccepted(301, 100));
    CHECK(!Idbe::ResponseAccepted(206, 100));
    CHECK(Idbe::ExpectedBodySize == 0x36D2);
}

static void CacheAndBudget() {
    using Action = Idbe::Session::Action;
    Idbe::Session session;
    const Idbe::IconRequest a{0x10, 0x0004000000030800ull, std::nullopt};
    const Idbe::IconRequest a_v{0x10, 0x0004000000030800ull, 1040};
    const Idbe::IconRequest b{0x10, 0x0004000000055D00ull, std::nullopt};

    auto first = session.Begin(a);
    CHECK(first.action == Action::Send && first.sent == 1);
    session.Answered(a, 200, std::vector<std::uint8_t>(Idbe::ExpectedBodySize, 0x5A));
    auto again = session.Begin(a);
    CHECK(again.action == Action::Cached && again.body.size() == Idbe::ExpectedBodySize &&
          again.body[7] == 0x5A && again.sent == 1);

    // The same title with a version is another file.
    CHECK(session.Begin(a_v).action == Action::Send);
    session.Answered(a_v, 404, {});
    CHECK(session.Begin(a_v).action == Action::KnownAbsent);
    CHECK(session.Sent() == 2);

    // Nothing came back: asked again later, and counted again.
    CHECK(session.Begin(b).action == Action::Send);
    session.Failed(b);
    CHECK(session.Begin(b).action == Action::Send);
    CHECK(session.Sent() == 4);

    // Too large an answer is not kept as a file.
    session.Answered(b, 200, std::vector<std::uint8_t>(Idbe::MaxBodySize + 1));
    CHECK(session.Begin(b).action == Action::KnownAbsent);

    // The budget: 64 requests, then none; what is kept still answers.
    Idbe::Session burst;
    for (unsigned i = 0; i < 200; ++i) {
        const Idbe::IconRequest request{0x10, 0x0004000000100000ull + i, std::nullopt};
        const auto decision = burst.Begin(request);
        CHECK((decision.action == Action::Send) == (i < Idbe::MaxRequestsPerRun));
        CHECK(i < Idbe::MaxRequestsPerRun || decision.action == Action::OverBudget);
        if (decision.action == Action::Send) {
            burst.Answered(request, 200, {1, 2, 3});
        }
    }
    CHECK(burst.Sent() == Idbe::MaxRequestsPerRun);
    CHECK(burst.Begin({0x10, 0x0004000000100000ull, std::nullopt}).action == Action::Cached);
    CHECK(burst.Begin({0x10, 0x0004000000100000ull + 199, std::nullopt}).action ==
          Action::OverBudget);
}

static void Log() {
    Idbe::LogEntry entry;
    entry.request = *Idbe::ParsePath("/icondata/10/0004000000030800-1040.idbe");
    entry.asked_url = "https://idbe-ctr.cdn.nintendo.net/icondata/10/0004000000030800-1040.idbe";
    entry.address = "23.0.0.1 (1.1.1.1)";
    entry.applet_headers = {{"User-Agent", "something"}, {"X-Thing", "secret"}};
    entry.certificate_withheld = "the 3DS family's default (ClCertA)";
    entry.outcome = "sent";
    entry.status = 200;
    entry.bytes = Idbe::ExpectedBodySize;
    entry.sent = 3;
    const std::string release = Idbe::LogLine(entry);
    CHECK(release.find("hen: idbe: GET https://idbe-ctr.cdn.nintendo.net/icondata/10/"
                       "0004000000030800-1040.idbe") == 0);
    CHECK(release.find("[Host: idbe-ctr.cdn.nintendo.net]") != std::string::npos);
    CHECK(release.find("[X-Thing]") != std::string::npos);
    CHECK(release.find("secret") == std::string::npos);
    CHECK(release.find("client certificate withheld: the 3DS family's default (ClCertA)") !=
          std::string::npos);
    CHECK(release.find("status 200, 14034 bytes; request 3 of 64 this run") != std::string::npos);
    CHECK(release.find('\n') == std::string::npos);
    entry.applet_header_values = true;
    CHECK(Idbe::LogLine(entry).find("[X-Thing: secret]") != std::string::npos);
    std::printf("example: %s\n", release.c_str());

    entry.outcome = "cache";
    const std::string cached = Idbe::LogLine(entry);
    CHECK(cached.find("headers sent") == std::string::npos);
    std::printf("example: %s\n", cached.c_str());
}

int main() {
    Paths();
    Hosts();
    Headers();
    CacheAndBudget();
    Log();
    std::printf("%d checks, %d failed\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
