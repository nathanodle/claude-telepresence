/*
 * claude-telepresence client for Windows (98/98SE/ME/2000/XP)
 *
 * Windows port of the v2 binary protocol client (see PROTOCOL.md).
 * Combines the stream/flow-control/protocol logic of client.c with
 * Win32 console, file, and process APIs.
 *
 * TARGET: Windows 98 (Dell OptiPlex GX1) is a real test target, not just
 * XP/2000. Only Win95 OSR2/98-era Win32 APIs are used:
 *   - ANSI ("A" suffixed) Win32 calls only, never the "W" (wide/Unicode)
 *     variants -- Win98 is ANSI/MBCS, not real UTF-16 like NT.
 *   - No NTFS-only calls, no services APIs, no overlapped I/O / IOCP.
 *   - Winsock2 (winsock2.h) only, no getaddrinfo (not in Win98's ws2_32).
 *
 * IMPORTANT (linking, not handled by this source file):
 * The resulting .exe must be linked with the OLD subsystem version that
 * the Windows 9x PE loader expects, or Win98 will refuse to run it:
 *   -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0
 *   -Wl,--major-os-version,4        -Wl,--minor-os-version,0
 * See README_WINDOWS.md for the full build command.
 *
 * Build (MinGW cross-compile from Linux):
 *   i686-w64-mingw32-gcc -o claude-telepresence.exe client_winxp.c -lws2_32 \
 *     -Wl,--major-subsystem-version,4 -Wl,--minor-subsystem-version,0 \
 *     -Wl,--major-os-version,4 -Wl,--minor-os-version,0
 *
 * Build (native MinGW on Windows):
 *   gcc -o claude-telepresence.exe client_winxp.c -lws2_32
 *
 * Build (Visual C++ 6.0 / VS2003+):
 *   cl /nologo client_winxp.c ws2_32.lib
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>

#include <windows.h>
#include <winsock2.h>
#include <io.h>
#include <direct.h>

#pragma comment(lib, "ws2_32.lib")

/* ============================================================================
 * Protocol Constants (from PROTOCOL.md) - mirrors client.c
 * ============================================================================ */

#define PROTO_VERSION       2

/* Packet types - Control */
#define PKT_HELLO           0x00
#define PKT_HELLO_ACK       0x01
#define PKT_PING            0x0E
#define PKT_PONG            0x0F
#define PKT_GOODBYE         0x0D

/* Packet types - Terminal */
#define PKT_TERM_INPUT      0x10
#define PKT_TERM_OUTPUT     0x11
#define PKT_TERM_RESIZE     0x12

/* Packet types - Streams */
#define PKT_STREAM_OPEN     0x20
#define PKT_STREAM_DATA     0x21
#define PKT_STREAM_END      0x22
#define PKT_STREAM_ERROR    0x23
#define PKT_STREAM_CANCEL   0x24

/* Packet types - Flow control */
#define PKT_WINDOW_UPDATE   0x28

/* Stream types */
#define STREAM_FILE_READ    0x01
#define STREAM_FILE_WRITE   0x02
#define STREAM_EXEC         0x03
#define STREAM_DIR_LIST     0x04
#define STREAM_FILE_STAT    0x05
#define STREAM_FILE_FIND    0x06
#define STREAM_FILE_SEARCH  0x07
#define STREAM_MKDIR        0x08
#define STREAM_REMOVE       0x09
#define STREAM_MOVE         0x0A
#define STREAM_FILE_EXISTS  0x0B
#define STREAM_REALPATH     0x0C

/* EXEC channels */
#define CHAN_STDOUT         0x01
#define CHAN_STDERR         0x02

/* Stream end status */
#define STATUS_OK           0x00
#define STATUS_ERROR        0x01
#define STATUS_CANCELLED    0x02

/* EXEC exit status */
#define EXIT_NORMAL         0x00
#define EXIT_SIGNAL         0x01
#define EXIT_TIMEOUT        0x02
#define EXIT_UNKNOWN        0xFF

/* Error codes */
#define ERR_NOT_FOUND       0x01
#define ERR_PERMISSION      0x02
#define ERR_IO_ERROR        0x03
#define ERR_TIMEOUT         0x04
#define ERR_CANCELLED       0x05
#define ERR_NO_MEMORY       0x06
#define ERR_INVALID         0x07
#define ERR_EXISTS          0x08
#define ERR_NOT_DIR         0x09
#define ERR_IS_DIR          0x0A
#define ERR_UNKNOWN         0xFF

/* HELLO flags */
#define FLAG_RESUME         0x01
#define FLAG_SIMPLE         0x02

/* GOODBYE reasons */
#define BYE_NORMAL          0x00
#define BYE_PROTOCOL_ERROR  0x01
#define BYE_TIMEOUT         0x02
#define BYE_RESOURCE        0x03
#define BYE_UNKNOWN         0xFF

/* Limits.
 * NOTE: windows.h already #defines MAX_PATH (260), so the protocol's
 * path-length limit uses a different name here to avoid collision. */
#define MAX_PACKET_SIZE     (1 * 1024 * 1024)   /* 1 MB - safe for legacy systems */
#define PROTO_MAX_PATH      4096
#define MAX_STREAMS         256
#define DEFAULT_WINDOW      (256 * 1024)        /* 256 KB */
#define MIN_WINDOW          (16 * 1024)         /* 16 KB */
#define CHUNK_SIZE          (64 * 1024)         /* 64 KB for file I/O */
#define SMALL_CHUNK         4096                /* 4 KB for exec output */

/* ============================================================================
 * 64-bit helper type (portable across MinGW gcc and MSVC/VC6)
 * ============================================================================ */

#ifdef _MSC_VER
typedef unsigned __int64 win_u64;
#else
typedef unsigned long long win_u64;
#endif

/* ============================================================================
 * Data Structures
 * ============================================================================ */

#define STREAM_STATE_IDLE       0
#define STREAM_STATE_OPEN       1
#define STREAM_STATE_HALF_LOCAL 2
#define STREAM_STATE_HALF_REMOTE 3
#define STREAM_STATE_CLOSED     4

struct stream {
    unsigned long id;
    int state;
    int type;
    FILE *file_fp;        /* For file read/write */
    HANDLE child_process; /* For EXEC streams: the child's process HANDLE */
    char exec_tmpfile[MAX_PATH]; /* For EXEC streams: output capture file */
    int exec_sent;               /* For EXEC streams: has output been sent yet */
};

/* Directory walking helper - wraps FindFirstFileA/FindNextFileA to behave
 * like POSIX opendir/readdir (Win32's FindFirstFile already returns the
 * first entry, so we track whether it's still "pending" i.e. unconsumed). */
typedef struct {
    HANDLE h;
    WIN32_FIND_DATAA fd;
    int pending;
} dir_frame;

/* Global state */
static SOCKET sockfd = INVALID_SOCKET;
static int simple_mode = 0;
static int resume_mode = 0;
static int raw_mode = 0;
static FILE *logfile = NULL;

/* Terminal state */
static HANDLE hStdin = INVALID_HANDLE_VALUE;
static HANDLE hStdout = INVALID_HANDLE_VALUE;
static DWORD orig_console_mode = 0;

/* Flow control */
static unsigned long send_window = DEFAULT_WINDOW;
static unsigned long recv_window = DEFAULT_WINDOW;
static unsigned long bytes_in_flight = 0;
static unsigned long bytes_to_ack = 0;
#define WINDOW_UPDATE_THRESHOLD 8192

/* Receive buffer for packet reassembly */
static unsigned char *recv_buf = NULL;
static int recv_buf_len = 0;
static int recv_buf_cap = 0;

/* Stream table */
static struct stream streams[MAX_STREAMS];

/* Working directory from HELLO */
static char remote_cwd[PROTO_MAX_PATH];

/* ============================================================================
 * Simple Mode Filter
 *
 * Strips ALL escape sequences (CSI cursor/color codes, OSC title-setting,
 * and other single-char ESC sequences), converts UTF-8 to ASCII.
 *
 * NOTE: this deliberately diverges from client.c's Unix filter, which only
 * strips SGR (color, ESC[...m) and re-emits other escapes raw -- that's
 * correct on a real terminal that interprets VT100 codes, but Win98's
 * console (WriteConsoleA, no ANSI.SYS for native Win32 console apps) does
 * not interpret escape sequences at all. Anything not stripped here prints
 * as literal garbage characters, so everything gets discarded instead.
 * ============================================================================ */

#define FLT_NORMAL   0
#define FLT_ESC      1
#define FLT_CSI      2
#define FLT_UTF8     3
#define FLT_OSC      4
#define FLT_OSC_ESC  5

