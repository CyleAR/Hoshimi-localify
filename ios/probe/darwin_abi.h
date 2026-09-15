/* Minimal public Darwin C ABI for the SDK-free arm64 diagnostic build.
 * Keep this list small; the macOS build uses Apple's real SDK headers. */
#pragma once
#ifdef HOSHIMI_SDK_FREE
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
typedef struct __sFILE FILE;
typedef struct _opaque_pthread_t *pthread_t;
typedef void *dispatch_queue_t;
typedef struct { const char *dli_fname; void *dli_fbase;
                 const char *dli_sname; void *dli_saddr; } Dl_info;
extern char *getenv(const char *);
extern FILE *fopen(const char *, const char *);
extern int fclose(FILE *);
extern size_t fread(void *, size_t, size_t, FILE *);
extern int fseek(FILE *, long, int);
extern long ftell(FILE *);
extern int fflush(FILE *);
extern int fprintf(FILE *, const char *, ...);
extern int vfprintf(FILE *, const char *, va_list);
extern int snprintf(char *, size_t, const char *, ...);
extern int strcmp(const char *, const char *);
extern size_t strlen(const char *);
extern char *strstr(const char *, const char *);
extern void *memcpy(void *, const void *, size_t);
extern void *memmove(void *, const void *, size_t);
extern int memcmp(const void *, const void *, size_t);
extern void *malloc(size_t);
extern void free(void *);
extern void flockfile(FILE *);
extern void funlockfile(FILE *);
extern unsigned int sleep(unsigned int);
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_detach(pthread_t);
extern dispatch_queue_t dispatch_get_main_queue(void);
extern void dispatch_async_f(dispatch_queue_t, void *, void (*)(void *));
extern int dladdr(const void *, Dl_info *);
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int dlclose(void *);
#define RTLD_NOW 0x2
#define RTLD_NOLOAD 0x10
#define SEEK_SET 0
#define SEEK_END 2
#else
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <dlfcn.h>
#include <dispatch/dispatch.h>
#endif

struct mach_header;
extern void _dyld_register_func_for_add_image(
    void (*callback)(const struct mach_header *, intptr_t));
