/* The bits of the TV's glibc 2.11 (ARM EABI, softfp) the shim uses. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdbool.h>

typedef long ssize_t;
typedef long off_t;
typedef long long off64_t;
typedef unsigned int mode_t;
typedef int pid_t;
typedef unsigned long pthread_t;
typedef struct { int __size[9]; } pthread_attr_t;
typedef union { char __size[24]; long __align; } pthread_mutex_t;
typedef struct _IO_FILE FILE;
extern FILE *stderr;

#define O_RDONLY 0
#define O_RDWR 2
#define O_CREAT 0100
#define O_TRUNC 01000
#define O_SYNC 010000
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FAILED ((void *)-1)
#define RTLD_NEXT ((void *)-1l)
#define RTLD_DEFAULT ((void *)0)
#define RTLD_NOW 2
#define EINVAL 22
#define ENOENT 2
#define ENOTTY 25
#define SEEK_SET 0
#define SIGSEGV 11
#define SIGBUS 7

int open(const char *, int, ...);
int close(int);
ssize_t read(int, void *, size_t);
ssize_t write(int, const void *, size_t);
off_t lseek(int, off_t, int);
int ioctl(int, unsigned long, ...);
void *mmap(void *, size_t, int, int, int, off_t);
void *mmap64(void *, size_t, int, int, int, off64_t);
int munmap(void *, size_t);
int ftruncate(int, off_t);
int ftruncate64(int, off64_t);
int unlink(const char *);
int dup(int);
int pipe(int[2]);
int usleep(unsigned);
unsigned sleep(unsigned);
pid_t getpid(void);
long syscall(long, ...);

void *dlsym(void *, const char *);
void *dlopen(const char *, int);
char *getenv(const char *);
int atoi(const char *);
unsigned long strtoul(const char *, char **, int);
void *malloc(size_t);
void *calloc(size_t, size_t);
void free(void *);
void abort(void);

int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int vfprintf(FILE *, const char *, va_list);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
int fflush(FILE *);

void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
int memcmp(const void *, const void *, size_t);
size_t strlen(const char *);
int strcmp(const char *, const char *);
int strncmp(const char *, const char *, size_t);
char *strcpy(char *, const char *);
char *strncpy(char *, const char *, size_t);
char *strstr(const char *, const char *);
char *strchr(const char *, int);
char *strrchr(const char *, int);

int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int pthread_mutex_lock(pthread_mutex_t *);
int pthread_mutex_unlock(pthread_mutex_t *);
#define PTHREAD_MUTEX_INITIALIZER { { 0 } }

struct timespec { long tv_sec, tv_nsec; };
int clock_gettime(int, struct timespec *);
#define CLOCK_MONOTONIC 1

typedef struct { unsigned long __val[32]; } sigset_t;
typedef struct { int si_signo, si_errno, si_code; void *si_addr; int pad[28]; } siginfo_t;
struct sigaction {
    void (*sa_sigaction)(int, siginfo_t *, void *);
    sigset_t sa_mask;
    int sa_flags;
    void (*sa_restorer)(void);
};
#define SA_SIGINFO 4
#define SA_RESETHAND 0x80000000
int sigaction(int, const struct sigaction *, struct sigaction *);
int raise(int);
void _exit(int);
long gettid_(void);
extern FILE *stdout;
int setvbuf(FILE *, char *, int, size_t);
#define _IOLBF 1
int kill(pid_t, int);
void exit(int);
int tgkill_(int, int, int);
ssize_t readlink(const char *, char *, size_t);
void hd2tv_where(const char *what, uint32_t a);
int msgctl(int, int, void *);
int semctl(int, int, int, ...);
int shmctl(int, int, void *);
#define IPC_RMID 0
int unsetenv(const char *);
int mkdir(const char *, mode_t);