static struct {
    int state;
    unsigned char seq[32];
    int seq_len;
    int utf8_need;
    int spinner;
} flt = { FLT_NORMAL, {0}, 0, 0, 0 };

static char spinner_chars[4] = { '-', '\\', '|', '/' };

static int utf8_to_ascii(unsigned char *seq, int len)
{
    unsigned char b0, b1, b2;

    if (len < 2) return '?';

    b0 = seq[0];
    b1 = seq[1];

    if (len == 2) {
        if (b0 == 0xC2) {
            if (b1 == 0xA0) return ' ';
            if (b1 == 0xB7) {
                return spinner_chars[flt.spinner++ & 3];
            }
        }
        return '?';
    }

    if (len == 3 && b0 == 0xE2) {
        b2 = seq[2];

        if (b1 == 0x94) {
            if (b2 == 0x82 || b2 == 0x83) return '|';
            if (b2 == 0x80 || b2 == 0x81 || b2 == 0x84) return '-';
            return '+';
        }
        if (b1 == 0x95) {
            if (b2 >= 0x90 && b2 <= 0x94) return '=';
            return '+';
        }

        if (b1 == 0x86) {
            if (b2 == 0x90) return '<';
            if (b2 == 0x91) return '^';
            if (b2 == 0x92) return '>';
            if (b2 == 0x93) return 'v';
            return '>';
        }

        if (b1 == 0x96) {
            if (b2 >= 0xB2 && b2 <= 0xB5) return '^';
            if (b2 >= 0xB6 && b2 <= 0xB9) return '>';
            if (b2 >= 0xBA && b2 <= 0xBD) return 'v';
            return '*';
        }

        if (b1 == 0x97) {
            if (b2 >= 0x80 && b2 <= 0x83) return '<';
            if (b2 == 0x8F) {
                return spinner_chars[flt.spinner++ & 3];
            }
            if (b2 == 0x8B) return 'o';
            if (b2 == 0x86 || b2 == 0x87) return '*';
            return '*';
        }

        if (b1 == 0x9C) {
            if (b2 == 0x93 || b2 == 0x94) return '+';
            if (b2 == 0x85) return '+';
            if (b2 == 0x97 || b2 == 0x98) return 'x';
            if (b2 == 0xA2 || b2 == 0xB3 || b2 == 0xB6 ||
                b2 == 0xBB || b2 == 0xBD) {
                return spinner_chars[flt.spinner++ & 3];
            }
            return '*';
        }

        if (b1 == 0x9D) {
            if (b2 == 0x8C) return 'x';
            return '*';
        }

        if (b1 == 0x9E) {
            return '>';
        }

        if (b1 == 0x88) {
            if (b2 == 0xB4) {
                return spinner_chars[flt.spinner++ & 3];
            }
            return '*';
        }

        if (b1 >= 0x8C && b1 <= 0x8F) {
            return '>';
        }

        if (b1 == 0x80) {
            if (b2 == 0xA2) return '*';
            if (b2 == 0xA3) return '>';
            if (b2 >= 0x93 && b2 <= 0x95) return '-';
            if (b2 == 0x98 || b2 == 0x99) return '\'';
            if (b2 == 0x9C || b2 == 0x9D) return '"';
            if (b2 == 0xA6) return '.';
            if (b2 == 0xB9) return '<';
            if (b2 == 0xBA) return '>';
            return ' ';
        }

        return '?';
    }

    if (len == 4 && b0 == 0xF0) {
        if (b1 == 0x9F) return '*';
        return '?';
    }

    return '?';
}

static int filter_simple(unsigned char *buf, int len)
{
    unsigned char *r, *w, *end;
    unsigned char c;

    if (len <= 0) return 0;

    r = buf;
    w = buf;
    end = buf + len;

    while (r < end) {
        c = *r++;

        switch (flt.state) {

        case FLT_NORMAL:
            if (c == 0x1B) {
                flt.state = FLT_ESC;
                flt.seq[0] = c;
                flt.seq_len = 1;
            } else if (c < 0x80) {
                *w++ = c;
            } else if ((c & 0xE0) == 0xC0) {
                flt.state = FLT_UTF8;
                flt.seq[0] = c;
                flt.seq_len = 1;
                flt.utf8_need = 1;
            } else if ((c & 0xF0) == 0xE0) {
                flt.state = FLT_UTF8;
                flt.seq[0] = c;
                flt.seq_len = 1;
                flt.utf8_need = 2;
            } else if ((c & 0xF8) == 0xF0) {
                flt.state = FLT_UTF8;
                flt.seq[0] = c;
                flt.seq_len = 1;
                flt.utf8_need = 3;
            } else {
                *w++ = '?';
            }
            break;

        case FLT_ESC:
            /* CSI (ESC[...): cursor movement, color, etc. */
            if (c == '[') {
                flt.state = FLT_CSI;
                flt.seq_len = 0;
            /* OSC (ESC]...): window title, etc. */
            } else if (c == ']') {
                flt.state = FLT_OSC;
                flt.seq_len = 0;
            } else {
                /* Single-char escape (ESC=, ESC>, ESC7, ESC8, ESCc, ...):
                 * meaningless on a plain console - discard silently. */
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
            }
            break;

        case FLT_CSI:
            /* Consume until the terminator byte (0x40-0x7E); the whole
             * sequence -- including cursor moves, erase-line, etc, not
             * just SGR color codes -- is discarded, never re-emitted. */
            if (c >= 0x40 && c <= 0x7E) {
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
            } else if (++flt.seq_len >= 64) {
                /* Runaway/malformed sequence - bail out rather than hang. */
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
            }
            break;

        case FLT_OSC:
            /* Consume until BEL (string terminator) or ESC (expect ESC\\). */
            if (c == 0x07) {
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
            } else if (c == 0x1B) {
                flt.state = FLT_OSC_ESC;
            } else if (++flt.seq_len >= 256) {
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
            }
            break;

        case FLT_OSC_ESC:
            /* After ESC inside OSC: '\\' completes the ST terminator,
             * anything else just resumes swallowing the OSC body. */
            flt.state = (c == '\\') ? FLT_NORMAL : FLT_OSC;
            flt.seq_len = 0;
            break;

        case FLT_UTF8:
            if ((c & 0xC0) == 0x80) {
                flt.seq[flt.seq_len++] = c;
                flt.utf8_need--;
                if (flt.utf8_need == 0) {
                    *w++ = (unsigned char)utf8_to_ascii(flt.seq, flt.seq_len);
                    flt.state = FLT_NORMAL;
                    flt.seq_len = 0;
                }
            } else {
                *w++ = '?';
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
                r--;
            }
            break;
        }
    }

    return (int)(w - buf);
}

/* ============================================================================
 * Logging
 * ============================================================================ */

static void log_packet(char *direction, int type, int length)
{
    char *name;
    if (!logfile) return;

    switch (type) {
        case PKT_HELLO:         name = "HELLO"; break;
        case PKT_HELLO_ACK:     name = "HELLO_ACK"; break;
        case PKT_PING:          name = "PING"; break;
        case PKT_PONG:          name = "PONG"; break;
        case PKT_GOODBYE:       name = "GOODBYE"; break;
        case PKT_TERM_INPUT:    name = "TERM_INPUT"; break;
        case PKT_TERM_OUTPUT:   name = "TERM_OUTPUT"; break;
        case PKT_TERM_RESIZE:   name = "TERM_RESIZE"; break;
        case PKT_STREAM_OPEN:   name = "STREAM_OPEN"; break;
        case PKT_STREAM_DATA:   name = "STREAM_DATA"; break;
        case PKT_STREAM_END:    name = "STREAM_END"; break;
        case PKT_STREAM_ERROR:  name = "STREAM_ERROR"; break;
        case PKT_STREAM_CANCEL: name = "STREAM_CANCEL"; break;
        case PKT_WINDOW_UPDATE: name = "WINDOW_UPDATE"; break;
        default:                name = "UNKNOWN"; break;
    }
    fprintf(logfile, "[%s] %s (0x%02X) len=%d\n", direction, name, type, length);
    fflush(logfile);
}

/* ============================================================================
 * Byte Order Helpers
 * ============================================================================ */

static unsigned long get_u32(unsigned char *buf)
{
    return ((unsigned long)buf[0] << 24) |
           ((unsigned long)buf[1] << 16) |
           ((unsigned long)buf[2] << 8) |
           (unsigned long)buf[3];
}

static void put_u32(unsigned char *buf, unsigned long val)
{
    buf[0] = (unsigned char)((val >> 24) & 0xFF);
    buf[1] = (unsigned char)((val >> 16) & 0xFF);
    buf[2] = (unsigned char)((val >> 8) & 0xFF);
    buf[3] = (unsigned char)(val & 0xFF);
}

