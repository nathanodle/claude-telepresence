# Portability Guide

Comprehensive documentation of portability considerations for compiling `client.c` on vintage Unix systems without GNU tools.

## Table of Contents

1. [Terminal I/O APIs](#1-terminal-io-apis)
2. [Directory Enumeration](#2-directory-enumeration)
3. [Integer Types](#3-integer-types)
4. [String Functions](#4-string-functions)
5. [Memory Functions](#5-memory-functions)
6. [Network Functions](#6-network-functions)
7. [Network Libraries](#7-network-libraries)
8. [Socket Types](#8-socket-types)
9. [Signal Handling](#9-signal-handling)
10. [Time Functions](#10-time-functions)
11. [File I/O](#11-file-io)
12. [Process Control](#12-process-control)
13. [Header Files](#13-header-files)
14. [Path Limits](#14-path-limits)
15. [Byte Order](#15-byte-order)
16. [Non-blocking I/O](#16-non-blocking-io)
17. [Compiler Differences](#17-compiler-differences)
18. [Platform-Specific Notes](#18-platform-specific-notes)
19. [Makefile Portability](#19-makefile-portability)
20. [Configure Script Techniques](#20-configure-script-techniques)

---

## 1. Terminal I/O APIs

Three different APIs exist for terminal control:

| API | Header | Systems | Notes |
|-----|--------|---------|-------|
| termios | `<termios.h>` | POSIX, most modern Unix | Preferred, POSIX standard |
| termio | `<termio.h>` | Older System V | Similar to termios but older |
| sgtty | `<sgtty.h>` | BSD 4.3, NeXTSTEP, very old | Uses ioctl with gtty/stty |

### termios (POSIX)
```c
#include <termios.h>

struct termios tio;
tcgetattr(fd, &tio);
tio.c_lflag &= ~(ICANON | ECHO);
tcsetattr(fd, TCSANOW, &tio);
```

### termio (System V)
```c
#include <termio.h>

struct termio tio;
ioctl(fd, TCGETA, &tio);
tio.c_lflag &= ~(ICANON | ECHO);
ioctl(fd, TCSETA, &tio);
```

### sgtty (BSD)
```c
#include <sgtty.h>

struct sgttyb sg;
gtty(fd, &sg);
sg.sg_flags |= RAW;
sg.sg_flags &= ~ECHO;
stty(fd, &sg);
```

### Detection Strategy
```c
#if defined(HAVE_TERMIOS_H)
#include <termios.h>
#define USE_TERMIOS
#elif defined(HAVE_TERMIO_H)
#include <termio.h>
#define USE_TERMIO
#elif defined(HAVE_SGTTY_H)
#include <sgtty.h>
#define USE_SGTTY
#endif
```

---

## 2. Directory Enumeration

| Header | Struct | Systems |
|--------|--------|---------|
| `<dirent.h>` | `struct dirent` | POSIX standard |
| `<sys/dir.h>` | `struct direct` | BSD, NeXTSTEP |
| `<sys/ndir.h>` | `struct direct` | Some old System V |
| `<ndir.h>` | `struct direct` | Very old systems |

All use `d_name` member for filename.

### Detection
```c
#if defined(HAVE_DIRENT_H)
#include <dirent.h>
typedef struct dirent dirent_t;
#elif defined(HAVE_SYS_DIR_H)
#include <sys/dir.h>
typedef struct direct dirent_t;
#elif defined(HAVE_SYS_NDIR_H)
#include <sys/ndir.h>
typedef struct direct dirent_t;
#elif defined(HAVE_NDIR_H)
#include <ndir.h>
typedef struct direct dirent_t;
#endif
```

### Usage Note
Some systems require `<sys/types.h>` before directory headers.

---

## 3. Integer Types

**Critical Issue**: `uint8_t`, `uint16_t`, `uint32_t`, `uint64_t` are C99/POSIX and not available on vintage systems.

### Header Availability

| Header | Standard | Systems |
|--------|----------|---------|
| `<stdint.h>` | C99 | Modern systems only |
| `<inttypes.h>` | C99 | Modern systems only |
| `<sys/types.h>` | Traditional | All Unix, but limited types |

### Typical Sizes on 32-bit Unix

| Type | Bytes | Notes |
|------|-------|-------|
| `char` | 1 | Always 1 byte |
| `short` | 2 | Almost always 16-bit |
| `int` | 4 | Usually 32-bit on 32-bit systems |
| `long` | 4 | 32-bit on ILP32, 64-bit on LP64 |
| `long long` | 8 | C99, not available everywhere |

### 64-bit Considerations (LP64 vs ILP32)

| Model | int | long | pointer | Systems |
|-------|-----|------|---------|---------|
| ILP32 | 32 | 32 | 32 | Most 32-bit Unix |
| LP64 | 32 | 64 | 64 | Modern 64-bit Unix |
| LLP64 | 32 | 32 | 64 | Windows 64-bit |

### Fallback Definitions
```c
#ifndef HAVE_STDINT_H
typedef unsigned char      uint8_t;
typedef signed char        int8_t;
typedef unsigned short     uint16_t;
typedef signed short       int16_t;
#if SIZEOF_INT == 4
typedef unsigned int       uint32_t;
typedef signed int         int32_t;
#elif SIZEOF_LONG == 4
typedef unsigned long      uint32_t;
typedef signed long        int32_t;
#endif
/* uint64_t may not be possible on some systems */
#if SIZEOF_LONG == 8
typedef unsigned long      uint64_t;
typedef signed long        int64_t;
#elif defined(HAVE_LONG_LONG)
typedef unsigned long long uint64_t;
typedef signed long long   int64_t;
#endif
#endif /* HAVE_STDINT_H */
```

### size_t and ssize_t

- `size_t` - Usually in `<stddef.h>` or `<sys/types.h>`, unsigned
- `ssize_t` - POSIX, signed version, may not exist on non-POSIX systems

```c
#ifndef HAVE_SSIZE_T
typedef long ssize_t;
#endif
```

---

## 4. String Functions

### Availability Matrix

| Function | Standard | Portable Alternative |
|----------|----------|---------------------|
| `strlen` | K&R | Universal |
| `strcpy` | K&R | Universal |
| `strncpy` | K&R | Universal (warning: may not null-terminate) |
| `strcmp` | K&R | Universal |
| `strncmp` | K&R | Universal |
| `strcat` | K&R | Universal |
| `strncat` | K&R | Universal |
| `strchr` | ANSI | Use `index()` on old BSD |
| `strrchr` | ANSI | Use `rindex()` on old BSD |
| `strstr` | ANSI | May need to implement |
| `strdup` | POSIX | Roll your own |
| `strlcpy` | BSD | Roll your own |
| `strlcat` | BSD | Roll your own |
| `strtok` | ANSI | Usually available |
| `strtol` | ANSI | Usually available |
| `atoi` | K&R | Universal |

### snprintf / vsnprintf

**Major portability problem.** Many vintage systems only have `sprintf()`.

| Function | Standard | Risk |
|----------|----------|------|
| `sprintf` | K&R | Buffer overflow - no length limit |
| `snprintf` | C99/POSIX | Not on vintage systems |
| `vsprintf` | ANSI | Buffer overflow |
| `vsnprintf` | C99/POSIX | Not on vintage systems |

**Options:**

1. **Careful buffer sizing** with sprintf:
```c
/* Only safe if you KNOW the max output size */
char buf[256];
sprintf(buf, "%d", value);  /* int is max ~11 chars */
```

2. **Provide fallback implementation** (complex, error-prone)

3. **Avoid complex formats** - build strings incrementally

4. **Use fixed large buffers** - wasteful but safe:
```c
char buf[8192];  /* Oversized to be safe */
sprintf(buf, "...", ...);
```

### strerror

| System | Method |
|--------|--------|
| ANSI/POSIX | `strerror(errno)` |
| Old BSD/SysV | `sys_errlist[errno]` with `sys_nerr` bounds check |

```c
#ifdef HAVE_STRERROR
#define STRERROR(e) strerror(e)
#else
extern char *sys_errlist[];
extern int sys_nerr;
#define STRERROR(e) ((e) < sys_nerr ? sys_errlist[e] : "Unknown error")
#endif
```

### strchr / strrchr vs index / rindex

Old BSD systems use `index()` and `rindex()` from `<strings.h>`:

```c
#ifdef HAVE_STRCHR
#define STRCHR strchr
#define STRRCHR strrchr
#else
#include <strings.h>
#define STRCHR index
#define STRRCHR rindex
#endif
```

### strdup Implementation
```c
#ifndef HAVE_STRDUP
char *strdup(const char *s) {
    size_t len = strlen(s) + 1;
    char *copy = malloc(len);
    if (copy) memcpy(copy, s, len);
    return copy;
}
#endif
```

### strlcpy / strlcat Implementation
```c
#ifndef HAVE_STRLCPY
size_t strlcpy(char *dst, const char *src, size_t size) {
    size_t srclen = strlen(src);
    if (size > 0) {
        size_t copylen = (srclen >= size) ? size - 1 : srclen;
        memcpy(dst, src, copylen);
        dst[copylen] = '\0';
    }
    return srclen;
}
#endif

#ifndef HAVE_STRLCAT
size_t strlcat(char *dst, const char *src, size_t size) {
    size_t dstlen = strlen(dst);
    size_t srclen = strlen(src);
    if (dstlen < size) {
        size_t copylen = (srclen >= size - dstlen) ? size - dstlen - 1 : srclen;
        memcpy(dst + dstlen, src, copylen);
        dst[dstlen + copylen] = '\0';
    }
    return dstlen + srclen;
}
#endif
```

---

## 5. Memory Functions

| Function | Standard | Alternative |
|----------|----------|-------------|
| `memcpy` | ANSI | Universal (undefined if overlaps) |
| `memmove` | ANSI | `bcopy` on BSD (handles overlap) |
| `memset` | ANSI | `bzero` on BSD (only for zeroing) |
| `memcmp` | ANSI | `bcmp` on BSD |
| `bzero` | BSD | `memset(p, 0, n)` |
| `bcopy` | BSD | `memmove(dst, src, n)` (note: args reversed!) |
| `bcmp` | BSD | `memcmp` |

### bcopy vs memcpy/memmove

**Critical**: Argument order differs!
```c
bcopy(src, dst, n);      /* BSD - source first */
memcpy(dst, src, n);     /* ANSI - destination first */
memmove(dst, src, n);    /* ANSI - destination first */
```

### Compatibility Macros
```c
#ifdef HAVE_MEMMOVE
#define MEMMOVE(d,s,n) memmove((d),(s),(n))
#else
#define MEMMOVE(d,s,n) bcopy((s),(d),(n))  /* Note: reversed args */
#endif

#ifdef HAVE_MEMSET
#define MEMZERO(p,n) memset((p),0,(n))
#else
#define MEMZERO(p,n) bzero((p),(n))
#endif
```

---

## 6. Network Functions

### Name Resolution

| Function | Standard | Notes |
|----------|----------|-------|
| `gethostbyname` | BSD/POSIX | Universal, but not thread-safe |
| `gethostbyname_r` | Some systems | Thread-safe variant, not portable |
| `getaddrinfo` | POSIX.1-2001 | Modern, but not on vintage systems |
| `gethostbyname2` | BSD | For IPv6, not portable |

**Recommendation**: Use `gethostbyname()` for maximum portability.

```c
struct hostent *he = gethostbyname(hostname);
if (!he) {
    /* Error: use h_errno, not errno */
}
```

### Address Conversion

| Function | Standard | Notes |
|----------|----------|-------|
| `inet_addr` | BSD | Returns in_addr_t, -1 on error |
| `inet_aton` | BSD | Better error handling |
| `inet_ntoa` | BSD | Returns static buffer (not thread-safe) |
| `inet_pton` | POSIX.1-2001 | Modern, IPv4/IPv6 |
| `inet_ntop` | POSIX.1-2001 | Modern, IPv4/IPv6 |

**Recommendation**: Use `inet_addr()` and `inet_ntoa()` for portability.

```c
unsigned long addr = inet_addr("192.168.1.1");
if (addr == INADDR_NONE) { /* error */ }
```

### Socket I/O

| Function | Notes |
|----------|-------|
| `send/recv` | Preferred for sockets |
| `read/write` | Work on sockets too |
| `sendto/recvfrom` | For UDP |
| `sendmsg/recvmsg` | Advanced, less portable |

---

## 7. Network Libraries

| System | Libraries Required |
|--------|-------------------|
| Solaris / SunOS 5.x | `-lsocket -lnsl` |
| HP-UX | Sometimes `-lnsl` |
| SCO Unix | `-lsocket` |
| UnixWare | `-lsocket -lnsl` |
| AIX | None (in libc) |
| IRIX | None (in libc) |
| BSD variants | None (in libc) |
| Linux | None (in libc) |
| NeXTSTEP | None (in libc) |
| Tru64/OSF1 | None (in libc) |

### Detection
```sh
# Try linking without libraries first
echo 'int socket(); int main(){socket();return 0;}' > conftest.c
if ! $CC -o conftest conftest.c 2>/dev/null; then
    # Try with -lsocket -lnsl
    if $CC -o conftest conftest.c -lsocket -lnsl 2>/dev/null; then
        LIBS="-lsocket -lnsl"
    fi
fi
```

---

## 8. Socket Types

### socklen_t

POSIX type for socket address length. Not available on older systems.

```c
#ifndef HAVE_SOCKLEN_T
typedef int socklen_t;  /* or unsigned int on some systems */
#endif
```

### in_addr_t

Type for IPv4 addresses. May not be defined.

```c
#ifndef HAVE_IN_ADDR_T
typedef unsigned long in_addr_t;
#endif
```

### INADDR_NONE

Return value from `inet_addr()` on error. May not be defined.

```c
#ifndef INADDR_NONE
#define INADDR_NONE ((in_addr_t)0xffffffff)
#endif
```

### INADDR_ANY and INADDR_LOOPBACK

Usually defined, but verify:

```c
#ifndef INADDR_ANY
#define INADDR_ANY ((in_addr_t)0x00000000)
#endif

#ifndef INADDR_LOOPBACK
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#endif
```

### Socket Option Constants

These should be universal but verify headers are included:

- `SO_REUSEADDR` - Allow address reuse
- `SO_KEEPALIVE` - Enable keepalive
- `TCP_NODELAY` - Disable Nagle (in `<netinet/tcp.h>`)

---

## 9. Signal Handling

### sigaction vs signal

| Function | Standard | Notes |
|----------|----------|-------|
| `sigaction` | POSIX | Preferred, reliable semantics |
| `signal` | K&R | Behavior varies (BSD vs SysV) |

**BSD signal()**: Handler remains installed after signal
**SysV signal()**: Handler reset to SIG_DFL after signal (must reinstall)

### sigaction Usage
```c
#ifdef HAVE_SIGACTION
struct sigaction sa;
sa.sa_handler = handler_func;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;
#ifdef SA_RESTART
sa.sa_flags |= SA_RESTART;  /* Restart interrupted syscalls */
#endif
sigaction(SIGINT, &sa, NULL);
#else
signal(SIGINT, handler_func);
#endif
```

### Signal Constants

Most signals are universal, but some are not:

| Signal | Notes |
|--------|-------|
| `SIGWINCH` | Terminal resize - not on all systems |
| `SIGINFO` | BSD only |
| `SIGPWR` | System V only |

```c
#ifdef SIGWINCH
sigaction(SIGWINCH, &sa, NULL);
#endif
```

### Signal Sets

`sigset_t`, `sigemptyset()`, `sigfillset()`, `sigaddset()`, `sigdelset()` are POSIX. Older systems may lack them.

---

## 10. Time Functions

| Function | Standard | Resolution | Notes |
|----------|----------|------------|-------|
| `time()` | K&R | 1 second | Universal |
| `gettimeofday()` | BSD/POSIX | Microseconds | Very portable |
| `clock_gettime()` | POSIX.1-2001 | Nanoseconds | Modern systems |
| `ftime()` | V7 | Milliseconds | Obsolete |

### Sleep Functions

| Function | Standard | Resolution | Notes |
|----------|----------|------------|-------|
| `sleep()` | POSIX | 1 second | Universal |
| `usleep()` | BSD | Microseconds | Deprecated in POSIX but widely available |
| `nanosleep()` | POSIX.1-2001 | Nanoseconds | Modern systems |
| `select()` | BSD | Microseconds | Portable sleep via timeout |

### Portable Microsecond Sleep
```c
void portable_usleep(unsigned long usec) {
#ifdef HAVE_NANOSLEEP
    struct timespec ts;
    ts.tv_sec = usec / 1000000;
    ts.tv_nsec = (usec % 1000000) * 1000;
    nanosleep(&ts, NULL);
#elif defined(HAVE_USLEEP)
    usleep(usec);
#else
    /* Use select() with no file descriptors */
    struct timeval tv;
    tv.tv_sec = usec / 1000000;
    tv.tv_usec = usec % 1000000;
    select(0, NULL, NULL, NULL, &tv);
#endif
}
```

### struct timeval vs struct timespec

- `struct timeval` - BSD, uses tv_sec (seconds) + tv_usec (microseconds)
- `struct timespec` - POSIX, uses tv_sec (seconds) + tv_nsec (nanoseconds)

---

## 11. File I/O

### Open Flags

| Flag | Standard | Notes |
|------|----------|-------|
| `O_RDONLY` | Universal | |
| `O_WRONLY` | Universal | |
| `O_RDWR` | Universal | |
| `O_CREAT` | Universal | |
| `O_TRUNC` | Universal | |
| `O_APPEND` | Universal | |
| `O_EXCL` | Universal | |
| `O_NONBLOCK` | POSIX | See Non-blocking I/O section |
| `O_NDELAY` | BSD/SysV | Older name for O_NONBLOCK |
| `O_SYNC` | POSIX | Not on all systems |
| `O_CLOEXEC` | POSIX.1-2008 | Modern systems only |

### File Operations

| Function | Standard | Notes |
|----------|----------|-------|
| `open/close/read/write` | Universal | |
| `lseek` | Universal | |
| `ftruncate` | POSIX | Not on very old systems |
| `truncate` | BSD/POSIX | |
| `fsync` | POSIX | |
| `stat/fstat/lstat` | POSIX | |
| `access` | POSIX | |
| `unlink` | Universal | |
| `rename` | POSIX | |
| `mkdir/rmdir` | POSIX | |

### stat Structure

The `struct stat` exists everywhere but members vary:

| Member | Notes |
|--------|-------|
| `st_mode` | Universal |
| `st_size` | Universal (may be 32-bit on old systems) |
| `st_mtime` | Universal |
| `st_atime` | Universal |
| `st_ctime` | Universal |
| `st_blksize` | BSD/POSIX, not universal |
| `st_blocks` | BSD/POSIX, not universal |

### File Type Macros

These should be universal but may require `<sys/stat.h>`:

- `S_ISREG(m)` - Regular file
- `S_ISDIR(m)` - Directory
- `S_ISLNK(m)` - Symbolic link
- `S_ISCHR(m)` - Character device
- `S_ISBLK(m)` - Block device
- `S_ISFIFO(m)` - FIFO/pipe
- `S_ISSOCK(m)` - Socket

---

## 12. Process Control

### Process Creation

| Function | Standard | Notes |
|----------|----------|-------|
| `fork()` | Universal | |
| `vfork()` | BSD | Optimization, behavior varies |
| `exec*()` | Universal | execl, execv, execle, execve, execlp, execvp |
| `system()` | ANSI | Invokes shell |
| `popen/pclose` | POSIX | |

### Process Waiting

| Function | Standard | Notes |
|----------|----------|-------|
| `wait()` | Universal | Waits for any child |
| `waitpid()` | POSIX | Waits for specific child |
| `wait3()` | BSD | With resource usage |
| `wait4()` | BSD | With resource usage + pid |

### waitpid Options

- `WNOHANG` - Don't block
- `WUNTRACED` - Also return for stopped children

### Exit Status Macros

- `WIFEXITED(status)` - True if exited normally
- `WEXITSTATUS(status)` - Exit code
- `WIFSIGNALED(status)` - True if killed by signal
- `WTERMSIG(status)` - Signal number
- `WIFSTOPPED(status)` - True if stopped
- `WSTOPSIG(status)` - Stop signal

### Pipe Creation

```c
int pipefd[2];
pipe(pipefd);  /* pipefd[0] = read end, pipefd[1] = write end */
```

### File Descriptor Operations

| Function | Notes |
|----------|-------|
| `dup()` | Duplicate fd |
| `dup2(old, new)` | Duplicate to specific fd |
| `fcntl()` | Various fd operations |
| `ioctl()` | Device-specific operations |

---

## 13. Header Files

### Standard Headers That May Be Missing

| Header | Standard | Fallback |
|--------|----------|----------|
| `<stdlib.h>` | ANSI | Declare functions manually |
| `<stddef.h>` | ANSI | Define NULL, size_t manually |
| `<string.h>` | ANSI | May need `<strings.h>` too |
| `<unistd.h>` | POSIX | Declare functions manually |
| `<stdint.h>` | C99 | Define types manually |
| `<stdbool.h>` | C99 | Define bool manually |

### K&R Fallbacks
```c
#ifndef HAVE_STDLIB_H
extern void *malloc();
extern void *realloc();
extern void free();
extern void exit();
extern void abort();
extern int atoi();
extern long atol();
extern char *getenv();
#endif

#ifndef NULL
#define NULL ((void *)0)
#endif

#ifndef HAVE_STDBOOL_H
typedef int bool;
#define true 1
#define false 0
#endif
```

### Header Include Order

Recommended order for maximum compatibility:

```c
/* 1. Feature test macros (before any headers) */
#define _POSIX_SOURCE
#define _BSD_SOURCE

/* 2. System headers */
#include <sys/types.h>   /* Before most other sys headers */
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/wait.h>

/* 3. Network headers */
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* 4. Standard C headers */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>

/* 5. POSIX headers */
#include <unistd.h>
#include <dirent.h>
#include <termios.h>
```

---

## 14. Path Limits

| Constant | Header | Notes |
|----------|--------|-------|
| `PATH_MAX` | `<limits.h>` | POSIX, value varies (256-4096+) |
| `MAXPATHLEN` | `<sys/param.h>` | BSD |
| `NAME_MAX` | `<limits.h>` | Max filename component |
| `_POSIX_PATH_MAX` | `<limits.h>` | Minimum guaranteed (256) |

### Portable Definition
```c
#ifdef PATH_MAX
#define MAX_PATH PATH_MAX
#elif defined(MAXPATHLEN)
#define MAX_PATH MAXPATHLEN
#elif defined(_POSIX_PATH_MAX)
#define MAX_PATH _POSIX_PATH_MAX
#else
#define MAX_PATH 1024  /* Safe default */
#endif
```

### Actual Limits by System

| System | PATH_MAX |
|--------|----------|
| Linux | 4096 |
| Solaris | 1024 |
| HP-UX | 1024 |
| AIX | 1023 |
| IRIX | 1024 |
| BSD variants | 1024 |
| macOS | 1024 |
| NeXTSTEP | 1024 |

---

## 15. Byte Order

### Byte Order Functions

These should be universal via `<netinet/in.h>` or `<arpa/inet.h>`:

- `htonl()` - Host to network long (32-bit)
- `htons()` - Host to network short (16-bit)
- `ntohl()` - Network to host long
- `ntohs()` - Network to host short

### Header Location
```c
#include <sys/types.h>
#include <netinet/in.h>  /* Usually here */
/* or */
#include <arpa/inet.h>   /* Sometimes here */
```

### Architecture Byte Orders

| Architecture | Endianness | Systems |
|--------------|------------|---------|
| SPARC | Big | Solaris, SunOS |
| PA-RISC | Big | HP-UX |
| MIPS | Big (usually) | IRIX, Ultrix |
| PowerPC/POWER | Big | AIX, old macOS |
| m68k | Big | NeXTSTEP, old Mac, Amiga |
| Alpha | Little | Tru64/OSF1 |
| x86/x86_64 | Little | Linux, *BSD, modern macOS |
| VAX | Little | Ultrix, VMS |
| ARM | Configurable | Usually little |

---

## 16. Non-blocking I/O

### Flag Names

| Flag | Standard | Notes |
|------|----------|-------|
| `O_NONBLOCK` | POSIX | Preferred |
| `O_NDELAY` | BSD/SysV | Older name |
| `FNDELAY` | BSD | In `<fcntl.h>` |
| `FNONBLOCK` | Some systems | Rare |

### Portable Definition
```c
#ifdef O_NONBLOCK
#define NONBLOCK_FLAG O_NONBLOCK
#elif defined(O_NDELAY)
#define NONBLOCK_FLAG O_NDELAY
#elif defined(FNDELAY)
#define NONBLOCK_FLAG FNDELAY
#else
#error "No non-blocking flag available"
#endif
```

### Setting Non-blocking Mode
```c
int flags = fcntl(fd, F_GETFL, 0);
if (flags < 0) flags = 0;
fcntl(fd, F_SETFL, flags | NONBLOCK_FLAG);
```

---

## 17. Compiler Differences

### Compiler by System

| System | Default cc | ANSI Mode | Alternative |
|--------|-----------|-----------|-------------|
| HP-UX | K&R bundled | `cc -Aa` | `/opt/ansic/bin/cc` |
| Solaris | Varies | `/opt/SUNWspro/bin/cc` | gcc |
| AIX | xlc | `xlc -qlanglvl=ansi` | gcc |
| IRIX | MIPSpro | `cc -ansi` | gcc |
| Tru64/OSF1 | DEC cc | `cc -std1` | gcc |
| NeXTSTEP | NeXT cc | Default is fine | |
| Ultrix | Various | Varies | |
| SCO | SCO cc | `cc -a` | gcc |
| BSD variants | gcc or clang | Default | |
| Linux | gcc or clang | Default | |

### K&R vs ANSI C Differences

| Feature | K&R | ANSI |
|---------|-----|------|
| Function prototypes | No | Yes |
| `void` keyword | No | Yes |
| `const` keyword | No | Yes |
| `volatile` keyword | No | Yes |
| Variable declarations | Start of block only | Start of block only (C89) |
| `//` comments | No | No (C89), Yes (C99) |
| Mixed declarations | No | No (C89), Yes (C99) |

### Writing K&R-Compatible Code

```c
/* K&R function definition */
int foo(a, b)
    int a;
    char *b;
{
    return a;
}

/* ANSI function definition */
int foo(int a, char *b) {
    return a;
}

/* Compatible with both (declaration only) */
int foo();  /* K&R */
int foo(int a, char *b);  /* ANSI */
```

### Predefined Macros by Compiler

| Macro | Compiler |
|-------|----------|
| `__GNUC__` | GCC |
| `__clang__` | Clang |
| `__SUNPRO_C` | Sun Studio |
| `__IBMC__` | IBM XL C |
| `__HP_cc` | HP C |
| `__sgi` | SGI MIPSpro |
| `__DECC` | DEC/Compaq C |

### Predefined Macros by OS

| Macro | System |
|-------|--------|
| `__sun` or `sun` | Solaris/SunOS |
| `__hpux` or `hpux` | HP-UX |
| `__sgi` or `sgi` | IRIX |
| `_AIX` | AIX |
| `__osf__` | Tru64/OSF1 |
| `__FreeBSD__` | FreeBSD |
| `__NetBSD__` | NetBSD |
| `__OpenBSD__` | OpenBSD |
| `__linux__` | Linux |
| `__APPLE__` | macOS/Darwin |
| `NeXT` | NeXTSTEP |

---

## 18. Platform-Specific Notes

### NeXTSTEP

- **OS Type**: Mach kernel + BSD 4.3 userland
- **Architecture**: m68k (original), i386 (later)
- **Terminal**: Uses sgtty, not termios
- **Directories**: Uses `<sys/dir.h>`, `struct direct`
- **Compiler**: Modified GCC, ANSI-compatible
- **Sockets**: In libc, no extra libraries

```c
#ifdef NeXT
#include <sgtty.h>
#include <sys/dir.h>
#define USE_SGTTY
#endif
```

### HP-UX

- **Architecture**: PA-RISC (older), Itanium (newer)
- **Bundled Compiler**: K&R only, use `-Aa` for ANSI
- **ANSI Compiler**: `/opt/ansic/bin/cc` or buy separately
- **Shared Libraries**: `.sl` extension, not `.so`
- **Library Check**: `chatr` instead of `ldd`
- **Terminal**: termios available

```c
#ifdef __hpux
/* May need -Aa for ANSI mode */
/* May need -lnsl for networking */
#endif
```

### Solaris / SunOS

- **SunOS 4.x**: BSD-based
- **Solaris (SunOS 5.x)**: SVR4-based
- **Architecture**: SPARC (big-endian), x86 (little-endian)
- **Libraries**: Need `-lsocket -lnsl`
- **Compiler**: Sun Studio in `/opt/SUNWspro/bin`
- **BSD Compat**: `/usr/ucb` has BSD-style commands

```c
#ifdef __sun
/* Solaris / SunOS */
#ifdef __SVR4
/* Solaris (SunOS 5.x) - needs -lsocket -lnsl */
#else
/* SunOS 4.x - BSD based */
#endif
#endif
```

### IRIX

- **Vendor**: Silicon Graphics (SGI)
- **Architecture**: MIPS (big-endian)
- **ABIs**: o32 (32-bit), n32 (new 32-bit), n64 (64-bit)
- **Compiler**: MIPSpro, use `-ansi` for ANSI mode
- **Sockets**: In libc

```c
#ifdef __sgi
/* IRIX */
#endif
```

### AIX

- **Vendor**: IBM
- **Architecture**: PowerPC/POWER (big-endian)
- **Object Format**: XCOFF, not ELF
- **Compiler**: xlc, use `-qlanglvl=ansi`
- **Sockets**: In libc

```c
#ifdef _AIX
/* AIX */
#endif
```

### Tru64 / OSF1 / Digital Unix

- **Vendors**: DEC, Compaq, HP
- **Architecture**: Alpha (64-bit, little-endian)
- **Compiler**: DEC C, use `-std1` for ANSI
- **Sockets**: In libc

```c
#ifdef __osf__
/* Tru64 / OSF1 */
#endif
```

### Ultrix

- **Vendor**: DEC
- **Architecture**: VAX or MIPS
- **Base**: BSD 4.2/4.3
- **Very Old**: May lack many POSIX features

```c
#ifdef ultrix
/* Ultrix */
#endif
```

### SCO Unix / UnixWare

- **Architecture**: x86
- **SCO OpenServer**: SVR3-based (older)
- **UnixWare**: SVR4-based
- **Libraries**: Need `-lsocket`

```c
#ifdef _SCO_DS
/* SCO OpenServer */
#endif
#ifdef __USLC__
/* UnixWare */
#endif
```

### FreeBSD / NetBSD / OpenBSD

- **Architecture**: Various (x86, ARM, etc.)
- **Generally**: POSIX-compliant with BSD extensions
- **Modern**: Usually have most features
- **Sockets**: In libc

```c
#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
/* BSD variant */
#endif
```

---

## 19. Makefile Portability

### Make Variants

| Make | Systems | Notes |
|------|---------|-------|
| BSD make | BSD variants | Different syntax from GNU |
| System V make | HP-UX, Solaris, AIX, IRIX | Basic features only |
| GNU make | Linux, available elsewhere | Most features |

### Portable Features (use these)

- Suffix rules: `.c.o:`
- Macros: `$(CC)`, `$(CFLAGS)`
- Targets and dependencies
- Tab for recipe lines

### Non-Portable Features (avoid these)

| Feature | Make Variant |
|---------|-------------|
| Pattern rules (`%.o: %.c`) | GNU only |
| `$(shell ...)` | GNU only |
| `ifdef` / `ifndef` | GNU only |
| `$(wildcard ...)` | GNU only |
| `.PHONY` | GNU, some BSD |
| `$<` in explicit rules | Varies |
| `include` directive | Varies |

### Portable Makefile Example
```makefile
# Portable Makefile
CC = cc
CFLAGS =
LIBS =

# Suffix rule (portable)
.c.o:
	$(CC) $(CFLAGS) -c $<

# Targets
all: claude-telepresence

claude-telepresence: client.o
	$(CC) $(CFLAGS) -o claude-telepresence client.o $(LIBS)

client.o: client.c

clean:
	rm -f claude-telepresence *.o
```

---

## 20. Configure Script Techniques

### Shell Portability

Use `/bin/sh`, not `/bin/bash`. Avoid:

| Avoid | Use Instead |
|-------|-------------|
| `$()` command substitution | Backticks `` ` ` `` |
| `[[ ]]` tests | `[ ]` tests |
| `==` in tests | `=` for string compare |
| Arrays | Multiple variables |
| `local` keyword | No local variables |
| `function` keyword | `func() { }` syntax |
| `echo -n` | `printf` |
| `source` | `.` (dot) |

### Basic Detection Template
```sh
#!/bin/sh
# configure - portable configuration script

# Initialize
CC="${CC:-cc}"
CFLAGS=""
LIBS=""

# Detect OS
OS=`uname -s`
echo "Operating system: $OS"

# Create temporary directory
TMPDIR="${TMPDIR:-/tmp}"
CONF_DIR="$TMPDIR/conf.$$"
mkdir -p "$CONF_DIR"
trap "rm -rf $CONF_DIR" 0 1 2 15

# Helper: try to compile a test program
try_compile() {
    echo "$1" > "$CONF_DIR/test.c"
    $CC $CFLAGS -o "$CONF_DIR/test" "$CONF_DIR/test.c" $LIBS 2>/dev/null
}

# Helper: try to compile and run
try_run() {
    if try_compile "$1"; then
        "$CONF_DIR/test"
        return $?
    fi
    return 1
}

# Helper: check for header
check_header() {
    echo "Checking for $1... \c"
    if try_compile "#include <$1>
int main() { return 0; }"; then
        echo "yes"
        return 0
    else
        echo "no"
        return 1
    fi
}

# Helper: check for function
check_func() {
    echo "Checking for $1... \c"
    if try_compile "char $1(); int main() { $1(); return 0; }"; then
        echo "yes"
        return 0
    else
        echo "no"
        return 1
    fi
}

# OS-specific setup
case "$OS" in
    SunOS)
        case `uname -r` in
            5.*) LIBS="-lsocket -lnsl" ;;
        esac
        ;;
    HP-UX)
        # Test ANSI mode
        if try_compile "int main(void) { return 0; }"; then
            : # OK
        else
            CFLAGS="-Aa"
            if try_compile "int main(void) { return 0; }"; then
                echo "Using -Aa for ANSI mode"
            fi
        fi
        ;;
esac

# Check headers
check_header termios.h && CFLAGS="$CFLAGS -DHAVE_TERMIOS_H"
check_header termio.h && CFLAGS="$CFLAGS -DHAVE_TERMIO_H"
check_header sgtty.h && CFLAGS="$CFLAGS -DHAVE_SGTTY_H"
check_header dirent.h && CFLAGS="$CFLAGS -DHAVE_DIRENT_H"
check_header sys/dir.h && CFLAGS="$CFLAGS -DHAVE_SYS_DIR_H"
check_header stdint.h && CFLAGS="$CFLAGS -DHAVE_STDINT_H"
check_header unistd.h && CFLAGS="$CFLAGS -DHAVE_UNISTD_H"

# Check functions
check_func snprintf && CFLAGS="$CFLAGS -DHAVE_SNPRINTF"
check_func strerror && CFLAGS="$CFLAGS -DHAVE_STRERROR"
check_func sigaction && CFLAGS="$CFLAGS -DHAVE_SIGACTION"
check_func memmove && CFLAGS="$CFLAGS -DHAVE_MEMMOVE"

# Check types
echo "Checking for socklen_t... \c"
if try_compile "#include <sys/types.h>
#include <sys/socket.h>
int main() { socklen_t x; return 0; }"; then
    echo "yes"
    CFLAGS="$CFLAGS -DHAVE_SOCKLEN_T"
else
    echo "no"
fi

# Output
echo ""
echo "Configuration:"
echo "  CC = $CC"
echo "  CFLAGS = $CFLAGS"
echo "  LIBS = $LIBS"
echo ""
echo "Run: $CC $CFLAGS -o claude-telepresence client.c $LIBS"

# Generate Makefile
cat > Makefile << EOF
CC = $CC
CFLAGS = $CFLAGS
LIBS = $LIBS

all: claude-telepresence

claude-telepresence: client.c
	\$(CC) \$(CFLAGS) -o claude-telepresence client.c \$(LIBS)

clean:
	rm -f claude-telepresence *.o
EOF

echo ""
echo "Makefile generated. Run 'make' to build."
```

---

## Summary: Current client.c Portability Risks

| Item | Risk | Status |
|------|------|--------|
| `uint8_t`, `uint16_t`, `uint32_t` | High | Needs stdint.h or fallback |
| `snprintf` | High | May not exist |
| termios vs sgtty | Medium | Already handled |
| dirent.h vs sys/dir.h | Medium | Already handled |
| Socket libraries | Medium | Needs configure detection |
| `strerror` | Low | Fallback available |
| `sigaction` | Low | Can use signal() |
| `memmove` | Low | Can use bcopy |

### Priority for Configure Script

1. **Detect OS** - Set library requirements
2. **Detect compiler mode** - ANSI vs K&R
3. **Check stdint.h** - Define fallback types
4. **Check snprintf** - Critical for safety
5. **Check terminal API** - termios/termio/sgtty
6. **Check directory API** - dirent.h variants
7. **Check socket type** - socklen_t

---

## References

- POSIX.1-2008 (IEEE Std 1003.1)
- The Single UNIX Specification, Version 4
- Advanced Programming in the UNIX Environment (Stevens)
- Portable C and Unix System Programming (Lapin)
- Autoconf documentation (for configure techniques)
