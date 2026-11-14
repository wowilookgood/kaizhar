// sys/un.h for the Switch 2 build (libnx has no Unix-domain sockets): the
// address type only, so httplib compiles. Creating such a socket fails.
#pragma once

#include <sys/socket.h>

struct sockaddr_un {
	sa_family_t sun_family;
	char sun_path[108];
};
