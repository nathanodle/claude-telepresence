/*
 * claude-telepresence client - K&R C version
 *
 * Thin client for legacy Unix systems (HP-UX, IRIX, NeXT, Solaris, etc.)
 * Connects to relay server, handles terminal I/O, executes operations locally.
 *
 * Written in K&R C for maximum portability to pre-ANSI systems.
 *
 * Build:
 *   HP-UX:   cc -o claude-telepresence client_kr.c -lsocket -lnsl
 *   IRIX:    cc -o claude-telepresence client_kr.c
 *   Solaris: cc -o claude-telepresence client_kr.c -lsocket -lnsl
 *   Linux:   gcc -o claude-telepresence client_kr.c
 *   NeXT:    cc -o claude-telepresence client_kr.c
 *   AIX:     cc -o claude-telepresence client_kr.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

/* Platform-specific headers */
/* NeXT uses sgtty instead of termios, and sys/dir.h instead of dirent.h */
/* Modern macOS defines __APPLE__ along with __MACH__, NeXTSTEP doesn't */
#if (defined(NeXT) || defined(__NeXT__)) || (defined(__MACH__) && !defined(__APPLE__))
#define NEXT_COMPAT 1
#include <libc.h>
#include <sys/dir.h>
#include <sgtty.h>
#define dirent direct
#else
/* sys/select.h doesn't exist on all systems - select() is in sys/time.h */
#ifndef __hpux
#include <sys/select.h>
#endif
#include <termios.h>
#include <dirent.h>
#endif

#ifdef __hpux
#include <sys/termios.h>
#endif

/* NeXT compatibility - missing POSIX macros and functions */
#ifdef NEXT_COMPAT
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif
#endif

/* snprintf compatibility - this file uses sprintf directly for K&R compat */

#define BUFFER_SIZE (10 * 1024 * 1024)  /* 10MB buffer for large files */
#define MAX_PATH 4096

/* Global state */
static int sockfd = -1;
#ifdef NEXT_COMPAT
static struct sgttyb orig_sgttyb;
static struct tchars orig_tchars;
static struct ltchars orig_ltchars;
static int orig_lmode;
#else
static struct termios orig_termios;
#endif
static int raw_mode = 0;
static int simple_mode = 0;  /* Filter fancy terminal effects */
static int resume_mode = 0;  /* Resume previous session */
static FILE *logfile = NULL; /* Debug log */

/* Buffers */
static char *recv_buffer = NULL;
static char *send_buffer = NULL;
static char *json_buffer = NULL;
static char *result_buffer = NULL;

/* Signal handling - use int instead of volatile sig_atomic_t for K&R */
static int got_sigwinch = 0;

static void sigwinch_handler(sig)
int sig;
{
    got_sigwinch = 1;
}

/* ============================================================================
 * Terminal handling
 * ============================================================================ */

#ifdef NEXT_COMPAT
static void disable_raw_mode()
{
    if (raw_mode) {
        ioctl(STDIN_FILENO, TIOCSETP, &orig_sgttyb);
        ioctl(STDIN_FILENO, TIOCSETC, &orig_tchars);
        ioctl(STDIN_FILENO, TIOCSLTC, &orig_ltchars);
        ioctl(STDIN_FILENO, TIOCLSET, &orig_lmode);
        raw_mode = 0;
    }
}

static void enable_raw_mode()
{
    struct sgttyb raw_sgttyb;
    struct tchars raw_tchars;
    struct ltchars raw_ltchars;
    int lmode;

    if (ioctl(STDIN_FILENO, TIOCGETP, &orig_sgttyb) < 0) return;
    if (ioctl(STDIN_FILENO, TIOCGETC, &orig_tchars) < 0) return;
    if (ioctl(STDIN_FILENO, TIOCGLTC, &orig_ltchars) < 0) return;
    if (ioctl(STDIN_FILENO, TIOCLGET, &orig_lmode) < 0) return;

    raw_sgttyb = orig_sgttyb;
    raw_sgttyb.sg_flags |= RAW;
    raw_sgttyb.sg_flags &= ~ECHO;

    /* Disable special characters */
    memset(&raw_tchars, -1, sizeof(raw_tchars));
    memset(&raw_ltchars, -1, sizeof(raw_ltchars));
    lmode = orig_lmode;

    if (ioctl(STDIN_FILENO, TIOCSETP, &raw_sgttyb) < 0) return;
    ioctl(STDIN_FILENO, TIOCSETC, &raw_tchars);
    ioctl(STDIN_FILENO, TIOCSLTC, &raw_ltchars);
    ioctl(STDIN_FILENO, TIOCLSET, &lmode);

    raw_mode = 1;
    atexit(disable_raw_mode);
}
#else
static void disable_raw_mode()
{
    if (raw_mode) {
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
        raw_mode = 0;
    }
}

static void enable_raw_mode()
{
    struct termios raw;

    if (tcgetattr(STDIN_FILENO, &orig_termios) == -1) return;

    raw = orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return;

    raw_mode = 1;
    atexit(disable_raw_mode);
}
#endif

static void get_terminal_size(rows, cols)
int *rows;
int *cols;
{
#ifdef TIOCGWINSZ
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0) {
        *rows = ws.ws_row;
        *cols = ws.ws_col;
        return;
    }
#endif
    *rows = 24;
    *cols = 80;
}

/* Spinner state for animated -\|/ */
static int spinner_state = 0;
static char spinner_chars[] = "-\\|/";

