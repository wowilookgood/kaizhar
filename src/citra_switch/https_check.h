// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

/// --https-check[=URL] (development builds): one HTTPS request with the server's certificate
/// checked, step by step, each step a "hen: tls:" line in the log: the console's clock, the
/// name lookup, the connection, the handshake, the certificate and its check, the answer's
/// status; then the same request once more the way the room list makes its own. It shows
/// whether sockets, the TLS library, the built-in certificate authorities and the clock work
/// together on the console. The default address is example.com's, a page kept for such uses.
namespace HttpsCheck {

/// url: empty for the default. True when the certificate was accepted and an answer came.
bool Run(const std::string& url);

} // namespace HttpsCheck