static unsigned int get_u16(unsigned char *buf)
{
    return ((unsigned int)buf[0] << 8) | (unsigned int)buf[1];
}

static void put_u16(unsigned char *buf, unsigned int val)
{
    buf[0] = (unsigned char)((val >> 8) & 0xFF);
    buf[1] = (unsigned char)(val & 0xFF);
}

/* ============================================================================
 * Packet I/O (Winsock2)
 * ============================================================================ */

#define WSA_WOULDBLOCK() (WSAGetLastError() == WSAEWOULDBLOCK)

/*
 * Send a packet. Returns 0 on success, -1 on error.
 */
static int send_packet(int type, unsigned char *payload, int length)
{
    unsigned char header[5];
    int sent, n;
    fd_set wfds;

    if (length > MAX_PACKET_SIZE) {
        if (logfile) fprintf(logfile, "[ERROR] Packet too large: %d\n", length);
        return -1;
    }

    header[0] = (unsigned char)type;
    put_u32(header + 1, (unsigned long)length);

    sent = 0;
    while (sent < 5) {
        n = send(sockfd, (char *)header + sent, 5 - sent, 0);
        if (n == SOCKET_ERROR) {
            if (WSA_WOULDBLOCK()) {
                FD_ZERO(&wfds);
                FD_SET(sockfd, &wfds);
                select(0, NULL, &wfds, NULL, NULL);
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        sent += n;
    }

    if (length > 0 && payload != NULL) {
        sent = 0;
        while (sent < length) {
            n = send(sockfd, (char *)payload + sent, length - sent, 0);
            if (n == SOCKET_ERROR) {
                if (WSA_WOULDBLOCK()) {
                    FD_ZERO(&wfds);
                    FD_SET(sockfd, &wfds);
                    select(0, NULL, &wfds, NULL, NULL);
                    continue;
                }
                return -1;
            }
            if (n == 0) return -1;
            sent += n;
        }
    }

    log_packet("SEND", type, length);
    return 0;
}

/*
 * Try to read a complete packet from the socket.
 * Returns packet type on success, -1 on error, 0 if incomplete.
 */
static int recv_packet(unsigned char **payload, int *length)
{
    unsigned char tmp[4096];
    int n, pkt_len, total_needed;
    unsigned char type;

    n = recv(sockfd, (char *)tmp, sizeof(tmp), 0);
    if (n == SOCKET_ERROR) {
        if (WSA_WOULDBLOCK()) {
            n = 0;
        } else {
            return -1;
        }
    } else if (n == 0) {
        return -1;  /* Connection closed */
    }

    if (n > 0) {
        if (recv_buf_len + n > recv_buf_cap) {
            int new_cap = recv_buf_cap ? recv_buf_cap * 2 : 8192;
            unsigned char *new_buf;
            while (new_cap < recv_buf_len + n) new_cap *= 2;
            if (new_cap > MAX_PACKET_SIZE + 5) new_cap = MAX_PACKET_SIZE + 5;
            new_buf = realloc(recv_buf, new_cap);
            if (!new_buf) return -1;
            recv_buf = new_buf;
            recv_buf_cap = new_cap;
        }
        memcpy(recv_buf + recv_buf_len, tmp, n);
        recv_buf_len += n;
    }

    if (recv_buf_len < 5) return 0;

    type = recv_buf[0];
    pkt_len = (int)get_u32(recv_buf + 1);

    if (pkt_len > MAX_PACKET_SIZE) {
        if (logfile) fprintf(logfile, "[ERROR] Received packet too large: %d\n", pkt_len);
        return -1;
    }

    total_needed = 5 + pkt_len;
    if (recv_buf_len < total_needed) return 0;

    *payload = recv_buf + 5;
    *length = pkt_len;

    log_packet("RECV", type, pkt_len);
    return type;
}

/*
 * Consume a packet from the receive buffer after processing.
 */
static void consume_packet(int length)
{
    int total = 5 + length;
    if (recv_buf_len > total) {
        memmove(recv_buf, recv_buf + total, recv_buf_len - total);
    }
    recv_buf_len -= total;
}

/*
 * Wait for send window to have space for 'needed' bytes.
 * Polls socket for WINDOW_UPDATE packets.
 */
static int wait_for_send_window(unsigned long needed)
{
    fd_set readfds;
    struct timeval tv;
    unsigned char *payload;
    int length, type;

    while (bytes_in_flight + needed > send_window) {
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 30;
        tv.tv_usec = 0;

        if (select(0, &readfds, NULL, NULL, &tv) <= 0) {
            return -1;
        }

        type = recv_packet(&payload, &length);
        if (type < 0) return -1;
        if (type == 0) continue;

        if (type == PKT_WINDOW_UPDATE && length >= 4) {
            unsigned long increment = get_u32(payload);
            if (bytes_in_flight >= increment) {
                bytes_in_flight -= increment;
            } else {
                bytes_in_flight = 0;
            }
            if (logfile) {
                fprintf(logfile, "[FLOW] Window update +%lu, in_flight=%lu\n",
                        increment, bytes_in_flight);
            }
        } else if (type == PKT_PING) {
            send_packet(PKT_PONG, payload, length);
        } else if (type == PKT_GOODBYE) {
            return -1;
        }

        consume_packet(length);
    }

    return 0;
}

/*
 * Send stream data with flow control.
 */
static int send_stream_data_fc(unsigned char *buf, int len)
{
    if (wait_for_send_window((unsigned long)len) < 0) {
        return -1;
    }

    if (send_packet(PKT_STREAM_DATA, buf, len) < 0) {
        return -1;
    }

    bytes_in_flight += len;

    return 0;
}

/* ============================================================================
 * Terminal Handling (Win32 console)
 * ============================================================================ */

static void disable_raw_mode(void)
{
    if (raw_mode && hStdin != INVALID_HANDLE_VALUE) {
        SetConsoleMode(hStdin, orig_console_mode);
        raw_mode = 0;
    }
}

static void enable_raw_mode(void)
{
    DWORD mode;

    hStdin = GetStdHandle(STD_INPUT_HANDLE);
    hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hStdin == INVALID_HANDLE_VALUE) return;
    if (!GetConsoleMode(hStdin, &orig_console_mode)) return;

    /* Disable line input, echo, and processed input so we get raw key
     * events one at a time instead of a line-buffered, echoed stream. */
    mode = orig_console_mode;
    mode &= ~((DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT));
    mode |= ENABLE_WINDOW_INPUT;

    if (!SetConsoleMode(hStdin, mode)) return;

    /* Try to enable ANSI/VT processing on output (Windows 10+ only).
     * This is a harmless no-op on 9x/2000/XP - ignore failure. */
    {
        DWORD out_mode;
        if (GetConsoleMode(hStdout, &out_mode)) {
            out_mode |= 0x0004;  /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
            SetConsoleMode(hStdout, out_mode);
        }
    }

    raw_mode = 1;
    atexit(disable_raw_mode);
}

static void get_terminal_size(int *rows, int *cols)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    if (hStdout != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(hStdout, &csbi)) {
        *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return;
    }
    *rows = 24;
    *cols = 80;
}

/* ============================================================================
 * Connection Setup
 * ============================================================================ */

static int connect_to_relay(char *host, int port)
{
    struct hostent *server;
    struct sockaddr_in serv_addr;
    unsigned long addr;
    WSADATA wsaData;
    u_long non_blocking;

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return -1;
    }

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sockfd == INVALID_SOCKET) {
        fprintf(stderr, "socket: %d\n", WSAGetLastError());
        WSACleanup();
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons((u_short)port);

    addr = inet_addr(host);
    if (addr != INADDR_NONE) {
        serv_addr.sin_addr.s_addr = addr;
    } else {
        server = gethostbyname(host);
        if (!server) {
            fprintf(stderr, "Cannot resolve host: %s\n", host);
            closesocket(sockfd);
            WSACleanup();
            return -1;
        }
        memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    }

    if (connect(sockfd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == SOCKET_ERROR) {
        fprintf(stderr, "connect: %d\n", WSAGetLastError());
        closesocket(sockfd);
        WSACleanup();
        return -1;
    }

    /* Disable Nagle's algorithm for low-latency interactive use */
    {
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));
    }

    /* Non-blocking mode */
    non_blocking = 1;
    ioctlsocket(sockfd, FIONBIO, &non_blocking);

    return 0;
}

