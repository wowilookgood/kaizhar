// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The datagrams of a multiplayer room (room_link.cpp). ENet, the library the room protocol runs
// on, sends and receives with sendmsg and recvmsg only, and nothing else in the program uses
// those two; the linker routes them here (--wrap in CMakeLists.txt). That gives a count for the
// log, the first failures with their error numbers, and a second way to do the same work:
// --room-plain-io (hen_room_plain_io) gathers each datagram into one buffer and uses sendto and
// recvfrom, for telling a fault of the runtime's sendmsg/recvmsg from anything else.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>

#include "switch_platform.h"

extern int hen_room_plain_io; // hen/jit/hen_jit.c

ssize_t __real_sendmsg(int fd, const struct msghdr* msg, int flags);
ssize_t __real_recvmsg(int fd, struct msghdr* msg, int flags);

// ENet's largest datagram (ENET_PROTOCOL_MAXIMUM_MTU).
#define DATAGRAM_MAX 4096

static unsigned long long s_sent, s_sentBytes, s_received, s_receivedBytes, s_sendFailed, s_receiveFailed;

static void add(unsigned long long* counter, unsigned long long amount) {
    __atomic_fetch_add(counter, amount, __ATOMIC_RELAXED);
}

// A socket with nothing to read or no room to write is not a failure: ENet's sockets don't block.
static bool would_block(int error) {
    return error == EWOULDBLOCK || error == EAGAIN || error == EINTR;
}

ssize_t __wrap_sendmsg(int fd, const struct msghdr* msg, int flags) {
    ssize_t result;
    if (hen_room_plain_io && msg != NULL) {
        unsigned char datagram[DATAGRAM_MAX];
        size_t length = 0;
        for (int i = 0; i < msg->msg_iovlen; i++) {
            if (length + msg->msg_iov[i].iov_len > sizeof(datagram)) {
                errno = EMSGSIZE;
                return -1;
            }
            memcpy(datagram + length, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
            length += msg->msg_iov[i].iov_len;
        }
        result = sendto(fd, datagram, length, flags, (const struct sockaddr*)msg->msg_name, msg->msg_namelen);
    } else {
        result = __real_sendmsg(fd, msg, flags);
    }
    if (result >= 0) {
        add(&s_sent, 1);
        add(&s_sentBytes, (unsigned long long)result);
    } else if (!would_block(errno)) {
        const int error = errno;
        if (__atomic_fetch_add(&s_sendFailed, 1, __ATOMIC_RELAXED) < 3) {
            printf("hen: room: sending a datagram failed: errno %d (%s)%s\n", error, strerror(error),
                   hen_room_plain_io ? " [sendto]" : " [sendmsg]");
        }
        errno = error;
    }
    return result;
}

ssize_t __wrap_recvmsg(int fd, struct msghdr* msg, int flags) {
    ssize_t result;
    if (hen_room_plain_io && msg != NULL) {
        unsigned char datagram[DATAGRAM_MAX];
        socklen_t name_length = msg->msg_namelen;
        result = recvfrom(fd, datagram, sizeof(datagram), flags, (struct sockaddr*)msg->msg_name,
                          msg->msg_name != NULL ? &name_length : NULL);
        if (result >= 0) {
            size_t left = (size_t)result;
            const unsigned char* from = datagram;
            for (int i = 0; i < msg->msg_iovlen && left > 0; i++) {
                const size_t part = left < msg->msg_iov[i].iov_len ? left : msg->msg_iov[i].iov_len;
                memcpy(msg->msg_iov[i].iov_base, from, part);
                from += part;
                left -= part;
            }
            msg->msg_namelen = name_length;
            msg->msg_flags = left > 0 ? MSG_TRUNC : 0; // (more than the caller had room for)
            result -= (ssize_t)left;
        }
    } else {
        result = __real_recvmsg(fd, msg, flags);
    }
    if (result >= 0) {
        add(&s_received, 1);
        add(&s_receivedBytes, (unsigned long long)result);
    } else if (!would_block(errno)) {
        const int error = errno;
        if (__atomic_fetch_add(&s_receiveFailed, 1, __ATOMIC_RELAXED) < 3) {
            printf("hen: room: receiving a datagram failed: errno %d (%s)%s\n", error, strerror(error),
                   hen_room_plain_io ? " [recvfrom]" : " [recvmsg]");
        }
        errno = error;
    }
    return result;
}

void sw_room_io_stats(unsigned long long out[6]) {
    out[0] = __atomic_load_n(&s_sent, __ATOMIC_RELAXED);
    out[1] = __atomic_load_n(&s_sentBytes, __ATOMIC_RELAXED);
    out[2] = __atomic_load_n(&s_received, __ATOMIC_RELAXED);
    out[3] = __atomic_load_n(&s_receivedBytes, __ATOMIC_RELAXED);
    out[4] = __atomic_load_n(&s_sendFailed, __ATOMIC_RELAXED);
    out[5] = __atomic_load_n(&s_receiveFailed, __ATOMIC_RELAXED);
}

void sw_room_io_reset(void) {
    __atomic_store_n(&s_sent, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_sentBytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_received, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_receivedBytes, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_sendFailed, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_receiveFailed, 0, __ATOMIC_RELAXED);
}
