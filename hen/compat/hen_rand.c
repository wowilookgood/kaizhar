// RAND_bytes for the Switch 2 build (hen/compat/openssl/rand.h).
#include <switch.h>

int RAND_bytes(unsigned char* buf, int num) {
	if (num > 0) {
		randomGet(buf, (size_t) num);
	}
	return 1;
}