static int send_hello(void)
{
    unsigned char buf[PROTO_MAX_PATH + 10];
    int flags = 0;
    int len;

    if (resume_mode) flags |= FLAG_RESUME;
    if (simple_mode) flags |= FLAG_SIMPLE;

    if (_getcwd(remote_cwd, sizeof(remote_cwd)) == NULL) {
        strcpy(remote_cwd, "C:\\");
    }

    buf[0] = (unsigned char)PROTO_VERSION;
    buf[1] = (unsigned char)flags;
    put_u32(buf + 2, (unsigned long)recv_window);
    strcpy((char *)buf + 6, remote_cwd);
    len = 6 + (int)strlen(remote_cwd) + 1;

    return send_packet(PKT_HELLO, buf, len);
}

static int wait_for_hello_ack(void)
{
    fd_set fds;
    struct timeval tv;
    unsigned char *payload;
    int length, type;
    int version, flags;
    unsigned long window;
    int timeout_secs = 10;

    while (timeout_secs > 0) {
        FD_ZERO(&fds);
        FD_SET(sockfd, &fds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        if (select(0, &fds, NULL, NULL, &tv) < 0) {
            return -1;
        }

        if (!FD_ISSET(sockfd, &fds)) {
            timeout_secs--;
            continue;
        }

        type = recv_packet(&payload, &length);
        if (type < 0) return -1;
        if (type == 0) continue;

        if (type == PKT_HELLO_ACK) {
            if (length < 6) {
                fprintf(stderr, "Invalid HELLO_ACK\n");
                return -1;
            }
            version = payload[0];
            flags = payload[1];
            window = get_u32(payload + 2);

            if (version != PROTO_VERSION) {
                fprintf(stderr, "Version mismatch: got %d, expected %d\n",
                        version, PROTO_VERSION);
                return -1;
            }

            send_window = window;
            if (logfile) {
                fprintf(logfile, "[HELLO_ACK] version=%d flags=0x%02X window=%lu\n",
                        version, flags, window);
            }
            consume_packet(length);
            return 0;
        } else {
            if (logfile) {
                fprintf(logfile, "[ERROR] Expected HELLO_ACK, got 0x%02X\n", type);
            }
            consume_packet(length);
        }
    }

    fprintf(stderr, "Timeout waiting for HELLO_ACK\n");
    return -1;
}

/* ============================================================================
 * Stream Management
 * ============================================================================ */

static struct stream *find_stream(unsigned long id)
{
    int i;
    for (i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].state != STREAM_STATE_IDLE && streams[i].id == id) {
            return &streams[i];
        }
    }
    return NULL;
}

static struct stream *alloc_stream(unsigned long id)
{
    int i;
    for (i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].state == STREAM_STATE_IDLE) {
            memset(&streams[i], 0, sizeof(struct stream));
            streams[i].id = id;
            streams[i].state = STREAM_STATE_OPEN;
            return &streams[i];
        }
    }
    return NULL;
}

static void free_stream(struct stream *s)
{
    if (s->file_fp) {
        fclose(s->file_fp);
        s->file_fp = NULL;
    }
    if (s->child_process) {
        /* Real process HANDLE (via CreateProcess, not _popen) - can
         * actually be forced to terminate on cancel, unlike _pclose()
         * which has no portable way to kill a _popen()'d child and
         * would just block until it exits on its own. */
        TerminateProcess(s->child_process, 1);
        CloseHandle(s->child_process);
        s->child_process = NULL;
    }
    if (s->exec_tmpfile[0]) {
        DeleteFileA(s->exec_tmpfile);
        s->exec_tmpfile[0] = '\0';
    }
    s->state = STREAM_STATE_IDLE;
}

/* ============================================================================
 * Stream Error Helper
 * ============================================================================ */

static int send_stream_error(unsigned long stream_id, int code, char *message)
{
    unsigned char buf[256];
    int len;

    put_u32(buf, stream_id);
    buf[4] = (unsigned char)code;
    strncpy((char *)buf + 5, message, sizeof(buf) - 6);
    buf[sizeof(buf) - 1] = '\0';
    len = 5 + (int)strlen((char *)buf + 5) + 1;

    return send_packet(PKT_STREAM_ERROR, buf, len);
}

static int send_stream_end(unsigned long stream_id, int status)
{
    unsigned char buf[5];

    put_u32(buf, stream_id);
    buf[4] = (unsigned char)status;

    return send_packet(PKT_STREAM_END, buf, 5);
}

/* ============================================================================
 * Directory walking helper (Win32 FindFirstFile/FindNextFile wrapped to
 * behave like POSIX opendir/readdir)
 * ============================================================================ */

static int dir_open(dir_frame *f, char *dirpath)
{
    char search[PROTO_MAX_PATH + 4];
    size_t len;

    len = strlen(dirpath);
    if (len + 3 >= sizeof(search)) return 0;

    if (len > 0 && (dirpath[len - 1] == '\\' || dirpath[len - 1] == '/')) {
        sprintf(search, "%s*", dirpath);
    } else {
        sprintf(search, "%s\\*", dirpath);
    }

    f->h = FindFirstFileA(search, &f->fd);
    if (f->h == INVALID_HANDLE_VALUE) return 0;
    f->pending = 1;
    return 1;
}

static int dir_read(dir_frame *f)
{
    if (f->pending) {
        f->pending = 0;
        return 1;
    }
    return FindNextFileA(f->h, &f->fd) ? 1 : 0;
}

static void dir_close(dir_frame *f)
{
    if (f->h != INVALID_HANDLE_VALUE) {
        FindClose(f->h);
        f->h = INVALID_HANDLE_VALUE;
    }
}

/* ============================================================================
 * File stat helper
 * ============================================================================ */

/* FILETIME is 100ns ticks since 1601-01-01; Unix epoch is 1970-01-01. */
#define FILETIME_UNIX_EPOCH_DIFF 11644473600UL

static unsigned long filetime_to_unix(FILETIME *ft)
{
    win_u64 t;

    t = ((win_u64)ft->dwHighDateTime << 32) | (win_u64)ft->dwLowDateTime;
    t /= 10000000UL;
    if (t > (win_u64)FILETIME_UNIX_EPOCH_DIFF) {
        t -= (win_u64)FILETIME_UNIX_EPOCH_DIFF;
    } else {
        t = 0;
    }
    return (unsigned long)t;
}

/*
 * Combined stat() equivalent. Uses FindFirstFile for full metadata; falls
 * back to GetFileAttributes for paths FindFirstFile can't enumerate
 * (e.g. drive roots like "C:\"), returning existence/type but no
 * size/mtime for those. Windows has no symlinks on FAT/Win9x, so this
 * also serves as the lstat()-equivalent (no-follow behaves like stat()).
 */
static int win_stat(char *path, int *exists, int *is_dir, win_u64 *size, unsigned long *mtime)
{
    WIN32_FIND_DATAA fd;
    HANDLE h;
    DWORD attrs;

    *exists = 0;
    *is_dir = 0;
    *size = 0;
    *mtime = 0;

    h = FindFirstFileA(path, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        *exists = 1;
        *is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        *size = ((win_u64)fd.nFileSizeHigh << 32) | (win_u64)fd.nFileSizeLow;
        *mtime = filetime_to_unix(&fd.ftLastWriteTime);
        return 1;
    }

    attrs = GetFileAttributesA(path);
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        *exists = 1;
        *is_dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
        return 1;
    }

    return 0;
}

/* ============================================================================
 * File Operations
 * ============================================================================ */

static void handle_file_read(struct stream *s, char *path)
{
    static unsigned char buf[CHUNK_SIZE + 4];
    size_t n;

    s->file_fp = fopen(path, "rb");
    if (!s->file_fp) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    put_u32(buf, s->id);
    while ((n = fread(buf + 4, 1, CHUNK_SIZE, s->file_fp)) > 0) {
        if (send_stream_data_fc(buf, 4 + (int)n) < 0) {
            free_stream(s);
            return;
        }
    }

    fclose(s->file_fp);
    s->file_fp = NULL;
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

static void handle_file_write(struct stream *s, char *path, int mode)
{
    /* Windows has no Unix permission bits to apply here. */
    (void)mode;

    s->file_fp = fopen(path, "wb");
    if (!s->file_fp) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    /* Stream stays open; data arrives via STREAM_DATA, closed on STREAM_END */
}

static void handle_file_write_data(struct stream *s, unsigned char *data, int len)
{
    if (!s->file_fp) {
        send_stream_error(s->id, ERR_INVALID, "No file open");
        return;
    }

    if ((int)fwrite(data, 1, len, s->file_fp) != len) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
        fclose(s->file_fp);
        s->file_fp = NULL;
        free_stream(s);
    }
}