/* Filter for simple mode: convert Unicode to ASCII and strip color codes */
static int filter_terminal_output(in, out, outlen)
char *in;
char *out;
int outlen;
{
    unsigned char *p;
    int inlen;
    int j = 0;
    int i = 0;
    unsigned char c, ch, cmd;
    int start;
    int looks_like_sgr, has_question;

    p = (unsigned char *)in;
    inlen = strlen(in);

    while (i < inlen && j < outlen - 1) {
        c = p[i];

        /* Handle ESC sequences - strip colors, keep everything else */
        if (c == 0x1b && (i + 1) < inlen && p[i + 1] == '[') {
            /* CSI sequence: ESC [ params command */
            start = i;
            i += 2;  /* skip ESC [ */

            /* Check if this looks like SGR (color) - only digits, semicolons, and 'm' */
            looks_like_sgr = 1;
            has_question = 0;

            /* Scan for command letter */
            while (i < inlen && p[i] >= 0x20 && p[i] < 0x40) {
                ch = p[i];
                /* SGR uses digits, semicolons, and colons (for extended color syntax) */
                /* If we see ? or other chars, it's not SGR */
                if (ch == '?') has_question = 1;
                if (ch != ';' && ch != ':' && (ch < '0' || ch > '9')) {
                    looks_like_sgr = 0;
                }
                i++;
            }

            if (i >= inlen) {
                /* Incomplete sequence at end */
                if (looks_like_sgr && !has_question) {
                    /* Probably a color code - discard */
                    break;
                }
                /* Otherwise pass through */
                while (start < inlen && j < outlen - 1) {
                    out[j++] = p[start++];
                }
                break;
            }

            cmd = p[i++];

            /* Strip SGR (color/formatting) which ends with 'm' */
            if (cmd == 'm') {
                /* Skip - don't copy */
            } else {
                /* Copy the whole sequence */
                while (start < i && j < outlen - 1) {
                    out[j++] = p[start++];
                }
            }
            continue;
        }

        /* Pass through other ASCII */
        if (c < 0x80) {
            out[j++] = c;
            i++;
            continue;
        }

        /* Handle UTF-8 sequences - convert known ones to ASCII */

        /* 3-byte UTF-8: E2 xx xx - includes box drawing, arrows, symbols */
        if (c == 0xE2 && (i + 2) < inlen) {
            unsigned char b1 = p[i + 1];
            unsigned char b2 = p[i + 2];

            /* Box drawing U+2500-U+257F (E2 94 80 - E2 95 BF) */
            if (b1 == 0x94) {
                /* Light box drawing */
                if (b2 >= 0x80 && b2 <= 0x84) out[j++] = '-';      /* horizontal lines */
                else if (b2 >= 0x82 && b2 <= 0x83) out[j++] = '|'; /* vertical lines */
                else out[j++] = '+';                               /* corners, etc */
                i += 3;
                continue;
            }
            if (b1 == 0x95) {
                /* Heavy/double box drawing */
                if (b2 >= 0x90 && b2 <= 0x94) out[j++] = '=';      /* double horizontal */
                else if (b2 >= 0x91 && b2 <= 0x93) out[j++] = '|'; /* double vertical */
                /* Rounded corners E2 95 AD-B0 */
                else out[j++] = '+';
                i += 3;
                continue;
            }

            /* Arrows U+2190-U+21FF (E2 86 xx) */
            if (b1 == 0x86) {
                if (b2 == 0x90) out[j++] = '<';       /* leftwards arrow */
                else if (b2 == 0x91) out[j++] = '^';  /* upwards arrow */
                else if (b2 == 0x92) out[j++] = '>';  /* rightwards arrow */
                else if (b2 == 0x93) out[j++] = 'v';  /* downwards arrow */
                else out[j++] = '>';                  /* other arrows default to > */
                i += 3;
                continue;
            }

            /* Geometric shapes U+25A0-U+25FF (E2 96 xx, E2 97 xx) */
            if (b1 == 0x96) {
                if (b2 == 0xB2) out[j++] = '^';       /* up triangle */
                else if (b2 == 0xB3) out[j++] = '^';  /* white up triangle */
                else if (b2 == 0xB4) out[j++] = '>';  /* small up triangle */
                else if (b2 == 0xB5) out[j++] = '>';  /* small white up triangle */
                else if (b2 == 0xB6) out[j++] = '>';  /* right triangle */
                else if (b2 == 0xB7) out[j++] = '>';  /* white right triangle */
                else if (b2 == 0xB8) out[j++] = '>';  /* small right triangle */
                else if (b2 == 0xB9) out[j++] = '>';  /* small white right triangle */
                else if (b2 == 0xBA) out[j++] = 'v';  /* small down triangle */
                else if (b2 == 0xBB) out[j++] = 'v';  /* small white down triangle */
                else if (b2 == 0xBC) out[j++] = 'v';  /* down triangle */
                else if (b2 == 0xBD) out[j++] = 'v';  /* white down triangle */
                else out[j++] = '*';                  /* other shapes */
                i += 3;
                continue;
            }
            if (b1 == 0x97) {
                if (b2 == 0x80) out[j++] = '<';       /* left triangle */
                else if (b2 == 0x81) out[j++] = '<';  /* white left triangle */
                else if (b2 == 0x82) out[j++] = '<';  /* small left triangle */
                else if (b2 == 0x83) out[j++] = '<';  /* small white left triangle */
                else if (b2 == 0x8F) {               /* bullet - use as spinner */
                    out[j++] = spinner_chars[spinner_state];
                    spinner_state = (spinner_state + 1) & 3;
                }
                else if (b2 == 0x8B) out[j++] = 'o';  /* white circle */
                else if (b2 == 0x86 || b2 == 0x87) out[j++] = '*'; /* diamonds */
                else out[j++] = '*';                  /* other shapes */
                i += 3;
                continue;
            }

            /* Dingbats - checkmarks, X marks, stars U+2700-U+27BF (E2 9C xx, E2 9D xx) */
            if (b1 == 0x9C) {
                /* Checkmarks: U+2713, U+2714 */
                if (b2 == 0x93 || b2 == 0x94) {
                    out[j++] = '+';
                    i += 3;
                    continue;
                }
                /* X marks: U+2717, U+2718 */
                if (b2 == 0x97 || b2 == 0x98) {
                    out[j++] = 'x';
                    i += 3;
                    continue;
                }
                /* Spinner stars/asterisks - animate! */
                if (b2 == 0xA2 || b2 == 0xB3 || b2 == 0xB6 ||
                    b2 == 0xBB || b2 == 0xBD) {
                    out[j++] = spinner_chars[spinner_state];
                    spinner_state = (spinner_state + 1) & 3;
                    i += 3;
                    continue;
                }
                /* U+2705 - green checkmark emoji */
                if (b2 == 0x85) {
                    out[j++] = '+';
                    i += 3;
                    continue;
                }
                /* Other dingbats */
                out[j++] = '*';
                i += 3;
                continue;
            }
            if (b1 == 0x9D) {
                /* U+274C - red X emoji */
                if (b2 == 0x8C) {
                    out[j++] = 'x';
                    i += 3;
                    continue;
                }
                out[j++] = '*';
                i += 3;
                continue;
            }

            /* Heavy arrows/dingbats U+2780-U+27BF (E2 9E xx) */
            if (b1 == 0x9E) {
                /* Most are arrow variants - map to > */
                out[j++] = '>';
                i += 3;
                continue;
            }

            /* Mathematical operators U+2200-U+22FF (E2 88 xx) */
            if (b1 == 0x88) {
                /* U+2234 - therefore, used as thinking indicator */
                if (b2 == 0xB4) {
                    out[j++] = spinner_chars[spinner_state];
                    spinner_state = (spinner_state + 1) & 3;
                    i += 3;
                    continue;
                }
                out[j++] = '*';
                i += 3;
                continue;
            }

            /* Miscellaneous Technical U+2300-U+23FF (E2 8C xx, E2 8D xx, E2 8E xx, E2 8F xx) */
            if (b1 >= 0x8C && b1 <= 0x8F) {
                /* Various technical symbols */
                out[j++] = '>';
                i += 3;
                continue;
            }

            /* General punctuation U+2000-U+206F (E2 80 xx) */
            if (b1 == 0x80) {
                if (b2 == 0xA2) out[j++] = '*';       /* bullet */
                else if (b2 == 0xA3) out[j++] = '>';  /* triangular bullet */
                else if (b2 >= 0x93 && b2 <= 0x95) out[j++] = '-'; /* dashes */
                else if (b2 == 0x98 || b2 == 0x99) out[j++] = '\''; /* quotes */
                else if (b2 == 0x9C || b2 == 0x9D) out[j++] = '"';  /* quotes */
                else if (b2 == 0xA6) out[j++] = '.';  /* ellipsis -> . */
                else if (b2 == 0xB9) out[j++] = '<';  /* single angle quote */
                else if (b2 == 0xBA) out[j++] = '>';  /* single angle quote */
                else out[j++] = ' ';
                i += 3;
                continue;
            }

            /* Unknown 3-byte E2 sequence */
            out[j++] = '?';
            i += 3;
            continue;
        }

        /* 2-byte UTF-8: C2/C3 xx */
        if ((c == 0xC2 || c == 0xC3) && (i + 1) < inlen) {
            unsigned char b1 = p[i + 1];
            if (c == 0xC2 && b1 == 0xA0) {
                out[j++] = ' ';  /* NBSP */
            } else if (c == 0xC2 && b1 == 0xB7) {
                /* middle dot U+00B7 - use as spinner */
                out[j++] = spinner_chars[spinner_state];
                spinner_state = (spinner_state + 1) & 3;
            } else {
                /* Other Latin-1 supplement - try to pass through or use ? */
                out[j++] = '?';
            }
            i += 2;
            continue;
        }

        /* 4-byte UTF-8: F0 xx xx xx - includes emoji */
        if (c == 0xF0 && (i + 3) < inlen) {
            unsigned char b1 = p[i + 1];
            /* Most emoji are in F0 9F xx xx range */
            if (b1 == 0x9F) {
                /* Common emoji mappings */
                /* Just use a generic placeholder for now */
                out[j++] = '*';
            } else {
                out[j++] = '?';
            }
            i += 4;
            continue;
        }

        /* Other multi-byte UTF-8 - skip with placeholder */
        if ((c & 0xE0) == 0xC0) { out[j++] = '?'; i += 2; continue; }
        if ((c & 0xF0) == 0xE0) { out[j++] = '?'; i += 3; continue; }
        if ((c & 0xF8) == 0xF0) { out[j++] = '?'; i += 4; continue; }

        /* Shouldn't get here, but skip byte */
        i++;
    }

    out[j] = '\0';
    return j;
}

