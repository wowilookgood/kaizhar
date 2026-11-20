// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>

#include <fmt/format.h>
#include <httplib.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "citra_switch/https_check.h"
#include "citra_switch/tls/ca_bundle.h"
#include "common/web_util.h"

namespace HttpsCheck {

namespace {

constexpr const char* DefaultUrl = "https://example.com/";
constexpr int StepSeconds = 8;

using Clock = std::chrono::steady_clock;

long long Ms(Clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - since).count();
}

void Line(const std::string& text) {
    std::printf("hen: tls: %s\n", text.c_str());
    std::fflush(stdout);
}

std::string LastError() {
    char text[160] = "no error recorded";
    const unsigned long code = ERR_peek_last_error();
    if (code != 0) {
        ERR_error_string_n(code, text, sizeof(text));
    }
    ERR_clear_error();
    return text;
}

std::string TimeText(const ASN1_TIME* time) {
    std::string text;
    if (BIO* bio = BIO_new(BIO_s_mem())) {
        if (ASN1_TIME_print(bio, time)) {
            char buffer[64];
            const int length = BIO_read(bio, buffer, sizeof(buffer) - 1);
            if (length > 0) {
                text.assign(buffer, static_cast<std::size_t>(length));
            }
        }
        BIO_free(bio);
    }
    return text;
}

/// The certificate authorities into the context's store. Returns how many were taken.
int LoadAuthorities(SSL_CTX* context) {
    const std::string& pem = CaBundle::Pem();
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) {
        return 0;
    }
    X509_STORE* store = SSL_CTX_get_cert_store(context);
    int count = 0;
    while (X509* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
        count += X509_STORE_add_cert(store, certificate) == 1;
        X509_free(certificate);
    }
    ERR_clear_error(); // (the end of the list reads as an error)
    BIO_free(bio);
    return count;
}

/// A connection made without blocking for longer than StepSeconds. -1: none.
int Connect(const addrinfo* address, std::string& why) {
    const int fd = socket(address->ai_family, SOCK_STREAM, 0);
    if (fd < 0) {
        why = fmt::format("no socket: errno {} ({})", errno, std::strerror(errno));
        return -1;
    }
    const int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, address->ai_addr, address->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(fd, &writable);
        timeval timeout{StepSeconds, 0};
        rc = select(fd + 1, nullptr, &writable, nullptr, &timeout);
        if (rc == 0) {
            why = fmt::format("no answer in {} s", StepSeconds);
            close(fd);
            return -1;
        }
        int error = 0;
        socklen_t length = sizeof(error);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length);
        if (rc < 0 || error != 0) {
            errno = error != 0 ? error : errno;
            rc = -1;
        } else {
            rc = 0;
        }
    }
    if (rc != 0) {
        why = fmt::format("errno {} ({})", errno, std::strerror(errno));
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, flags);
    const timeval timeout{StepSeconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return fd;
}

} // namespace