static void handle_file_write_end(struct stream *s)
{
    if (s->file_fp) {
        fclose(s->file_fp);
        s->file_fp = NULL;
    }
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

static void handle_file_stat(struct stream *s, char *path)
{
    unsigned char buf[32];
    int exists, is_dir;
    win_u64 size;
    unsigned long mtime;
    unsigned char type;
    unsigned long mode;

    put_u32(buf, s->id);

    if (!win_stat(path, &exists, &is_dir, &size, &mtime) || !exists) {
        buf[4] = 0;  /* exists = false */
        buf[5] = '?';
        put_u32(buf + 6, 0);
        put_u32(buf + 10, 0); put_u32(buf + 14, 0);
        put_u32(buf + 18, 0); put_u32(buf + 22, 0);
    } else {
        type = is_dir ? 'd' : 'f';
        /* Windows has no Unix mode bits; synthesize a plausible value. */
        mode = is_dir ? 0755UL : 0644UL;

        buf[4] = 1;  /* exists = true */
        buf[5] = type;
        put_u32(buf + 6, mode);
        put_u32(buf + 10, (unsigned long)(size >> 32));
        put_u32(buf + 14, (unsigned long)(size & 0xFFFFFFFFUL));
        put_u32(buf + 18, 0);
        put_u32(buf + 22, mtime);
    }

    if (send_stream_data_fc(buf, 26) < 0) {
        free_stream(s);
        return;
    }
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

static void handle_file_exists(struct stream *s, char *path)
{
    unsigned char buf[6];
    DWORD attrs;

    put_u32(buf, s->id);
    attrs = GetFileAttributesA(path);
    buf[4] = (attrs != INVALID_FILE_ATTRIBUTES) ? 1 : 0;

    if (send_stream_data_fc(buf, 5) < 0) {
        free_stream(s);
        return;
    }
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

static void handle_mkdir(struct stream *s, char *path)
{
    if (_mkdir(path) < 0 && errno != EEXIST) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
    } else {
        send_stream_end(s->id, STATUS_OK);
    }
    free_stream(s);
}

static void handle_remove(struct stream *s, char *path)
{
    if (remove(path) != 0) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
    } else {
        send_stream_end(s->id, STATUS_OK);
    }
    free_stream(s);
}

static void handle_move(struct stream *s, char *oldpath, char *newpath)
{
    if (rename(oldpath, newpath) != 0) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
    } else {
        send_stream_end(s->id, STATUS_OK);
    }
    free_stream(s);
}

static void handle_realpath(struct stream *s, char *path)
{
    unsigned char buf[PROTO_MAX_PATH + 4];
    char resolved[PROTO_MAX_PATH];

    put_u32(buf, s->id);

    if (_fullpath(resolved, path, sizeof(resolved)) == NULL) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }
    strcpy((char *)buf + 4, resolved);
    if (send_stream_data_fc(buf, 4 + (int)strlen(resolved) + 1) < 0) {
        free_stream(s);
        return;
    }
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

/* ============================================================================
 * Glob Pattern Matching
 *
 * Ported directly from client.c - pure portable C, no OS dependency.
 * Supports: * (any chars), ? (single char), [abc], [a-z], [!abc]
 * ============================================================================ */

static int glob_match(char *p, char *s)
{
    char *star_p, *star_s, *q;
    int match, invert;

    star_p = NULL;
    star_s = NULL;

    while (*s) {
        if (*p == '*') {
            while (*p == '*') p++;
            if (!*p) return 1;
            star_p = p;
            star_s = s;
            continue;
        }

        if (*p == '?') {
            p++;
            s++;
            continue;
        }

        if (*p == '[') {
            match = 0;
            invert = 0;
            q = p + 1;

            if (*q == '!' || *q == '^') {
                invert = 1;
                q++;
            }

            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    if ((unsigned char)*s >= (unsigned char)q[0] &&
                        (unsigned char)*s <= (unsigned char)q[2])
                        match = 1;
                    q += 3;
                } else {
                    if (*s == *q) match = 1;
                    q++;
                }
            }

            if (*q == ']') q++;

            if (invert) match = !match;

            if (match) {
                p = q;
                s++;
                continue;
            }
        } else if (*p == *s) {
            p++;
            s++;
            continue;
        }

        if (star_p) {
            p = star_p;
            star_s++;
            s = star_s;
            continue;
        }

        return 0;
    }

    while (*p == '*') p++;
    return *p == '\0';
}

/* ============================================================================
 * File Find (Glob-based recursive file search)
 * ============================================================================ */

#define MAX_DIR_DEPTH 64

