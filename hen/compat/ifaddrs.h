// ifaddrs.h for the Switch 2 build (libnx has none): network interfaces can't
// be listed, so getifaddrs always fails. Used by httplib and the 3DS socket
// service (soc_u.cpp), which then report no interfaces.
#pragma once

#include <errno.h>
#include <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
	struct ifaddrs* ifa_next;
	char* ifa_name;
	unsigned int ifa_flags;
	struct sockaddr* ifa_addr;
	struct sockaddr* ifa_netmask;
	union {
		struct sockaddr* ifu_broadaddr;
		struct sockaddr* ifu_dstaddr;
	} ifa_ifu;
	void* ifa_data;
};
#define ifa_broadaddr ifa_ifu.ifu_broadaddr
#define ifa_dstaddr ifa_ifu.ifu_dstaddr

static inline int getifaddrs(struct ifaddrs** ifap) {
	*ifap = 0;
	errno = ENOSYS;
	return -1;
}

static inline void freeifaddrs(struct ifaddrs* ifa) { (void) ifa; }

#ifdef __cplusplus
}
#endif
