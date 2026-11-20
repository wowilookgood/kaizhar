// Included ahead of every LibreSSL source file (libressl_switch.cmake): declarations that the
// sources expect from headers they do include on other systems.
#pragma once

#include <limits.h>
#include <sys/endian.h>
#include <sys/types.h>
#include <arpa/inet.h>

#ifndef SSIZE_MAX
#define SSIZE_MAX LONG_MAX
#endif