/* ============================================================================
 * Simple JSON helpers (no external dependencies)
 * ============================================================================ */

static void json_escape_string(in, out, outlen)
char *in;
char *out;
int outlen;
{
    int i = 0;
    unsigned char c;

    out[i++] = '"';
    while (*in && i < outlen - 2) {
        c = (unsigned char)*in;
        if (c == '"') {
            if (i + 2 >= outlen - 1) break;
            out[i++] = '\\'; out[i++] = '"';
        } else if (c == '\\') {
            if (i + 2 >= outlen - 1) break;
            out[i++] = '\\'; out[i++] = '\\';
        } else if (c == '\n') {
            if (i + 2 >= outlen - 1) break;
            out[i++] = '\\'; out[i++] = 'n';
        } else if (c == '\r') {
            if (i + 2 >= outlen - 1) break;
            out[i++] = '\\'; out[i++] = 'r';
        } else if (c == '\t') {
            if (i + 2 >= outlen - 1) break;
            out[i++] = '\\'; out[i++] = 't';
        } else if (c < 32) {
            /* Encode other control characters as \uXXXX */
            if (i + 6 >= outlen - 1) break;
            out[i++] = '\\';
            out[i++] = 'u';
            out[i++] = '0';
            out[i++] = '0';
            out[i++] = "0123456789abcdef"[(c >> 4) & 0xf];
            out[i++] = "0123456789abcdef"[c & 0xf];
        } else {
            out[i++] = c;
        }
        in++;
    }
    out[i++] = '"';
    out[i] = '\0';
}

static char *json_get_string(json, key, out, outlen)
char *json;
char *key;
char *out;
int outlen;
{
    char search[256];
    char *start, *end;
    int len;
    char *r, *w;

    sprintf(search, "\"%s\":", key);
    start = strstr(json, search);
    if (!start) {
        out[0] = '\0';
        return out;
    }

    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;

    if (*start == '"') {
        start++;
        end = start;
        while (*end && !(*end == '"' && *(end-1) != '\\')) end++;
        len = end - start;
        if (len >= outlen) len = outlen - 1;
        memcpy(out, start, len);
        out[len] = '\0';

        /* Unescape */
        r = out;
        w = out;
        while (*r) {
            if (*r == '\\' && *(r+1)) {
                r++;
                switch (*r) {
                    case 'n': *w++ = '\n'; r++; break;
                    case 'r': *w++ = '\r'; r++; break;
                    case 't': *w++ = '\t'; r++; break;
                    case '"': *w++ = '"'; r++; break;
                    case '\\': *w++ = '\\'; r++; break;
                    case 'u':
                        /* Unicode escape \uXXXX */
                        if (r[1] && r[2] && r[3] && r[4]) {
                            char hex[5];
                            unsigned int codepoint;
                            hex[0] = r[1]; hex[1] = r[2];
                            hex[2] = r[3]; hex[3] = r[4]; hex[4] = 0;
                            codepoint = (unsigned int)strtoul(hex, NULL, 16);
                            if (codepoint < 0x80) {
                                *w++ = (char)codepoint;
                            } else if (codepoint < 0x800) {
                                *w++ = (char)(0xC0 | (codepoint >> 6));
                                *w++ = (char)(0x80 | (codepoint & 0x3F));
                            } else {
                                *w++ = (char)(0xE0 | (codepoint >> 12));
                                *w++ = (char)(0x80 | ((codepoint >> 6) & 0x3F));
                                *w++ = (char)(0x80 | (codepoint & 0x3F));
                            }
                            r += 5;
                        } else {
                            *w++ = *r++;
                        }
                        break;
                    default: *w++ = *r++;
                }
            } else {
                *w++ = *r++;
            }
        }
        *w = '\0';
    } else {
        out[0] = '\0';
    }
    return out;
}

static long json_get_int(json, key)
char *json;
char *key;
{
    char search[256];
    char *start;

    sprintf(search, "\"%s\":", key);
    start = strstr(json, search);
    if (!start) return -1;

    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;

    return atol(start);
}

static int json_get_bool(json, key)
char *json;
char *key;
{
    char search[256];
    char *start;

    sprintf(search, "\"%s\":", key);
    start = strstr(json, search);
    if (!start) return 0;

    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;

    return (strncmp(start, "true", 4) == 0);
}

/* ============================================================================
 * Network functions
 * ============================================================================ */