bool Run(const std::string& url_text) {
    const std::string asked = url_text.empty() ? DefaultUrl : url_text;
    const Common::URLInfo url = Common::SplitUrl(asked);
    if (!url.is_https || url.host.empty()) {
        Line(fmt::format("check of {}: not an https:// address", asked));
        return false;
    }
    const std::string path = url.path.empty() ? "/" : url.path;
    Line(fmt::format("check of https://{}:{}{}", url.host, url.port, path));

    // The clock: certificates are checked against it.
    const std::time_t now = std::time(nullptr);
    char clock_text[40] = "?";
    if (const std::tm* utc = std::gmtime(&now)) {
        std::strftime(clock_text, sizeof(clock_text), "%Y-%m-%d %H:%M:%S", utc);
    }
    Line(fmt::format("clock: {} UTC ({} s since 1970){}", clock_text, static_cast<long long>(now),
                     now < 1700000000 ? ": before 2023, certificates will look not yet valid" : ""));

    auto step = Clock::now();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    const int lookup = getaddrinfo(url.host.c_str(), std::to_string(url.port).c_str(), &hints, &addresses);
    if (lookup != 0 || addresses == nullptr) {
        Line(fmt::format("resolver: {} not found in {} ms: {} ({})", url.host, Ms(step), lookup,
                         gai_strerror(lookup)));
        return false;
    }
    std::string found;
    for (const addrinfo* a = addresses; a != nullptr; a = a->ai_next) {
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr, ip, sizeof(ip));
        found += found.empty() ? ip : std::string(", ") + ip;
    }
    Line(fmt::format("resolver: {} is {} ({} ms)", url.host, found, Ms(step)));

    step = Clock::now();
    std::string why;
    const int fd = Connect(addresses, why);
    freeaddrinfo(addresses);
    if (fd < 0) {
        Line(fmt::format("connect: failed after {} ms: {}", Ms(step), why));
        return false;
    }
    Line(fmt::format("connect: port {} open in {} ms", url.port, Ms(step)));

    bool verified = false, answered = false;
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    SSL* ssl = nullptr;
    do {
        if (context == nullptr) {
            Line("library: no context: " + LastError());
            break;
        }
        step = Clock::now();
        const int authorities = LoadAuthorities(context);
        Line(fmt::format("authorities: {} loaded in {} ms ({} KiB of PEM)", authorities, Ms(step),
                         CaBundle::Pem().size() / 1024));
        // The handshake goes on whatever the check says, so that the log names the reason.
        SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
        ssl = SSL_new(context);
        if (ssl == nullptr) {
            Line("library: no connection object: " + LastError());
            break;
        }
        SSL_set_fd(ssl, fd);
        SSL_set_tlsext_host_name(ssl, url.host.c_str());
        X509_VERIFY_PARAM_set1_host(SSL_get0_param(ssl), url.host.c_str(), 0);
        step = Clock::now();
        const int handshake = SSL_connect(ssl);
        if (handshake != 1) {
            Line(fmt::format("handshake: failed after {} ms: SSL error {}, {}", Ms(step),
                             SSL_get_error(ssl, handshake), LastError()));
            break;
        }
        Line(fmt::format("handshake: done in {} ms, {}, {}", Ms(step), SSL_get_version(ssl),
                         SSL_get_cipher_name(ssl)));

        if (X509* certificate = SSL_get_peer_certificate(ssl)) {
            char subject[200] = "?", issuer[200] = "?";
            X509_NAME_oneline(X509_get_subject_name(certificate), subject, sizeof(subject));
            X509_NAME_oneline(X509_get_issuer_name(certificate), issuer, sizeof(issuer));
            Line(fmt::format("certificate: {}; issued by {}; valid {} to {}", subject, issuer,
                             TimeText(X509_get_notBefore(certificate)),
                             TimeText(X509_get_notAfter(certificate))));
            X509_free(certificate);
        } else {
            Line("certificate: the server sent none");
        }
        const long result = SSL_get_verify_result(ssl);
        verified = result == X509_V_OK;
        Line(fmt::format("verification: {} ({}: {})", verified ? "accepted" : "REFUSED", result,
                         X509_verify_cert_error_string(result)));

        step = Clock::now();
        const std::string request = fmt::format(
            "GET {} HTTP/1.1\r\nHost: {}\r\nUser-Agent: Kaizhar\r\nAccept: */*\r\nConnection: close\r\n\r\n",
            path, url.host);
        if (SSL_write(ssl, request.data(), static_cast<int>(request.size())) <= 0) {
            Line("request: could not be sent: " + LastError());
            break;
        }
        std::string head;
        char buffer[512];
        while (head.find("\r\n") == std::string::npos && head.size() < 2048) {
            const int got = SSL_read(ssl, buffer, sizeof(buffer));
            if (got <= 0) {
                break;
            }
            head.append(buffer, static_cast<std::size_t>(got));
        }
        const std::size_t end = head.find("\r\n");
        if (end == std::string::npos) {
            Line(fmt::format("answer: none in {} ms: {}", Ms(step), LastError()));
            break;
        }
        answered = true;
        Line(fmt::format("answer: {} ({} ms)", head.substr(0, end), Ms(step)));
    } while (false);
    if (ssl != nullptr) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    if (context != nullptr) {
        SSL_CTX_free(context);
    }
    close(fd);

    // Once more as the room list asks a lobby: the HTTP library's client, which refuses a
    // certificate that doesn't check.
    step = Clock::now();
    httplib::SSLClient client(url.host, url.port);
    const std::string& authorities = CaBundle::Pem();
    client.load_ca_cert_store(authorities.data(), authorities.size());
    client.enable_server_certificate_verification(true);
    client.set_connection_timeout(StepSeconds);
    client.set_read_timeout(StepSeconds);
    client.set_write_timeout(StepSeconds);
    client.set_follow_location(false);
    client.set_keep_alive(false);
    std::size_t bytes = 0;
    const httplib::Result got = client.Get(path, [&](const char*, std::size_t length) {
        bytes += length;
        return bytes < 1024 * 1024;
    });
    if (got) {
        Line(fmt::format("the room list's way: status {}, {} bytes, {} ms", got->status, bytes, Ms(step)));
    } else {
        Line(fmt::format("the room list's way: failed after {} ms: {} (verification result {})", Ms(step),
                         httplib::to_string(got.error()), client.get_openssl_verify_result()));
    }
    const bool ok = verified && answered && got;
    Line(ok ? "check passed" : "check FAILED");
    return ok;
}

} // namespace HttpsCheck
