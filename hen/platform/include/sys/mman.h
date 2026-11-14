// sys/mman.h for Switch 2 (newlib has none). Horizon has no file mapping:
// mmap always fails (hen_posix.c). SQLite needs the declarations to build with
// WAL; it never maps anything here (SQLITE_MAX_MMAP_SIZE=0, and WAL keeps its
// index in heap memory in exclusive locking mode, see HenDeps.cmake).
#pragma once

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_FAILED ((void*) -1)

void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset);
int munmap(void* addr, size_t length);

#ifdef __cplusplus
}
#endif