static int connect_to_relay(host, port)
char *host;
int port;
{
    struct hostent *server;
    struct sockaddr_in serv_addr;
    unsigned long addr;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    /* Try numeric IP first (old gethostbyname doesn't handle IPs) */
    addr = inet_addr(host);
    if (addr != (unsigned long)-1) {
        serv_addr.sin_addr.s_addr = addr;
    } else {
        /* Fall back to hostname lookup */
        server = gethostbyname(host);
        if (!server) {
            fprintf(stderr, "Cannot resolve host: %s\n", host);
            close(sockfd);
            return -1;
        }
        memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    return 0;
}

static int send_message(json)
char *json;
{
    int len;
    unsigned char header[4];
    int sent;
    int n;

    len = strlen(json);

    /* Big-endian length header */
    header[0] = (len >> 24) & 0xFF;
    header[1] = (len >> 16) & 0xFF;
    header[2] = (len >> 8) & 0xFF;
    header[3] = len & 0xFF;

    if (write(sockfd, header, 4) != 4) return -1;

    sent = 0;
    while (sent < len) {
        n = write(sockfd, json + sent, len - sent);
        if (n <= 0) return -1;
        sent += n;
    }

    return 0;
}

static int recv_message(buf, buflen)
char *buf;
int buflen;
{
    unsigned char header[4];
    int len, received;
    int n;

    /* Read length header */
    if (read(sockfd, header, 4) != 4) return -1;

    len = ((int)header[0] << 24) | ((int)header[1] << 16) |
          ((int)header[2] << 8) | (int)header[3];

    if (len >= buflen) {
        fprintf(stderr, "Message too large: %d\n", len);
        return -1;
    }

    received = 0;
    while (received < len) {
        n = read(sockfd, buf + received, len - received);
        if (n <= 0) return -1;
        received += n;
    }
    buf[len] = '\0';

    return len;
}

/* ============================================================================
 * Operation handlers
 * ============================================================================ */

static void handle_fs_readFile(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    FILE *fp;
    long file_len_signed;
    int file_len;
    char *content;
    char *encoded;
    int enc_len;
    static char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int i, j;
    unsigned int n;

    json_get_string(params, "path", path, sizeof(path));

    fp = fopen(path, "rb");
    if (!fp) {
        sprintf(response, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
        return;
    }

    fseek(fp, 0, SEEK_END);
    file_len_signed = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    /* Validate file_len to prevent overflow/underflow issues.
     * Max 7MB so base64 (~9.3MB) + JSON wrapper fits in 10MB response buffer */
    if (file_len_signed < 0 || file_len_signed > 7 * 1024 * 1024) {
        fclose(fp);
        sprintf(response, "{\"error\":\"File too large (max 7MB)\"}");
        return;
    }
    file_len = (int)file_len_signed;

    content = malloc(file_len + 1);
    if (!content) {
        fclose(fp);
        sprintf(response, "{\"error\":\"Out of memory\"}");
        return;
    }

    fread(content, 1, file_len, fp);
    content[file_len] = '\0';
    fclose(fp);

    /* Base64 encode */
    enc_len = ((file_len + 2) / 3) * 4 + 1;
    encoded = malloc(enc_len);
    if (!encoded) {
        free(content);
        sprintf(response, "{\"error\":\"Out of memory\"}");
        return;
    }

    j = 0;
    for (i = 0; i < file_len; i += 3) {
        n = ((unsigned char)content[i]) << 16;
        if (i + 1 < file_len) n |= ((unsigned char)content[i+1]) << 8;
        if (i + 2 < file_len) n |= ((unsigned char)content[i+2]);

        encoded[j++] = b64[(n >> 18) & 63];
        encoded[j++] = b64[(n >> 12) & 63];
        encoded[j++] = (i + 1 < file_len) ? b64[(n >> 6) & 63] : '=';
        encoded[j++] = (i + 2 < file_len) ? b64[n & 63] : '=';
    }
    encoded[j] = '\0';

    sprintf(response, "{\"result\":\"%s\"}", encoded);

    free(content);
    free(encoded);
}

static void handle_fs_writeFile(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    char *data;
    char *decoded;
    int data_len, dec_len;
    int is_buffer;
    FILE *fp;
    static int d[] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
    };
    int i, j;
    int a, b, c, e;

    json_get_string(params, "path", path, sizeof(path));
    is_buffer = json_get_bool(params, "isBuffer");

    data = malloc(BUFFER_SIZE);
    if (!data) {
        sprintf(response, "{\"error\":\"Out of memory\"}");
        return;
    }
    json_get_string(params, "data", data, BUFFER_SIZE);

    if (is_buffer) {
        /* Base64 decode */
        data_len = strlen(data);
        dec_len = (data_len / 4) * 3;
        decoded = malloc(dec_len + 1);
        if (!decoded) {
            free(data);
            sprintf(response, "{\"error\":\"Out of memory\"}");
            return;
        }

        j = 0;
        /* Process only complete 4-byte groups to avoid buffer over-read */
        for (i = 0; i + 3 < data_len; i += 4) {
            a = d[(unsigned char)data[i]];
            b = d[(unsigned char)data[i+1]];
            c = d[(unsigned char)data[i+2]];
            e = d[(unsigned char)data[i+3]];
            if (a < 0 || b < 0) break;
            decoded[j++] = (a << 2) | (b >> 4);
            if (c >= 0) decoded[j++] = ((b & 15) << 4) | (c >> 2);
            if (e >= 0) decoded[j++] = ((c & 3) << 6) | e;
        }
        dec_len = j;

        fp = fopen(path, "wb");
        if (!fp) {
            free(data);
            free(decoded);
            sprintf(response, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
            return;
        }
        fwrite(decoded, 1, dec_len, fp);
        fclose(fp);
        free(decoded);
    } else {
        fp = fopen(path, "w");
        if (!fp) {
            free(data);
            sprintf(response, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
            return;
        }
        fputs(data, fp);
        fclose(fp);
    }

    free(data);
    sprintf(response, "{\"result\":true}");
}

static void handle_fs_stat(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    struct stat st;

    json_get_string(params, "path", path, sizeof(path));

    if (stat(path, &st) < 0) {
        sprintf(response, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
        return;
    }

    sprintf(response,
        "{\"result\":{"
        "\"dev\":%lu,\"ino\":%lu,\"mode\":%u,\"nlink\":%lu,"
        "\"uid\":%u,\"gid\":%u,\"rdev\":%lu,\"size\":%lu,"
        "\"blksize\":%lu,\"blocks\":%lu,"
        "\"atimeMs\":%lu,\"mtimeMs\":%lu,\"ctimeMs\":%lu,\"birthtimeMs\":%lu,"
        "\"isFile\":%s,\"isDirectory\":%s,\"isSymbolicLink\":false"
        "}}",
        (unsigned long)st.st_dev, (unsigned long)st.st_ino, (unsigned)st.st_mode,
        (unsigned long)st.st_nlink, (unsigned)st.st_uid, (unsigned)st.st_gid,
        (unsigned long)st.st_rdev, (unsigned long)st.st_size,
        (unsigned long)st.st_blksize, (unsigned long)st.st_blocks,
        (unsigned long)st.st_atime * 1000, (unsigned long)st.st_mtime * 1000,
        (unsigned long)st.st_ctime * 1000, (unsigned long)st.st_ctime * 1000,
        S_ISREG(st.st_mode) ? "true" : "false",
        S_ISDIR(st.st_mode) ? "true" : "false"
    );
}

static void handle_fs_lstat(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    struct stat st;

    json_get_string(params, "path", path, sizeof(path));

    if (lstat(path, &st) < 0) {
        sprintf(response, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
        return;
    }

    sprintf(response,
        "{\"result\":{"
        "\"dev\":%lu,\"ino\":%lu,\"mode\":%u,\"nlink\":%lu,"
        "\"uid\":%u,\"gid\":%u,\"rdev\":%lu,\"size\":%lu,"
        "\"blksize\":%lu,\"blocks\":%lu,"
        "\"atimeMs\":%lu,\"mtimeMs\":%lu,\"ctimeMs\":%lu,\"birthtimeMs\":%lu,"
        "\"isFile\":%s,\"isDirectory\":%s,\"isSymbolicLink\":%s"
        "}}",
        (unsigned long)st.st_dev, (unsigned long)st.st_ino, (unsigned)st.st_mode,
        (unsigned long)st.st_nlink, (unsigned)st.st_uid, (unsigned)st.st_gid,
        (unsigned long)st.st_rdev, (unsigned long)st.st_size,
        (unsigned long)st.st_blksize, (unsigned long)st.st_blocks,
        (unsigned long)st.st_atime * 1000, (unsigned long)st.st_mtime * 1000,
        (unsigned long)st.st_ctime * 1000, (unsigned long)st.st_ctime * 1000,
        S_ISREG(st.st_mode) ? "true" : "false",
        S_ISDIR(st.st_mode) ? "true" : "false",
        S_ISLNK(st.st_mode) ? "true" : "false"
    );
}

static void handle_fs_readdir(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    DIR *dir;
    struct dirent *ent;
    int pos;
    int first = 1;
    char escaped_name[1024];
    char full_path[MAX_PATH + 256];
    struct stat st;

    json_get_string(params, "path", path, sizeof(path));

    dir = opendir(path);
    if (!dir) {
        sprintf(response, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
        return;
    }

    pos = sprintf(response, "{\"result\":[");

    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        json_escape_string(ent->d_name, escaped_name, sizeof(escaped_name));

        sprintf(full_path, "%s/%s", path, ent->d_name);

        if (pos + 256 >= resplen) break;

        if (!first) response[pos++] = ',';
        first = 0;

        if (stat(full_path, &st) == 0) {
            pos += sprintf(response + pos,
                "{\"name\":%s,\"isFile\":%s,\"isDirectory\":%s}",
                escaped_name,
                S_ISREG(st.st_mode) ? "true" : "false",
                S_ISDIR(st.st_mode) ? "true" : "false"
            );
        } else {
            pos += sprintf(response + pos, "%s", escaped_name);
        }
    }

    closedir(dir);
    sprintf(response + pos, "]}");
}

static void handle_fs_exists(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    struct stat st;

    json_get_string(params, "path", path, sizeof(path));

    if (stat(path, &st) == 0) {
        sprintf(response, "{\"result\":true}");
    } else {
        sprintf(response, "{\"result\":false}");
    }
}

static void handle_fs_mkdir(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];

    json_get_string(params, "path", path, sizeof(path));

    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        sprintf(response, "{\"error\":\"%s\"}", strerror(errno));
        return;
    }

    sprintf(response, "{\"result\":true}");
}

static void handle_fs_unlink(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];

    json_get_string(params, "path", path, sizeof(path));

    if (unlink(path) < 0) {
        sprintf(response, "{\"error\":\"%s\"}", strerror(errno));
        return;
    }

    sprintf(response, "{\"result\":true}");
}

static void handle_fs_rename(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char old_path[MAX_PATH], new_path[MAX_PATH];

    json_get_string(params, "oldPath", old_path, sizeof(old_path));
    json_get_string(params, "newPath", new_path, sizeof(new_path));

    if (rename(old_path, new_path) < 0) {
        sprintf(response, "{\"error\":\"%s\"}", strerror(errno));
        return;
    }

    sprintf(response, "{\"result\":true}");
}

static void handle_fs_access(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    int mode;

    json_get_string(params, "path", path, sizeof(path));
    mode = (int)json_get_int(params, "mode");
    if (mode < 0) mode = F_OK;

    if (access(path, mode) < 0) {
        sprintf(response, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
        return;
    }

    sprintf(response, "{\"result\":true}");
}

static void handle_fs_realpath(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    char resolved[MAX_PATH];
    char escaped[MAX_PATH * 2];

    json_get_string(params, "path", path, sizeof(path));

    if (!realpath(path, resolved)) {
        sprintf(response, "{\"error\":\"%s\"}", strerror(errno));
        return;
    }

    json_escape_string(resolved, escaped, sizeof(escaped));
    sprintf(response, "{\"result\":%s}", escaped);
}

/* ============================================================================
 * Native find and search implementations
 * ============================================================================ */

/* Simple wildcard pattern matcher (supports * and ?) */
static int match_pattern(pattern, str)
char *pattern;
char *str;
{
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;  /* trailing * matches all */
            while (*str) {
                if (match_pattern(pattern, str)) return 1;
                str++;
            }
            return 0;
        } else if (*pattern == '?' || *pattern == *str) {
            pattern++;
            str++;
        } else {
            return 0;
        }
    }
    /* Handle trailing * */
    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *str == '\0');
}

