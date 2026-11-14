// dlfcn.h for Switch 2 homebrew: an .nro can't load native libraries, so
// dlopen always fails (dlerror says why) and dladdr finds nothing. Lets code
// that loads plugins (Dusklight's native code mods) build and report the
// failure at run time. Implemented in hen_posix.c.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY 0x0001
#define RTLD_NOW 0x0002
#define RTLD_GLOBAL 0x0100
#define RTLD_LOCAL 0x0000
#define RTLD_DEFAULT ((void*)0)

typedef struct {
	const char* dli_fname;
	void* dli_fbase;
	const char* dli_sname;
	void* dli_saddr;
} Dl_info;

void* dlopen(const char* path, int mode);
void* dlsym(void* handle, const char* name);
int dlclose(void* handle);
char* dlerror(void);
int dladdr(const void* addr, Dl_info* info);

#ifdef __cplusplus
}
#endif