static void handle_file_find(struct stream *s, char *base_path, char *pattern)
{
    dir_frame stack[MAX_DIR_DEPTH];
    int path_len[MAX_DIR_DEPTH];
    int depth;
    static char path[PROTO_MAX_PATH];
    static unsigned char buf[PROTO_MAX_PATH + 4];
    int namelen;
    int exists, is_dir;
    win_u64 size;
    unsigned long mtime;
    char *base_name;

    strncpy(path, base_path, PROTO_MAX_PATH - 1);
    path[PROTO_MAX_PATH - 1] = '\0';

    if (!win_stat(path, &exists, &is_dir, &size, &mtime) || !exists) {
        send_stream_error(s->id, ERR_NOT_FOUND, "Path not found");
        free_stream(s);
        return;
    }

    if (!is_dir) {
        base_name = strrchr(base_path, '\\');
        if (!base_name) base_name = strrchr(base_path, '/');
        base_name = base_name ? base_name + 1 : base_path;
        if (glob_match(pattern, base_name)) {
            put_u32(buf, s->id);
            strcpy((char *)buf + 4, base_path);
            if (send_stream_data_fc(buf, 4 + (int)strlen(base_path) + 1) < 0) {
                free_stream(s);
                return;
            }
        }
        send_stream_end(s->id, STATUS_OK);
        free_stream(s);
        return;
    }

    if (!dir_open(&stack[0], path)) {
        send_stream_error(s->id, ERR_NOT_FOUND, "Cannot open directory");
        free_stream(s);
        return;
    }
    path_len[0] = (int)strlen(path);
    depth = 0;
    put_u32(buf, s->id);

    while (depth >= 0) {
        if (!dir_read(&stack[depth])) {
            dir_close(&stack[depth]);
            depth--;
            if (depth >= 0) path[path_len[depth]] = '\0';
            continue;
        }

        if (strcmp(stack[depth].fd.cFileName, ".") == 0) continue;
        if (strcmp(stack[depth].fd.cFileName, "..") == 0) continue;

        namelen = (int)strlen(stack[depth].fd.cFileName);
        if (path_len[depth] + 1 + namelen >= PROTO_MAX_PATH - 1) continue;
        path[path_len[depth]] = '\\';
        strcpy(path + path_len[depth] + 1, stack[depth].fd.cFileName);

        if (glob_match(pattern, stack[depth].fd.cFileName)) {
            strcpy((char *)buf + 4, path);
            if (send_stream_data_fc(buf, 4 + (int)strlen(path) + 1) < 0) {
                while (depth >= 0) dir_close(&stack[depth--]);
                free_stream(s);
                return;
            }
        }

        if ((stack[depth].fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
            depth < MAX_DIR_DEPTH - 1) {
            if (dir_open(&stack[depth + 1], path)) {
                depth++;
                path_len[depth] = (int)strlen(path);
            }
        }
    }

    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

/* ============================================================================
 * Boyer-Moore-Horspool Substring Search
 *
 * Ported directly from client.c - pure portable C, no OS dependency.
 * ============================================================================ */

static void bm_build_skip(char *pattern, int plen, int skip[256])
{
    int i;

    for (i = 0; i < 256; i++) {
        skip[i] = plen;
    }

    for (i = 0; i < plen - 1; i++) {
        skip[(unsigned char)pattern[i]] = plen - 1 - i;
    }
}

static char *bm_search(char *text, int tlen, char *pattern, int plen, int skip[256])
{
    int i, j;

    if (plen == 0) return text;
    if (plen > tlen) return NULL;

    i = 0;
    while (i <= tlen - plen) {
        j = plen - 1;
        while (j >= 0 && text[i + j] == pattern[j]) {
            j--;
        }
        if (j < 0) {
            return text + i;
        }
        i += skip[(unsigned char)text[i + plen - 1]];
    }

    return NULL;
}

/* ============================================================================
 * Binary File Detection
 * ============================================================================ */

static int is_binary_file(char *path)
{
    FILE *fp;
    unsigned char buf[512];
    size_t n, i;

    fp = fopen(path, "rb");
    if (!fp) return 0;

    n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    for (i = 0; i < n; i++) {
        if (buf[i] == '\0') return 1;
    }

    return 0;
}

/* ============================================================================
 * File Search (Grep-like content search)
 * ============================================================================ */

#define MAX_LINE_LEN 4096

static int search_in_file(struct stream *s, char *filepath, char *pattern, int plen, int skip[256])
{
    FILE *fp;
    static char line[MAX_LINE_LEN];
    static unsigned char buf[8 + PROTO_MAX_PATH + MAX_LINE_LEN];
    unsigned long line_num;
    int pathlen, linelen;
    char *nl;

    fp = fopen(filepath, "r");
    if (!fp) return 0;  /* Skip unreadable files */

    put_u32(buf, s->id);
    pathlen = (int)strlen(filepath);

    line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        nl = strchr(line, '\r');
        if (nl) *nl = '\0';
        linelen = (int)strlen(line);

        if (bm_search(line, linelen, pattern, plen, skip)) {
            put_u32(buf + 4, line_num);
            strcpy((char *)buf + 8, filepath);
            strcpy((char *)buf + 8 + pathlen + 1, line);
            if (send_stream_data_fc(buf, 8 + pathlen + 1 + linelen + 1) < 0) {
                fclose(fp);
                return -1;
            }
        }
    }

    fclose(fp);
    return 0;
}

static void handle_file_search(struct stream *s, char *base_path, char *pattern)
{
    dir_frame stack[MAX_DIR_DEPTH];
    int path_len[MAX_DIR_DEPTH];
    int depth;
    static char path[PROTO_MAX_PATH];
    int skip[256];
    int plen;
    int namelen;
    int exists, is_dir;
    win_u64 size;
    unsigned long mtime;

    plen = (int)strlen(pattern);
    if (plen == 0) {
        send_stream_end(s->id, STATUS_OK);
        free_stream(s);
        return;
    }
    bm_build_skip(pattern, plen, skip);

    strncpy(path, base_path, PROTO_MAX_PATH - 1);
    path[PROTO_MAX_PATH - 1] = '\0';

    if (!win_stat(path, &exists, &is_dir, &size, &mtime) || !exists) {
        send_stream_error(s->id, ERR_NOT_FOUND, "Path not found");
        free_stream(s);
        return;
    }

    if (!is_dir) {
        if (!is_binary_file(path)) {
            if (search_in_file(s, path, pattern, plen, skip) < 0) {
                free_stream(s);
                return;
            }
        }
        send_stream_end(s->id, STATUS_OK);
        free_stream(s);
        return;
    }

    if (!dir_open(&stack[0], path)) {
        send_stream_error(s->id, ERR_NOT_FOUND, "Cannot open directory");
        free_stream(s);
        return;
    }
    path_len[0] = (int)strlen(path);
    depth = 0;

    while (depth >= 0) {
        if (!dir_read(&stack[depth])) {
            dir_close(&stack[depth]);
            depth--;
            if (depth >= 0) path[path_len[depth]] = '\0';
            continue;
        }

        if (strcmp(stack[depth].fd.cFileName, ".") == 0) continue;
        if (strcmp(stack[depth].fd.cFileName, "..") == 0) continue;

        namelen = (int)strlen(stack[depth].fd.cFileName);
        if (path_len[depth] + 1 + namelen >= PROTO_MAX_PATH - 1) continue;
        path[path_len[depth]] = '\\';
        strcpy(path + path_len[depth] + 1, stack[depth].fd.cFileName);

        if (stack[depth].fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < MAX_DIR_DEPTH - 1) {
                if (dir_open(&stack[depth + 1], path)) {
                    depth++;
                    path_len[depth] = (int)strlen(path);
                }
            }
        } else {
            if (!is_binary_file(path)) {
                if (search_in_file(s, path, pattern, plen, skip) < 0) {
                    while (depth >= 0) dir_close(&stack[depth--]);
                    free_stream(s);
                    return;
                }
            }
        }
    }

    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

/* ============================================================================
 * Directory Listing
 * ============================================================================ */

static void handle_dir_list(struct stream *s, char *path)
{
    unsigned char buf[512];
    dir_frame df;
    win_u64 size;
    unsigned long mtime;
    unsigned char type;
    int namelen;

    if (!dir_open(&df, path)) {
        send_stream_error(s->id, ERR_NOT_FOUND, "Cannot open directory");
        free_stream(s);
        return;
    }

    put_u32(buf, s->id);

    while (dir_read(&df)) {
        if (strcmp(df.fd.cFileName, ".") == 0 || strcmp(df.fd.cFileName, "..") == 0)
            continue;

        type = (df.fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 'd' : 'f';
        size = ((win_u64)df.fd.nFileSizeHigh << 32) | (win_u64)df.fd.nFileSizeLow;
        mtime = filetime_to_unix(&df.fd.ftLastWriteTime);

        /* Entry format: type(1) + size(8) + mtime(8) + name(null-term) */
        buf[4] = type;
        put_u32(buf + 5, (unsigned long)(size >> 32));
        put_u32(buf + 9, (unsigned long)(size & 0xFFFFFFFFUL));
        put_u32(buf + 13, 0);
        put_u32(buf + 17, mtime);

        namelen = (int)strlen(df.fd.cFileName);
        if (namelen > (int)sizeof(buf) - 22) namelen = (int)sizeof(buf) - 22;
        memcpy(buf + 21, df.fd.cFileName, namelen);
        buf[21 + namelen] = '\0';

        if (send_stream_data_fc(buf, 21 + namelen + 1) < 0) {
            dir_close(&df);
            free_stream(s);
            return;
        }
    }

    dir_close(&df);
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

/* ============================================================================
 * Command Execution (Streaming via CreateProcess + file-redirected output)
 *
 * Originally used _popen(), which respects COMSPEC and so transparently
 * runs under COMMAND.COM on Windows 9x or cmd.exe on the NT family - we
 * never hardcode either. But _popen()'s completion detection relies on
 * the read pipe reporting "broken" via PeekNamedPipe once the child
 * exits, and that turned out to be unreliable on genuine Windows 9x
 * (confirmed against a real Dell OptiPlex GX1: the remote command
 * completed - the physical console showed a normal prompt again - but
 * the pipe never signaled broken, so the client sat forever waiting for
 * output that would never come, and no STREAM_END was ever sent).
 *
 * Using CreateProcess() directly gives us the child's actual process
 * HANDLE, so completion is detected via GetExitCodeProcess() - genuinely
 * asking Windows "has this process exited" - independent of whatever
 * quirks the pipe has. As a bonus, this also lets STREAM_CANCEL actually
 * terminate a running command (TerminateProcess), instead of the old
 * _pclose()-blocks-until-exit limitation with no way to force it.
 *
 * Output capture ALSO confirmed broken on real Windows 9x when done via
 * STARTUPINFO's hStdOutput/hStdError pipe redirection: COMMAND.COM's
 * built-in commands (VER, DIR, MEM, ECHO, ...) are implemented with
 * legacy DOS-era I/O internally and don't reliably honor an inherited
 * Win32 stdout handle - `execute_command("VER")` came back completely
 * empty. But COMMAND.COM's OWN shell-level `>` redirection is exactly
 * how it implements its normal, well-supported "redirect to a file"
 * feature, and DOES work reliably (verified: `VER > file` produced
 * correct output every time). So instead of fighting the pipe, this
 * wraps every command in `> tempfile` redirection and reads the file
 * back after the process exits - the same technique, generalized.
 * stderr is intentionally NOT captured (matches the earlier _popen-based
 * behavior): COMMAND.COM doesn't understand "2>&1"-style merging, and
 * even single-stream "2>" redirection isn't reliably supported by
 * genuine DOS-era COMMAND.COM, so it goes to the local console instead.
 * ============================================================================ */

static void handle_exec(struct stream *s, char *command)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    char *comspec;
    char cmdline[2048];
    char tmppath[MAX_PATH];
    DWORD err;

    if (!GetTempPathA(sizeof(tmppath), tmppath) ||
        !GetTempFileNameA(tmppath, "tex", 0, s->exec_tmpfile)) {
        send_stream_error(s->id, ERR_IO_ERROR, "GetTempFileName failed");
        free_stream(s);
        return;
    }

    comspec = getenv("COMSPEC");
    if (!comspec) comspec = "COMMAND.COM";
    _snprintf(cmdline, sizeof(cmdline), "%s /C %s > \"%s\"", comspec, command, s->exec_tmpfile);
    cmdline[sizeof(cmdline) - 1] = '\0';

    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                         NULL, NULL, &si, &pi)) {
        err = GetLastError();
        {
            char msg[64];
            _snprintf(msg, sizeof(msg), "CreateProcess failed (error %lu)", err);
            msg[sizeof(msg) - 1] = '\0';
            send_stream_error(s->id, ERR_IO_ERROR, msg);
        }
        free_stream(s);
        return;
    }

    CloseHandle(pi.hThread);
    s->child_process = pi.hProcess;
    s->exec_sent = 0;
}