/* Recursive find helper - appends matches to result buffer */
static void find_recursive(dir_path, pattern, result, pos, maxlen, count, max_results, depth)
char *dir_path;
char *pattern;
char *result;
int *pos;
int maxlen;
int *count;
int max_results;
int depth;
{
    DIR *dir;
    struct dirent *ent;
    char full_path[MAX_PATH + 256];
    struct stat st;
    char escaped[MAX_PATH * 2 + 512];

    /* Limit recursion depth to prevent stack overflow */
    if (depth > 64) return;
    if (*count >= max_results) return;

    dir = opendir(dir_path);
    if (!dir) return;

    while ((ent = readdir(dir)) != NULL && *count < max_results) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        sprintf(full_path, "%s/%s", dir_path, ent->d_name);

        /* Use lstat to NOT follow symlinks - prevents infinite loops */
        if (lstat(full_path, &st) < 0) continue;

        /* Skip symlinks entirely to avoid loops */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Recurse into subdirectory */
            find_recursive(full_path, pattern, result, pos, maxlen, count, max_results, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            /* Check if filename matches pattern */
            if (match_pattern(pattern, ent->d_name)) {
                json_escape_string(full_path, escaped, sizeof(escaped));
                if (*pos + strlen(escaped) + 2 < maxlen) {
                    if (*count > 0) result[(*pos)++] = ',';
                    *pos += sprintf(result + *pos, "%s", escaped);
                    (*count)++;
                }
            }
        }
    }

    closedir(dir);
}

static void handle_fs_find(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    char pattern[256];
    int pos;
    int count = 0;

    json_get_string(params, "path", path, sizeof(path));
    json_get_string(params, "pattern", pattern, sizeof(pattern));

    if (!path[0]) strcpy(path, ".");
    if (!pattern[0]) strcpy(pattern, "*");

    pos = sprintf(response, "{\"result\":[");
    find_recursive(path, pattern, response, &pos, resplen - 10, &count, 200, 0);
    sprintf(response + pos, "]}");
}

/* Recursive search helper - searches file contents */
/* Static buffers for search to avoid stack bloat during recursion. */
static char search_line_buf[2048];
static char search_match_buf[8192];
static char search_escaped_buf[16384];

/* Check if directory should be skipped */
static int skip_directory(name)
char *name;
{
    /* Skip hidden directories (., .., .git, .svn, etc.) */
    if (name[0] == '.') return 1;
    /* Skip known large/slow directories */
    if (strcmp(name, "node_modules") == 0) return 1;
    if (strcmp(name, "__pycache__") == 0) return 1;
    if (strcmp(name, "CVS") == 0) return 1;
    if (strcmp(name, "RCS") == 0) return 1;
    return 0;
}

