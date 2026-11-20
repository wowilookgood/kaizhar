// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#pragma once

#include <string>

/// The certificate authorities HTTPS servers are checked against: the list LibreSSL comes with
/// (externals/libressl/cert.pem, built into the program by ca_bundle_data.S), followed by the
/// user's own additions in /switch/Kaizhar/cacert.pem if that file exists.
namespace CaBundle {

/// PEM text. Read once; the same string afterwards.
const std::string& Pem();

} // namespace CaBundle