/*
 * Poll a running exec stream: check whether the process has exited (via
 * GetExitCodeProcess() - see the comment above handle_exec for why not
 * pipe-broken detection), and once it has, read its output back from
 * the redirect file COMMAND.COM wrote to and stream it as one or more
 * chunks. Returns 1 if stream still active, 0 if done, -1 on error.
 */
static int poll_exec_stream(struct stream *s)
{
    static unsigned char buf[SMALL_CHUNK + 5];
    DWORD exit_code;
    FILE *fp;
    size_t nread;

    if (!s->child_process) return 0;

    if (!GetExitCodeProcess(s->child_process, &exit_code) || exit_code == STILL_ACTIVE) {
        /* Still running (or the check itself failed transiently) -
         * nothing more to do this poll. */
        return 1;
    }

    if (!s->exec_sent) {
        s->exec_sent = 1;
        put_u32(buf, s->id);
        buf[4] = CHAN_STDOUT;

        fp = fopen(s->exec_tmpfile, "rb");
        if (fp) {
            while ((nread = fread(buf + 5, 1, SMALL_CHUNK, fp)) > 0) {
                if (send_stream_data_fc(buf, 5 + (int)nread) < 0) {
                    fclose(fp);
                    return -1;
                }
            }
            fclose(fp);
        }
        /* If the temp file couldn't be opened, we still send STREAM_END
         * below with the real exit code - a command that failed to even
         * start would show up as a nonzero/unusual exit code rather than
         * output, which is enough signal without treating this as a
         * transport-level stream error. */
    }

    {
        unsigned char endbuf[9];
        put_u32(endbuf, s->id);
        endbuf[4] = EXIT_NORMAL;
        put_u32(endbuf + 5, (unsigned long)exit_code);
        send_packet(PKT_STREAM_END, endbuf, 9);
    }

    free_stream(s);
    return 0;
}

/* ============================================================================
 * STREAM_OPEN Handler
 * ============================================================================ */

/*
 * Safely extract a null-terminated string from a buffer.
 * Ported directly from client.c.
 */
static char *safe_string(unsigned char *buf, int offset, int length, unsigned char **end_out)
{
    unsigned char *start, *p, *limit;

    if (offset >= length) return NULL;

    start = buf + offset;
    limit = buf + length;

    for (p = start; p < limit; p++) {
        if (*p == '\0') {
            if (end_out) *end_out = p + 1;
            return (char *)start;
        }
    }

    return NULL;
}

static void handle_stream_open(unsigned char *payload, int length)
{
    unsigned long stream_id;
    int stream_type;
    struct stream *s;
    char *path, *newpath;
    unsigned char *path_end;
    int mode;

    if (length < 5) return;

    stream_id = get_u32(payload);
    stream_type = payload[4];

    if (find_stream(stream_id)) {
        send_stream_error(stream_id, ERR_INVALID, "Stream ID already in use");
        return;
    }

    path = safe_string(payload, 5, length, &path_end);
    if (!path) {
        send_stream_error(stream_id, ERR_INVALID, "Invalid path (no null terminator)");
        return;
    }

    if (strlen(path) >= PROTO_MAX_PATH) {
        send_stream_error(stream_id, ERR_INVALID, "Path too long");
        return;
    }

    s = alloc_stream(stream_id);
    if (!s) {
        send_stream_error(stream_id, ERR_NO_MEMORY, "Too many streams");
        return;
    }
    s->type = stream_type;

    switch (stream_type) {
        case STREAM_FILE_READ:
            handle_file_read(s, path);
            break;

        case STREAM_FILE_WRITE:
            mode = 0;
            if (path_end + 2 <= payload + length) {
                mode = get_u16(path_end);
            }
            handle_file_write(s, path, mode);
            break;

        case STREAM_EXEC:
            handle_exec(s, path);
            break;

        case STREAM_DIR_LIST:
            handle_dir_list(s, path);
            break;

        case STREAM_FILE_STAT:
            handle_file_stat(s, path);
            break;

        case STREAM_FILE_EXISTS:
            handle_file_exists(s, path);
            break;

        case STREAM_MKDIR:
            handle_mkdir(s, path);
            break;

        case STREAM_REMOVE:
            handle_remove(s, path);
            break;

        case STREAM_MOVE:
            newpath = safe_string(payload, (int)(path_end - payload), length, NULL);
            if (!newpath) {
                send_stream_error(stream_id, ERR_INVALID, "Invalid destination path");
                free_stream(s);
                return;
            }
            if (strlen(newpath) >= PROTO_MAX_PATH) {
                send_stream_error(stream_id, ERR_INVALID, "Destination path too long");
                free_stream(s);
                return;
            }
            handle_move(s, path, newpath);
            break;

        case STREAM_REALPATH:
            handle_realpath(s, path);
            break;

        case STREAM_FILE_FIND:
            newpath = safe_string(payload, (int)(path_end - payload), length, NULL);
            if (!newpath) {
                send_stream_error(stream_id, ERR_INVALID, "Invalid search pattern");
                free_stream(s);
                return;
            }
            handle_file_find(s, path, newpath);
            break;

        case STREAM_FILE_SEARCH:
            newpath = safe_string(payload, (int)(path_end - payload), length, NULL);
            if (!newpath) {
                send_stream_error(stream_id, ERR_INVALID, "Invalid search pattern");
                free_stream(s);
                return;
            }
            handle_file_search(s, path, newpath);
            break;

        default:
            send_stream_error(stream_id, ERR_INVALID, "Unknown stream type");
            free_stream(s);
            break;
    }
}

/* ============================================================================
 * Packet Handlers
 * ============================================================================ */

static void send_window_update(void)
{
    unsigned char buf[4];

    if (bytes_to_ack >= WINDOW_UPDATE_THRESHOLD) {
        put_u32(buf, bytes_to_ack);
        send_packet(PKT_WINDOW_UPDATE, buf, 4);
        if (logfile) {
            fprintf(logfile, "[FLOW] Sent window update +%lu\n", bytes_to_ack);
        }
        bytes_to_ack = 0;
    }
}

static void handle_stream_data(unsigned char *payload, int length)
{
    unsigned long stream_id;
    struct stream *s;

    if (length < 4) return;

    stream_id = get_u32(payload);
    s = find_stream(stream_id);
    if (!s) {
        if (logfile) fprintf(logfile, "[WARN] Data for unknown stream %lu\n", stream_id);
        return;
    }

    if (s->type == STREAM_FILE_WRITE) {
        handle_file_write_data(s, payload + 4, length - 4);
    }

    bytes_to_ack += length;
    send_window_update();
}

static void handle_stream_end(unsigned char *payload, int length)
{
    unsigned long stream_id;
    struct stream *s;

    if (length < 5) return;

    stream_id = get_u32(payload);
    s = find_stream(stream_id);
    if (!s) return;

    if (s->type == STREAM_FILE_WRITE) {
        handle_file_write_end(s);
    } else {
        free_stream(s);
    }
}

static void handle_stream_cancel(unsigned char *payload, int length)
{
    unsigned long stream_id;
    struct stream *s;

    if (length < 4) return;

    stream_id = get_u32(payload);
    s = find_stream(stream_id);
    if (!s) return;

    send_stream_end(stream_id, STATUS_CANCELLED);
    free_stream(s);
}

static void handle_window_update(unsigned char *payload, int length)
{
    unsigned long increment;

    if (length < 4) return;

    increment = get_u32(payload);
    if (bytes_in_flight >= increment) {
        bytes_in_flight -= increment;
    } else {
        bytes_in_flight = 0;
    }

    if (logfile) {
        fprintf(logfile, "[FLOW] Window update +%lu, in_flight=%lu\n",
                increment, bytes_in_flight);
    }
}

/* ============================================================================
 * Console Input -> TERM_INPUT
 *
 * Windows' select() only works on sockets, not console handles or pipes,
 * so console input is polled separately via WaitForSingleObject() +
 * ReadConsoleInputA() each iteration of the main loop (this also lets us
 * translate arrow/Home/End/Delete/PageUp/PageDown into the VT-style
 * escape sequences Claude Code expects, and to catch console resize
 * events directly instead of via a SIGWINCH-style signal).
 * ============================================================================ */

static void append_key(char *buf, int *len, int cap, char c)
{
    if (*len < cap) buf[(*len)++] = c;
}