/* Check if file extension suggests binary */
static int is_binary_extension(name)
char *name;
{
    char *ext;
    ext = strrchr(name, '.');
    if (!ext) return 0;
    if (strcmp(ext, ".o") == 0 || strcmp(ext, ".a") == 0 ||
        strcmp(ext, ".so") == 0 || strcmp(ext, ".dylib") == 0 ||
        strcmp(ext, ".gz") == 0 || strcmp(ext, ".tar") == 0 ||
        strcmp(ext, ".zip") == 0 || strcmp(ext, ".Z") == 0 ||
        strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0 ||
        strcmp(ext, ".png") == 0 || strcmp(ext, ".gif") == 0 ||
        strcmp(ext, ".tiff") == 0 || strcmp(ext, ".tif") == 0 ||
        strcmp(ext, ".pdf") == 0 || strcmp(ext, ".ps") == 0 ||
        strcmp(ext, ".exe") == 0 || strcmp(ext, ".bin") == 0 ||
        strcmp(ext, ".obj") == 0 || strcmp(ext, ".class") == 0 ||
        strcmp(ext, ".pyc") == 0 || strcmp(ext, ".pyo") == 0) {
        return 1;
    }
    return 0;
}

static void search_recursive(dir_path, search_pattern, file_pattern, result, pos, maxlen,
                            match_count, max_matches, file_count, max_files, depth)
