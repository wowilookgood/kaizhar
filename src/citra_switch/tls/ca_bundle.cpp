// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <fstream>
#include <iterator>

#include "citra_switch/tls/ca_bundle.h"

extern "C" {
extern const char kaizhar_ca_bundle[];
extern const char kaizhar_ca_bundle_end[];
}

namespace CaBundle {

namespace {
constexpr const char* UserFile = "/switch/Kaizhar/cacert.pem";
}

const std::string& Pem() {
    static const std::string pem = [] {
        std::string text(kaizhar_ca_bundle, kaizhar_ca_bundle_end);
        std::ifstream file(UserFile, std::ios::binary);
        if (file) {
            text += '\n';
            text.append(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        }
        return text;
    }();
    return pem;
}

} // namespace CaBundle