/* Local echo, independent of the remote round-trip.
 *
 * The relay now dedups Claude's redraw-in-place lines (--ax-screen-reader
 * re-announces the whole input buffer on every keystroke, which was very
 * noisy), but that also silently ate the only feedback of what you were
 * typing. Echoing locally as each key is captured, the conventional
 * approach for dumb terminals/serial links, fixes that independent of
 * whatever the remote side redraws. Backspace uses the classic
 * backspace-space-backspace trick, which erases the previous character
 * in place on any console, ANSI-aware or not. */
static void local_echo(char ch)
{
    DWORD written;
    char seq[3];

    if (ch == '\b' || ch == 0x7F) {
        seq[0] = '\b'; seq[1] = ' '; seq[2] = '\b';
        WriteConsoleA(hStdout, seq, 3, &written, NULL);
    } else if (ch == '\r' || ch == '\n') {
        seq[0] = '\r'; seq[1] = '\n';
        WriteConsoleA(hStdout, seq, 2, &written, NULL);
    } else if (ch >= 0x20 && ch < 0x7F) {
        WriteConsoleA(hStdout, &ch, 1, &written, NULL);
    }
    /* Other control chars (Ctrl+C, Tab, Esc, ...): no local echo - not
     * meaningful to render as typed text on a linear scrolling console. */
}

static void handle_console_input(void)
{
    INPUT_RECORD input_buf[128];
    DWORD events_read, i;
    char keys[256];
    int klen;
    int rows, cols;
    unsigned char resize_buf[4];

    if (WaitForSingleObject(hStdin, 0) != WAIT_OBJECT_0) return;
    if (!ReadConsoleInputA(hStdin, input_buf, 128, &events_read)) return;

    klen = 0;

    for (i = 0; i < events_read; i++) {
        if (input_buf[i].EventType == KEY_EVENT &&
            input_buf[i].Event.KeyEvent.bKeyDown) {
            KEY_EVENT_RECORD *key = &input_buf[i].Event.KeyEvent;
            char ch = key->uChar.AsciiChar;
            WORD vk = key->wVirtualKeyCode;

            if (ch != 0) {
                if (simple_mode) local_echo(ch);
                append_key(keys, &klen, (int)sizeof(keys), ch);
            } else {
                switch (vk) {
                    case VK_UP:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), 'A');
                        break;
                    case VK_DOWN:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), 'B');
                        break;
                    case VK_RIGHT:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), 'C');
                        break;
                    case VK_LEFT:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), 'D');
                        break;
                    case VK_HOME:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), 'H');
                        break;
                    case VK_END:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), 'F');
                        break;
                    case VK_DELETE:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), '3');
                        append_key(keys, &klen, (int)sizeof(keys), '~');
                        break;
                    case VK_PRIOR:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), '5');
                        append_key(keys, &klen, (int)sizeof(keys), '~');
                        break;
                    case VK_NEXT:
                        append_key(keys, &klen, (int)sizeof(keys), 0x1b);
                        append_key(keys, &klen, (int)sizeof(keys), '[');
                        append_key(keys, &klen, (int)sizeof(keys), '6');
                        append_key(keys, &klen, (int)sizeof(keys), '~');
                        break;
                    default:
                        break;
                }
            }
        } else if (input_buf[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
            get_terminal_size(&rows, &cols);
            put_u16(resize_buf, (unsigned int)rows);
            put_u16(resize_buf + 2, (unsigned int)cols);
            send_packet(PKT_TERM_RESIZE, resize_buf, 4);
        }
    }

    if (klen > 0) {
        send_packet(PKT_TERM_INPUT, (unsigned char *)keys, klen);
    }
}

/* ============================================================================
 * Main Loop
 * ============================================================================ */

static void main_loop(void)
{
    fd_set readfds;
    struct timeval tv;
    int rows, cols;
    unsigned char *payload;
    int length, type;
    unsigned char resize_buf[4];
    int i;

    enable_raw_mode();

    get_terminal_size(&rows, &cols);
    put_u16(resize_buf, (unsigned int)rows);
    put_u16(resize_buf + 2, (unsigned int)cols);
    send_packet(PKT_TERM_RESIZE, resize_buf, 4);

    while (1) {
        handle_console_input();

        /* Poll active EXEC streams - Windows has no way to select() on
         * an anonymous pipe HANDLE together with a socket, so we poll
         * on the same short cadence as the socket wait below. */
        for (i = 0; i < MAX_STREAMS; i++) {
            if (streams[i].state != STREAM_STATE_IDLE &&
                streams[i].type == STREAM_EXEC &&
                streams[i].child_process != NULL) {
                if (poll_exec_stream(&streams[i]) < 0) {
                    fprintf(stderr, "\r\nFlow control error\r\n");
                    goto done;
                }
            }
        }

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  /* 10ms - short for responsiveness */

        if (select(0, &readfds, NULL, NULL, &tv) == SOCKET_ERROR) {
            break;
        }

        if (FD_ISSET(sockfd, &readfds) || recv_buf_len > 0) {
            while (1) {
                type = recv_packet(&payload, &length);
                if (type < 0) {
                    fprintf(stderr, "\r\nConnection closed\r\n");
                    goto done;
                }
                if (type == 0) break;

                switch (type) {
                    case PKT_TERM_OUTPUT: {
                        DWORD written;
                        if (simple_mode) {
                            int filtered_len = filter_simple(payload, length);
                            WriteConsoleA(hStdout, payload, (DWORD)filtered_len, &written, NULL);
                        } else {
                            WriteConsoleA(hStdout, payload, (DWORD)length, &written, NULL);
                        }
                        bytes_to_ack += length;
                        send_window_update();
                        break;
                    }

                    case PKT_STREAM_OPEN:
                        handle_stream_open(payload, length);
                        break;

                    case PKT_STREAM_DATA:
                        handle_stream_data(payload, length);
                        break;

                    case PKT_STREAM_END:
                        handle_stream_end(payload, length);
                        break;

                    case PKT_STREAM_CANCEL:
                        handle_stream_cancel(payload, length);
                        break;

                    case PKT_WINDOW_UPDATE:
                        handle_window_update(payload, length);
                        break;

                    case PKT_PING:
                        send_packet(PKT_PONG, payload, length);
                        break;

                    case PKT_GOODBYE:
                        if (logfile) {
                            fprintf(logfile, "[GOODBYE] reason=%d\n",
                                    length > 0 ? payload[0] : -1);
                        }
                        fprintf(stderr, "\r\nServer disconnected\r\n");
                        consume_packet(length);
                        goto done;

                    default:
                        if (logfile) {
                            fprintf(logfile, "[WARN] Unknown packet type 0x%02X\n", type);
                        }
                        break;
                }

                consume_packet(length);
            }
        }
    }

done:
    disable_raw_mode();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[])
{
    char *host = NULL;
    int port = 0;
    int i;

    for (i = 0; i < MAX_STREAMS; i++) {
        streams[i].state = STREAM_STATE_IDLE;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--simple") == 0) {
            simple_mode = 1;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--resume") == 0) {
            resume_mode = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            logfile = fopen("telepresence-v2.log", "w");
            if (logfile) {
                fprintf(stderr, "Logging to telepresence-v2.log\n");
            } else {
                logfile = fopen("C:\\telepresence-v2.log", "w");
                if (logfile) {
                    fprintf(stderr, "Logging to C:\\telepresence-v2.log\n");
                } else {
                    fprintf(stderr, "Warning: Could not open log file: %s\n",
                            strerror(errno));
                }
            }
        } else if (argv[i][0] != '-') {
            if (!host) {
                host = argv[i];
            } else if (!port) {
                port = atoi(argv[i]);
            }
        }
    }

    if (!host || !port) {
        fprintf(stderr, "Usage: %s [-s] [-r] [-l] <host> <port>\n", argv[0]);
        fprintf(stderr, "\nOptions:\n");
        fprintf(stderr, "  -s, --simple   Simple mode (ASCII terminal)\n");
        fprintf(stderr, "  -r, --resume   Resume previous session\n");
        fprintf(stderr, "  -l, --log      Enable debug logging\n");
        return 1;
    }

    recv_buf_cap = 8192;
    recv_buf = malloc(recv_buf_cap);
    if (!recv_buf) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    fprintf(stderr, "Connecting to %s:%d...\n", host, port);

    if (connect_to_relay(host, port) < 0) {
        return 1;
    }

    fprintf(stderr, "Connected, sending HELLO...\n");

    if (send_hello() < 0) {
        fprintf(stderr, "Failed to send HELLO\n");
        return 1;
    }

    if (wait_for_hello_ack() < 0) {
        return 1;
    }

    fprintf(stderr, "Session established.\n\n");

    main_loop();

    if (sockfd != INVALID_SOCKET) {
        closesocket(sockfd);
    }
    WSACleanup();

    if (recv_buf) free(recv_buf);
    if (logfile) fclose(logfile);

    return 0;
}