char *dir_path;
char *search_pattern;
char *file_pattern;
char *result;
int *pos;
int maxlen;
int *match_count;
int max_matches;
int *file_count;
int max_files;
int depth;
{
    DIR *dir;
    struct dirent *ent;
    char full_path[MAX_PATH + 256];
    struct stat st;
    FILE *fp;
    int line_num;
    int is_binary;
    int i, n;
    char *newline;
    char *line_start;
    char *line_end;

    /* Limit recursion depth - use smaller limit to save stack */
    if (depth > 32) return;
    if (*match_count >= max_matches) return;
    if (*file_count >= max_files) return;

    dir = opendir(dir_path);
    if (!dir) return;

    while ((ent = readdir(dir)) != NULL) {
        /* Check limits */
        if (*match_count >= max_matches) break;
        if (*file_count >= max_files) break;

        /* Skip . and .. */
        if (ent->d_name[0] == '.' &&
            (ent->d_name[1] == '\0' ||
             (ent->d_name[1] == '.' && ent->d_name[2] == '\0')))
            continue;

        sprintf(full_path, "%s/%s", dir_path, ent->d_name);

        /* Use lstat to NOT follow symlinks */
        if (lstat(full_path, &st) < 0) continue;

        /* Skip symlinks */
        if (S_ISLNK(st.st_mode)) continue;

        if (S_ISDIR(st.st_mode)) {
            /* Skip problematic directories */
            if (skip_directory(ent->d_name)) continue;
            /* Recurse */
            search_recursive(full_path, search_pattern, file_pattern, result, pos, maxlen,
                           match_count, max_matches, file_count, max_files, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            /* Check file pattern */
            if (file_pattern[0] && !match_pattern(file_pattern, ent->d_name)) continue;

            /* Skip by extension */
            if (is_binary_extension(ent->d_name)) continue;

            /* Skip large files (>512KB) */
            if (st.st_size > 512 * 1024) continue;

            /* Skip empty files */
            if (st.st_size == 0) continue;

            /* Count this file */
            (*file_count)++;

            /* Open and check for binary content */
            fp = fopen(full_path, "r");
            if (!fp) continue;

            /* Read first chunk and check for nulls */
            n = fread(search_line_buf, 1, 512, fp);
            is_binary = 0;
            for (i = 0; i < n; i++) {
                if (search_line_buf[i] == '\0') {
                    is_binary = 1;
                    break;
                }
            }
            if (is_binary) {
                fclose(fp);
                continue;
            }

            /* Search the first chunk we already read */
            search_line_buf[n] = '\0';
            line_num = 1;

            /* Process already-read data line by line */
            line_start = search_line_buf;
            while ((line_end = strchr(line_start, '\n')) != NULL && *match_count < max_matches) {
                *line_end = '\0';
                /* Remove CR if present */
                if (line_end > line_start && *(line_end-1) == '\r')
                    *(line_end-1) = '\0';

                if (strstr(line_start, search_pattern)) {
                    sprintf(search_match_buf, "%s:%d:%s",
                            full_path, line_num, line_start);
                    json_escape_string(search_match_buf, search_escaped_buf, sizeof(search_escaped_buf));
                    if (*pos + strlen(search_escaped_buf) + 2 < maxlen) {
                        if (*match_count > 0) result[(*pos)++] = ',';
                        *pos += sprintf(result + *pos, "%s", search_escaped_buf);
                        (*match_count)++;
                    }
                }
                line_num++;
                line_start = line_end + 1;
            }

            /* Continue reading rest of file */
            while (fgets(search_line_buf, sizeof(search_line_buf), fp) && *match_count < max_matches) {
                line_num++;
                if (line_num > 5000) break;  /* Limit lines per file */

                /* Remove newlines */
                newline = strchr(search_line_buf, '\n');
                if (newline) *newline = '\0';
                newline = strchr(search_line_buf, '\r');
                if (newline) *newline = '\0';

                if (strstr(search_line_buf, search_pattern)) {
                    sprintf(search_match_buf, "%s:%d:%s",
                            full_path, line_num, search_line_buf);
                    json_escape_string(search_match_buf, search_escaped_buf, sizeof(search_escaped_buf));
                    if (*pos + strlen(search_escaped_buf) + 2 < maxlen) {
                        if (*match_count > 0) result[(*pos)++] = ',';
                        *pos += sprintf(result + *pos, "%s", search_escaped_buf);
                        (*match_count)++;
                    }
                }
            }

            fclose(fp);
        }
    }

    closedir(dir);
}

static void handle_fs_search(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char path[MAX_PATH];
    char pattern[512];
    char file_pattern[256];
    int pos;
    int count = 0;
    int file_count = 0;

    json_get_string(params, "path", path, sizeof(path));
    json_get_string(params, "pattern", pattern, sizeof(pattern));
    json_get_string(params, "filePattern", file_pattern, sizeof(file_pattern));

    if (!path[0]) strcpy(path, ".");
    if (!pattern[0]) {
        sprintf(response, "{\"error\":\"pattern is required\"}");
        return;
    }

    pos = sprintf(response, "{\"result\":[");
    search_recursive(path, pattern, file_pattern, response, &pos, resplen - 10, &count, 200, &file_count, 500, 0);
    sprintf(response + pos, "]}");
}

static void handle_cp_exec(params, response, resplen)
char *params;
char *response;
int resplen;
{
    char command[MAX_PATH * 2];
    char wrapped_cmd[MAX_PATH * 2 + 32];
    char *output;
    char *escaped_output;
    FILE *fp;
    int output_len = 0;
    int status;
    int max_output;
    int n;

    if (logfile) { fprintf(logfile, "[cp.exec] parsing command from: %.100s\n", params); fflush(logfile); }

    json_get_string(params, "command", command, sizeof(command));

    if (logfile) { fprintf(logfile, "[cp.exec] command: %s\n", command); fflush(logfile); }

    output = malloc(BUFFER_SIZE);
    if (!output) {
        sprintf(response, "{\"error\":\"Out of memory\"}");
        return;
    }

    /* Wrap command to capture stderr (including shell errors like "command not found")
     * The { cmd; } 2>&1 construct captures all stderr while preserving any
     * redirections the command itself has (they apply inside the group) */
    sprintf(wrapped_cmd, "{ %s; } 2>&1", command);

    if (logfile) { fprintf(logfile, "[cp.exec] calling popen with: %s\n", wrapped_cmd); fflush(logfile); }

    fp = popen(wrapped_cmd, "r");
    if (!fp) {
        if (logfile) { fprintf(logfile, "[cp.exec] popen failed: %s\n", strerror(errno)); fflush(logfile); }
        free(output);
        sprintf(response, "{\"error\":\"%s\",\"status\":-1,\"stdout\":\"\",\"stderr\":\"\"}", strerror(errno));
        return;
    }

    if (logfile) { fprintf(logfile, "[cp.exec] popen succeeded, reading output...\n"); fflush(logfile); }

    /* Cap output at 4MB so escaped (up to 8MB) + JSON wrapper fits in 10MB response */
    max_output = 4 * 1024 * 1024;
    while (output_len < max_output) {
        n = fread(output + output_len, 1, max_output - output_len, fp);
        if (n == 0) break;
        output_len += n;
    }
    output[output_len] = '\0';

    if (logfile) { fprintf(logfile, "[cp.exec] read %d bytes, calling pclose...\n", output_len); fflush(logfile); }

    status = pclose(fp);

    if (logfile) { fprintf(logfile, "[cp.exec] pclose returned %d\n", status); fflush(logfile); }

    /* Extract exit status portably */
#ifdef NEXT_COMPAT
    if ((status & 0xff) == 0) {
        status = (status >> 8) & 0xff;
    }
#else
    if (WIFEXITED(status)) {
        status = WEXITSTATUS(status);
    }
#endif

    if (logfile) { fprintf(logfile, "[cp.exec] exit status: %d, output_len: %d\n", status, output_len); fflush(logfile); }

    escaped_output = malloc(output_len * 2 + 3);
    if (!escaped_output) {
        free(output);
        sprintf(response, "{\"error\":\"Out of memory\"}");
        return;
    }

    json_escape_string(output, escaped_output, output_len * 2 + 3);

    if (logfile) { fprintf(logfile, "[cp.exec] building response...\n"); fflush(logfile); }

    sprintf(response, "{\"result\":{\"status\":%d,\"stdout\":%s,\"stderr\":\"\"}}",
             status, escaped_output);

    if (logfile) { fprintf(logfile, "[cp.exec] done, response: %.100s...\n", response); fflush(logfile); }

    free(output);
    free(escaped_output);
}

static void handle_request(json, response, resplen)
char *json;
char *response;
int resplen;
{
    char op[64];
    char *params_buf;
    char *params;
    long req_id;
    int params_len;

    if (logfile) { fprintf(logfile, "[handle_request] parsing...\n"); fflush(logfile); }

    req_id = json_get_int(json, "id");
    json_get_string(json, "op", op, sizeof(op));

    if (logfile) { fprintf(logfile, "[handle_request] id=%ld op=%s\n", req_id, op); fflush(logfile); }

    /* Extract params object - find "params":{...} */
    params = strstr(json, "\"params\":");
    if (params) {
        params += 9;
        while (*params == ' ') params++;
        params_len = strlen(params);
        params_buf = malloc(params_len + 1);
        if (!params_buf) {
            sprintf(response, "{\"type\":\"response\",\"id\":%ld,\"error\":\"Out of memory\"}", req_id);
            return;
        }
        strncpy(params_buf, params, params_len);
        params_buf[params_len] = '\0';
    } else {
        params_buf = malloc(3);
        if (!params_buf) {
            sprintf(response, "{\"type\":\"response\",\"id\":%ld,\"error\":\"Out of memory\"}", req_id);
            return;
        }
        strcpy(params_buf, "{}");
    }

    if (logfile) { fprintf(logfile, "[handle_request] params: %.100s\n", params_buf); fflush(logfile); }

    /* Route to handler */
    if (strcmp(op, "fs.readFile") == 0) {
        handle_fs_readFile(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.writeFile") == 0) {
        handle_fs_writeFile(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.stat") == 0) {
        handle_fs_stat(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.lstat") == 0) {
        handle_fs_lstat(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.readdir") == 0) {
        handle_fs_readdir(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.exists") == 0) {
        handle_fs_exists(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.mkdir") == 0) {
        handle_fs_mkdir(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.unlink") == 0) {
        handle_fs_unlink(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.rename") == 0) {
        handle_fs_rename(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.access") == 0) {
        handle_fs_access(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.realpath") == 0) {
        handle_fs_realpath(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.find") == 0) {
        handle_fs_find(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "fs.search") == 0) {
        handle_fs_search(params_buf, result_buffer, BUFFER_SIZE);
    } else if (strcmp(op, "cp.exec") == 0 || strcmp(op, "cp.spawn") == 0) {
        if (logfile) { fprintf(logfile, "[handle_request] calling handle_cp_exec\n"); fflush(logfile); }
        handle_cp_exec(params_buf, result_buffer, BUFFER_SIZE);
        if (logfile) { fprintf(logfile, "[handle_request] handle_cp_exec returned\n"); fflush(logfile); }
    } else {
        sprintf(result_buffer, "{\"error\":\"Unknown operation: %s\"}", op);
    }

    /* Wrap response with ID and type */
    sprintf(response, "{\"type\":\"response\",\"id\":%ld,%s",
             req_id, result_buffer + 1);  /* Skip opening { of result_buffer */

    free(params_buf);
}

/* ============================================================================
 * Main loop
 * ============================================================================ */

static void main_loop()
{
    fd_set readfds;
    struct timeval tv;
    int maxfd, rows, cols, n, len;
    char input_buf[256];
    char escaped[512];
    char type[32];
    char *data;
    char *filtered;
    int datalen, flen, x;
    int ii;
    unsigned char c;

    enable_raw_mode();

    /* Set up SIGWINCH handler */
    signal(SIGWINCH, sigwinch_handler);

    /* Send initial terminal size */
    get_terminal_size(&rows, &cols);
    sprintf(send_buffer, "{\"type\":\"resize\",\"rows\":%d,\"cols\":%d}", rows, cols);
    send_message(send_buffer);

    maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

    while (1) {
        /* Handle window resize */
        if (got_sigwinch) {
            got_sigwinch = 0;
            get_terminal_size(&rows, &cols);
            sprintf(send_buffer, "{\"type\":\"resize\",\"rows\":%d,\"cols\":%d}", rows, cols);
            send_message(send_buffer);
        }

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        tv.tv_sec = 0;
        tv.tv_usec = 100000;  /* 100ms */

        if (select(maxfd + 1, &readfds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Handle terminal input */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            n = read(STDIN_FILENO, input_buf, sizeof(input_buf) - 1);
            if (n > 0) {
                input_buf[n] = '\0';

                /* Debug: log input bytes in hex */
                if (logfile) {
                    fprintf(logfile, "[INPUT] %d bytes: ", n);
                    for (ii = 0; ii < n; ii++) {
                        fprintf(logfile, "%02x ", (unsigned char)input_buf[ii]);
                    }
                    fprintf(logfile, " = \"");
                    for (ii = 0; ii < n; ii++) {
                        c = (unsigned char)input_buf[ii];
                        if (c >= 32 && c < 127) {
                            fprintf(logfile, "%c", c);
                        } else if (c == 27) {
                            fprintf(logfile, "<ESC>");
                        } else {
                            fprintf(logfile, "<%02x>", c);
                        }
                    }
                    fprintf(logfile, "\"\n");
                    fflush(logfile);
                }

                json_escape_string(input_buf, escaped, sizeof(escaped));
                sprintf(send_buffer, "{\"type\":\"terminal_input\",\"data\":%s}", escaped);
                send_message(send_buffer);
            }
        }

        /* Handle messages from relay */
        if (FD_ISSET(sockfd, &readfds)) {
            len = recv_message(recv_buffer, BUFFER_SIZE);
            if (len <= 0) {
                fprintf(stderr, "\r\nConnection closed\r\n");
                break;
            }

            json_get_string(recv_buffer, "type", type, sizeof(type));

            if (strcmp(type, "terminal_output") == 0) {
                data = malloc(len + 1);
                if (data) {
                    json_get_string(recv_buffer, "data", data, len + 1);
                    datalen = strlen(data);

                    if (logfile) {
                        fprintf(logfile, "=== RECV len=%d datalen=%d simple=%d ===\n",
                                len, datalen, simple_mode);
                        /* Log hex dump of first 200 bytes */
                        fprintf(logfile, "HEX: ");
                        for (x = 0; x < datalen && x < 200; x++) {
                            fprintf(logfile, "%02x ", (unsigned char)data[x]);
                        }
                        fprintf(logfile, "\n");
                        fflush(logfile);
                    }

                    if (simple_mode) {
                        filtered = malloc(len + 1);
                        if (filtered) {
                            flen = filter_terminal_output(data, filtered, len + 1);

                            if (logfile) {
                                fprintf(logfile, "FILTERED len=%d\n", flen);
                                fprintf(logfile, "HEX: ");
                                for (x = 0; x < flen && x < 200; x++) {
                                    fprintf(logfile, "%02x ", (unsigned char)filtered[x]);
                                }
                                fprintf(logfile, "\n---\n");
                                fflush(logfile);
                            }

                            write(STDOUT_FILENO, filtered, flen);
                            free(filtered);
                        }
                    } else {
                        write(STDOUT_FILENO, data, datalen);
                    }
                    free(data);
                }
            } else if (strcmp(type, "request") == 0) {
                if (logfile) {
                    fprintf(logfile, "=== REQUEST received ===\n");
                    fprintf(logfile, "RAW: %.200s...\n", recv_buffer);
                    fflush(logfile);
                }
                handle_request(recv_buffer, send_buffer, BUFFER_SIZE);
                if (logfile) {
                    fprintf(logfile, "=== RESPONSE ready ===\n");
                    fprintf(logfile, "RAW: %.200s...\n", send_buffer);
                    fflush(logfile);
                }
                send_message(send_buffer);
                if (logfile) {
                    fprintf(logfile, "=== RESPONSE sent ===\n");
                    fflush(logfile);
                }
            } else if (logfile && type[0] != '\0') {
                fprintf(logfile, "=== UNKNOWN type: '%s' ===\n", type);
                fprintf(logfile, "RAW: %.200s...\n", recv_buffer);
                fflush(logfile);
            }
        }
    }

    disable_raw_mode();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(argc, argv)
int argc;
char *argv[];
{
    char *host;
    int port;
    int i;
    char cwd[MAX_PATH];
    char escaped_cwd[MAX_PATH * 2];
    char *p, *q;

    /* Parse options */
    host = NULL;
    port = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--simple") == 0) {
            simple_mode = 1;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--resume") == 0) {
            resume_mode = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            logfile = fopen("/tmp/telepresence.log", "w");
            if (logfile) {
                fprintf(stderr, "*** Logging enabled: /tmp/telepresence.log ***\n");
                fprintf(logfile, "=== Log started ===\n");
                fflush(logfile);
            } else {
                fprintf(stderr, "*** Failed to open log file! ***\n");
            }
        }
    }

    /* Find host and port (non-flag arguments) */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] != '-') {
            if (!host) {
                host = argv[i];
            } else if (!port) {
                port = atoi(argv[i]);
            }
        }
    }

    if (!host || !port) {
        fprintf(stderr, "Usage: %s [-s] [-r] [-l] <host> <port>\n", argv[0]);
        fprintf(stderr, "Connect to claude-telepresence relay server\n");
        fprintf(stderr, "\nOptions:\n");
        fprintf(stderr, "  -s, --simple   Simple mode: convert Unicode to ASCII\n");
        fprintf(stderr, "  -r, --resume   Resume previous conversation\n");
        fprintf(stderr, "  -l, --log      Log to /tmp/telepresence.log\n");
        return 1;
    }

    /* Allocate buffers */
    recv_buffer = malloc(BUFFER_SIZE);
    send_buffer = malloc(BUFFER_SIZE);
    json_buffer = malloc(BUFFER_SIZE);
    result_buffer = malloc(BUFFER_SIZE);

    if (!recv_buffer || !send_buffer || !json_buffer || !result_buffer) {
        fprintf(stderr, "Failed to allocate buffers\n");
        return 1;
    }

    fprintf(stderr, "Connecting to %s:%d (simple=%d, log=%s)...\n",
            host, port, simple_mode, logfile ? "yes" : "no");

    if (connect_to_relay(host, port) < 0) {
        return 1;
    }

    /* Send hello with cwd so relay knows our working directory */
#ifdef NEXT_COMPAT
    /* NeXT uses getwd() instead of getcwd() */
    if (getwd(cwd) == NULL) {
        strcpy(cwd, "/");
    }
#else
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        strcpy(cwd, "/");
    }
#endif

    /* Escape cwd for JSON */
    p = cwd;
    q = escaped_cwd;
    while (*p && q < escaped_cwd + sizeof(escaped_cwd) - 2) {
        if (*p == '"' || *p == '\\') {
            *q++ = '\\';
        }
        *q++ = *p++;
    }
    *q = '\0';

    if (resume_mode) {
        sprintf(send_buffer,
                 "{\"type\":\"hello\",\"cwd\":\"%s\",\"resume\":true}", escaped_cwd);
    } else {
        sprintf(send_buffer,
                 "{\"type\":\"hello\",\"cwd\":\"%s\"}", escaped_cwd);
    }
    send_message(send_buffer);

    fprintf(stderr, "Connected! Starting Claude Code session...\n\n");

    main_loop();

    if (sockfd >= 0) close(sockfd);
    free(recv_buffer);
    free(send_buffer);
    free(json_buffer);
    free(result_buffer);

    return 0;
}
