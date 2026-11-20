// The certificate authority list (KAIZHAR_CA_BUNDLE: externals/libressl/cert.pem, set by
// src/citra_switch/CMakeLists.txt) as read-only data. tls/ca_bundle.cpp reads it.
    .section .rodata.kaizhar_ca_bundle, "a"
    .balign 8
    .global kaizhar_ca_bundle
    .global kaizhar_ca_bundle_end
kaizhar_ca_bundle:
    .incbin KAIZHAR_CA_BUNDLE
kaizhar_ca_bundle_end:
    .byte 0
