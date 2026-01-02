/*
 * claude-telepresence client
 *
 * Binary streaming protocol client for legacy Unix systems.
 * Written in K&R C for maximum portability.
 *
 * Build:
 *   HP-UX:    cc -o claude-telepresence client.c
 *   Solaris:  cc -o claude-telepresence client.c -lsocket -lnsl
 *   IRIX/AIX: cc -o claude-telepresence client.c
 *   Linux:    gcc -o claude-telepresence client.c
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
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

/* Platform-specific terminal handling */
#if (defined(NeXT) || defined(__NeXT__)) || (defined(__MACH__) && !defined(__APPLE__))
#define NEXT_COMPAT 1
#include <libc.h>
#include <sys/dir.h>
#include <sgtty.h>
#define dirent direct
#else
#ifndef __hpux
#include <sys/select.h>
#endif
#include <termios.h>
#include <dirent.h>
#endif

#ifdef __hpux
#include <sys/termios.h>
#endif

/* POSIX macros for older systems */
#ifndef S_ISDIR
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#endif
#ifndef S_ISREG
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#endif
#ifndef S_ISLNK
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#endif

/* ============================================================================
 * Protocol Constants (from PROTOCOL_V2.md)
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

/* Limits */
#define MAX_PACKET_SIZE     (1 * 1024 * 1024)   /* 1 MB - safe for legacy systems */
#define MAX_PATH            4096
#define MAX_STREAMS         256
#define DEFAULT_WINDOW      (256 * 1024)        /* 256 KB */
#define MIN_WINDOW          (16 * 1024)         /* 16 KB */
#define CHUNK_SIZE          (64 * 1024)         /* 64 KB for file I/O */
#define SMALL_CHUNK         4096                /* 4 KB for exec output */

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/* Stream state */
#define STREAM_STATE_IDLE       0
#define STREAM_STATE_OPEN       1
#define STREAM_STATE_HALF_LOCAL 2   /* We sent END, waiting for peer */
#define STREAM_STATE_HALF_REMOTE 3  /* Peer sent END, we still sending */
#define STREAM_STATE_CLOSED     4

struct stream {
    unsigned long id;
    int state;
    int type;
    int child_pid;      /* For EXEC streams */
    int child_fd;       /* Pipe to child stdout/stderr */
    FILE *file_fp;      /* For file read/write */
    DIR *dir_ptr;       /* For directory listing */
};

/* Global state */
static int sockfd = -1;
static int simple_mode = 0;
static int resume_mode = 0;
static int raw_mode = 0;
static FILE *logfile = NULL;

/* Terminal state */
#ifdef NEXT_COMPAT
static struct sgttyb orig_sgttyb;
static struct tchars orig_tchars;
static struct ltchars orig_ltchars;
static int orig_lmode;
#else
static struct termios orig_termios;
#endif

/* Flow control */
static unsigned long send_window = DEFAULT_WINDOW;
static unsigned long recv_window = DEFAULT_WINDOW;
static unsigned long bytes_in_flight = 0;
static unsigned long bytes_to_ack = 0;  /* Bytes received, not yet acknowledged */
#define WINDOW_UPDATE_THRESHOLD 8192  /* Send update every 8KB for responsiveness */

/* Receive buffer for packet reassembly */
static unsigned char *recv_buf = NULL;
static int recv_buf_len = 0;
static int recv_buf_cap = 0;

/* Stream table - odd IDs only (client-initiated would be odd, but we mostly receive) */
static struct stream streams[MAX_STREAMS];
/* static int next_stream_id = 1; */  /* Client uses odd IDs - for future use */

/* Working directory from HELLO */
static char remote_cwd[MAX_PATH];

/* Signal handling */
static int got_sigwinch = 0;

static void sigwinch_handler(sig)
int sig;
{
    got_sigwinch = 1;
}

/* ============================================================================
 * Simple Mode Filter
 *
 * Strips SGR (color) sequences, converts UTF-8 to ASCII.
 * State machine handles sequences split across packet boundaries.
 * In-place filtering - output always <= input.
 * ============================================================================ */

/* Filter states */
#define FLT_NORMAL  0
#define FLT_ESC     1
#define FLT_CSI     2
#define FLT_UTF8    3

/* Filter state - persists across packets */
static struct {
    int state;
    unsigned char seq[32];
    int seq_len;
    int utf8_need;
    int spinner;
} flt = { FLT_NORMAL, {0}, 0, 0, 0 };

static char spinner_chars[4] = { '-', '\\', '|', '/' };

/*
 * Convert completed UTF-8 sequence to ASCII.
 * Returns ASCII character based on mapping tables in spec.
 */
static int utf8_to_ascii(seq, len)
unsigned char *seq;
int len;
{
    unsigned char b0, b1, b2;

    if (len < 2) return '?';

    b0 = seq[0];
    b1 = seq[1];

    /* 2-byte sequences (C2/C3 xx) */
    if (len == 2) {
        if (b0 == 0xC2) {
            if (b1 == 0xA0) return ' ';           /* NBSP */
            if (b1 == 0xB7) {                     /* middle dot - spinner */
                return spinner_chars[flt.spinner++ & 3];
            }
        }
        return '?';
    }

    /* 3-byte sequences (E2 xx xx) */
    if (len == 3 && b0 == 0xE2) {
        b2 = seq[2];

        /* Box drawing E2 94 xx, E2 95 xx */
        if (b1 == 0x94) {
            /* Check vertical first (82, 83) before horizontal range */
            if (b2 == 0x82 || b2 == 0x83) return '|';
            if (b2 == 0x80 || b2 == 0x81 || b2 == 0x84) return '-';
            return '+';
        }
        if (b1 == 0x95) {
            if (b2 >= 0x90 && b2 <= 0x94) return '=';
            return '+';
        }

        /* Arrows E2 86 xx */
        if (b1 == 0x86) {
            if (b2 == 0x90) return '<';
            if (b2 == 0x91) return '^';
            if (b2 == 0x92) return '>';
            if (b2 == 0x93) return 'v';
            return '>';
        }

        /* Geometric shapes E2 96 xx */
        if (b1 == 0x96) {
            if (b2 >= 0xB2 && b2 <= 0xB5) return '^';
            if (b2 >= 0xB6 && b2 <= 0xB9) return '>';
            if (b2 >= 0xBA && b2 <= 0xBD) return 'v';
            return '*';
        }

        /* Geometric shapes E2 97 xx */
        if (b1 == 0x97) {
            if (b2 >= 0x80 && b2 <= 0x83) return '<';
            if (b2 == 0x8F) {  /* black circle - spinner */
                return spinner_chars[flt.spinner++ & 3];
            }
            if (b2 == 0x8B) return 'o';
            if (b2 == 0x86 || b2 == 0x87) return '*';
            return '*';
        }

        /* Dingbats E2 9C xx */
        if (b1 == 0x9C) {
            if (b2 == 0x93 || b2 == 0x94) return '+';  /* checkmarks */
            if (b2 == 0x85) return '+';                /* heavy check */
            if (b2 == 0x97 || b2 == 0x98) return 'x';  /* X marks */
            /* Stars - spinner */
            if (b2 == 0xA2 || b2 == 0xB3 || b2 == 0xB6 ||
                b2 == 0xBB || b2 == 0xBD) {
                return spinner_chars[flt.spinner++ & 3];
            }
            return '*';
        }

        /* Dingbats E2 9D xx */
        if (b1 == 0x9D) {
            if (b2 == 0x8C) return 'x';  /* cross mark */
            return '*';
        }

        /* Heavy arrows E2 9E xx */
        if (b1 == 0x9E) {
            return '>';
        }

        /* Math operators E2 88 xx */
        if (b1 == 0x88) {
            if (b2 == 0xB4) {  /* therefore - spinner */
                return spinner_chars[flt.spinner++ & 3];
            }
            return '*';
        }

        /* Technical symbols E2 8C-8F xx */
        if (b1 >= 0x8C && b1 <= 0x8F) {
            return '>';
        }

        /* General punctuation E2 80 xx */
        if (b1 == 0x80) {
            if (b2 == 0xA2) return '*';                /* bullet */
            if (b2 == 0xA3) return '>';                /* triangular bullet */
            if (b2 >= 0x93 && b2 <= 0x95) return '-';  /* dashes */
            if (b2 == 0x98 || b2 == 0x99) return '\''; /* single quotes */
            if (b2 == 0x9C || b2 == 0x9D) return '"';  /* double quotes */
            if (b2 == 0xA6) return '.';                /* ellipsis */
            if (b2 == 0xB9) return '<';                /* left angle */
            if (b2 == 0xBA) return '>';                /* right angle */
            return ' ';
        }

        return '?';
    }

    /* 4-byte sequences (F0 xx xx xx) - emoji */
    if (len == 4 && b0 == 0xF0) {
        if (b1 == 0x9F) return '*';  /* emoji */
        return '?';
    }

    return '?';
}

/*
 * Filter terminal output in-place for simple mode.
 * Returns new length (always <= original).
 * State persists across calls for split sequences.
 */
static int filter_simple(buf, len)
unsigned char *buf;
int len;
{
    unsigned char *r, *w, *end;
    unsigned char c;
    int i;

    if (len <= 0) return 0;

    r = buf;
    w = buf;
    end = buf + len;

    while (r < end) {
        c = *r++;

        switch (flt.state) {

        case FLT_NORMAL:
            if (c == 0x1B) {
                /* ESC - start escape sequence */
                flt.state = FLT_ESC;
                flt.seq[0] = c;
                flt.seq_len = 1;
            } else if (c < 0x80) {
                /* ASCII - pass through */
                *w++ = c;
            } else if ((c & 0xE0) == 0xC0) {
                /* 2-byte UTF-8 start */
                flt.state = FLT_UTF8;
                flt.seq[0] = c;
                flt.seq_len = 1;
                flt.utf8_need = 1;
            } else if ((c & 0xF0) == 0xE0) {
                /* 3-byte UTF-8 start */
                flt.state = FLT_UTF8;
                flt.seq[0] = c;
                flt.seq_len = 1;
                flt.utf8_need = 2;
            } else if ((c & 0xF8) == 0xF0) {
                /* 4-byte UTF-8 start */
                flt.state = FLT_UTF8;
                flt.seq[0] = c;
                flt.seq_len = 1;
                flt.utf8_need = 3;
            } else {
                /* Invalid UTF-8 start byte */
                *w++ = '?';
            }
            break;

        case FLT_ESC:
            flt.seq[flt.seq_len++] = c;
            if (c == '[') {
                /* CSI sequence */
                flt.state = FLT_CSI;
            } else {
                /* Non-CSI escape - pass through */
                for (i = 0; i < flt.seq_len; i++) {
                    *w++ = flt.seq[i];
                }
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
            }
            break;

        case FLT_CSI:
            if (c < 0x20) {
                /* Control character - execute immediately, stay in CSI state */
                /* This handles BS, CR, LF, etc. during escape sequences */
                *w++ = c;
            } else if (c >= 0x40 && c <= 0x7E) {
                /* Command byte - end of CSI */
                flt.seq[flt.seq_len++] = c;
                if (c == 'm') {
                    /* SGR (color) - discard */
                } else {
                    /* Other CSI - pass through */
                    for (i = 0; i < flt.seq_len; i++) {
                        *w++ = flt.seq[i];
                    }
                }
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
            } else if (flt.seq_len >= 30) {
                /* Too long - flush and reset */
                for (i = 0; i < flt.seq_len; i++) {
                    *w++ = flt.seq[i];
                }
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
            } else {
                /* Parameter/intermediate byte (0x20-0x3F) - accumulate */
                flt.seq[flt.seq_len++] = c;
            }
            break;

        case FLT_UTF8:
            if ((c & 0xC0) == 0x80) {
                /* Valid continuation byte */
                flt.seq[flt.seq_len++] = c;
                flt.utf8_need--;
                if (flt.utf8_need == 0) {
                    /* Complete - convert to ASCII */
                    *w++ = utf8_to_ascii(flt.seq, flt.seq_len);
                    flt.state = FLT_NORMAL;
                    flt.seq_len = 0;
                }
            } else {
                /* Invalid continuation - output ? and reprocess */
                *w++ = '?';
                flt.state = FLT_NORMAL;
                flt.seq_len = 0;
                r--;  /* Back up to reprocess this byte */
            }
            break;
        }
    }

    return w - buf;
}

/* ============================================================================
 * Logging
 * ============================================================================ */

static void log_packet(direction, type, length)
char *direction;
int type;
int length;
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

static unsigned long get_u32(buf)
unsigned char *buf;
{
    return ((unsigned long)buf[0] << 24) |
           ((unsigned long)buf[1] << 16) |
           ((unsigned long)buf[2] << 8) |
           (unsigned long)buf[3];
}

static void put_u32(buf, val)
unsigned char *buf;
unsigned long val;
{
    buf[0] = (val >> 24) & 0xFF;
    buf[1] = (val >> 16) & 0xFF;
    buf[2] = (val >> 8) & 0xFF;
    buf[3] = val & 0xFF;
}

static unsigned int get_u16(buf)
unsigned char *buf;
{
    return ((unsigned int)buf[0] << 8) | (unsigned int)buf[1];
}

static void put_u16(buf, val)
unsigned char *buf;
unsigned int val;
{
    buf[0] = (val >> 8) & 0xFF;
    buf[1] = val & 0xFF;
}

/* ============================================================================
 * Packet I/O
 * ============================================================================ */

/*
 * Send a packet. Returns 0 on success, -1 on error.
 */
static int send_packet(type, payload, length)
int type;
unsigned char *payload;
int length;
{
    unsigned char header[5];
    int sent, n;

    if (length > MAX_PACKET_SIZE) {
        if (logfile) fprintf(logfile, "[ERROR] Packet too large: %d\n", length);
        return -1;
    }

    header[0] = type;
    put_u32(header + 1, (unsigned long)length);

    /* Send header */
    sent = 0;
    while (sent < 5) {
        n = write(sockfd, header + sent, 5 - sent);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                /* Would block, wait for socket to be writable */
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(sockfd, &wfds);
                select(sockfd + 1, NULL, &wfds, NULL, NULL);
                continue;
            }
            return -1;
        }
        if (n == 0) return -1;
        sent += n;
    }

    /* Send payload */
    if (length > 0 && payload != NULL) {
        sent = 0;
        while (sent < length) {
            n = write(sockfd, payload + sent, length - sent);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    fd_set wfds;
                    FD_ZERO(&wfds);
                    FD_SET(sockfd, &wfds);
                    select(sockfd + 1, NULL, &wfds, NULL, NULL);
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
 * Payload and length are set on success.
 */
static int recv_packet(payload, length)
unsigned char **payload;
int *length;
{
    unsigned char tmp[4096];
    int n, pkt_len, total_needed;
    unsigned char type;

    /* Read available data into buffer */
    n = read(sockfd, tmp, sizeof(tmp));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            n = 0;  /* No data available */
        } else {
            return -1;  /* Error */
        }
    } else if (n == 0) {
        return -1;  /* Connection closed */
    }

    /* Append to receive buffer */
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

    /* Check if we have a complete packet */
    if (recv_buf_len < 5) return 0;  /* Need header */

    type = recv_buf[0];
    pkt_len = (int)get_u32(recv_buf + 1);

    if (pkt_len > MAX_PACKET_SIZE) {
        if (logfile) fprintf(logfile, "[ERROR] Received packet too large: %d\n", pkt_len);
        return -1;  /* Protocol error */
    }

    total_needed = 5 + pkt_len;
    if (recv_buf_len < total_needed) return 0;  /* Need more data */

    /* Complete packet available */
    *payload = recv_buf + 5;
    *length = pkt_len;

    log_packet("RECV", type, pkt_len);
    return type;
}

/*
 * Consume a packet from the receive buffer after processing.
 */
static void consume_packet(length)
int length;
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
 * Returns 0 on success, -1 on error.
 */
static int wait_for_send_window(needed)
unsigned long needed;
{
    fd_set readfds;
    struct timeval tv;
    unsigned char *payload;
    int length, type;

    while (bytes_in_flight + needed > send_window) {
        /* Need to wait for WINDOW_UPDATE */
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 30;  /* 30 second timeout */
        tv.tv_usec = 0;

        if (select(sockfd + 1, &readfds, NULL, NULL, &tv) <= 0) {
            return -1;  /* Timeout or error */
        }

        type = recv_packet(&payload, &length);
        if (type < 0) return -1;
        if (type == 0) continue;  /* No complete packet yet */

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
            /* Respond to PING while waiting */
            send_packet(PKT_PONG, payload, length);
        } else if (type == PKT_GOODBYE) {
            return -1;
        }
        /* Ignore other packets while waiting for window */

        consume_packet(length);
    }

    return 0;
}

/*
 * Send stream data with flow control.
 * Waits for window space, sends, and tracks bytes_in_flight.
 * Returns 0 on success, -1 on error.
 */
static int send_stream_data_fc(buf, len)
unsigned char *buf;
int len;
{
    /* Wait for window space */
    if (wait_for_send_window((unsigned long)len) < 0) {
        return -1;
    }

    /* Send the packet */
    if (send_packet(PKT_STREAM_DATA, buf, len) < 0) {
        return -1;
    }

    /* Track bytes in flight */
    bytes_in_flight += len;

    return 0;
}

/* ============================================================================
 * Terminal Handling
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

/* ============================================================================
 * Connection Setup
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

    addr = inet_addr(host);
    if (addr != (unsigned long)-1) {
        serv_addr.sin_addr.s_addr = addr;
    } else {
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

    /* Disable Nagle's algorithm for low-latency interactive use */
    {
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, (char *)&flag, sizeof(flag));
    }

    /* Set non-blocking mode */
    fcntl(sockfd, F_SETFL, O_NONBLOCK);

    return 0;
}

static int send_hello()
{
    unsigned char buf[MAX_PATH + 10];
    int flags = 0;
    int len;

    if (resume_mode) flags |= FLAG_RESUME;
    if (simple_mode) flags |= FLAG_SIMPLE;

    /* Get current working directory */
#ifdef NEXT_COMPAT
    if (getwd(remote_cwd) == NULL)
#else
    if (getcwd(remote_cwd, sizeof(remote_cwd)) == NULL)
#endif
    {
        strcpy(remote_cwd, "/");
    }

    buf[0] = PROTO_VERSION;
    buf[1] = flags;
    put_u32(buf + 2, (unsigned long)recv_window);
    strcpy((char *)buf + 6, remote_cwd);
    len = 6 + strlen(remote_cwd) + 1;

    return send_packet(PKT_HELLO, buf, len);
}

static int wait_for_hello_ack()
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

        if (select(sockfd + 1, &fds, NULL, NULL, &tv) < 0) {
            if (errno == EINTR) continue;
            return -1;
        }

        if (!FD_ISSET(sockfd, &fds)) {
            timeout_secs--;
            continue;
        }

        type = recv_packet(&payload, &length);
        if (type < 0) return -1;
        if (type == 0) continue;  /* Incomplete */

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

static struct stream *find_stream(id)
unsigned long id;
{
    int i;
    for (i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].state != STREAM_STATE_IDLE && streams[i].id == id) {
            return &streams[i];
        }
    }
    return NULL;
}

static struct stream *alloc_stream(id)
unsigned long id;
{
    int i;
    for (i = 0; i < MAX_STREAMS; i++) {
        if (streams[i].state == STREAM_STATE_IDLE) {
            memset(&streams[i], 0, sizeof(struct stream));
            streams[i].id = id;
            streams[i].state = STREAM_STATE_OPEN;
            streams[i].child_pid = -1;
            streams[i].child_fd = -1;
            return &streams[i];
        }
    }
    return NULL;  /* No free slots */
}

static void free_stream(s)
struct stream *s;
{
    if (s->file_fp) {
        fclose(s->file_fp);
        s->file_fp = NULL;
    }
    if (s->dir_ptr) {
        closedir(s->dir_ptr);
        s->dir_ptr = NULL;
    }
    if (s->child_fd >= 0) {
        close(s->child_fd);
        s->child_fd = -1;
    }
    if (s->child_pid > 0) {
        kill(s->child_pid, SIGTERM);
        waitpid(s->child_pid, NULL, WNOHANG);
        s->child_pid = -1;
    }
    s->state = STREAM_STATE_IDLE;
}

/* ============================================================================
 * Stream Error Helper
 * ============================================================================ */

static int send_stream_error(stream_id, code, message)
unsigned long stream_id;
int code;
char *message;
{
    unsigned char buf[256];
    int len;

    put_u32(buf, stream_id);
    buf[4] = code;
    strncpy((char *)buf + 5, message, sizeof(buf) - 6);
    buf[sizeof(buf) - 1] = '\0';
    len = 5 + strlen((char *)buf + 5) + 1;

    return send_packet(PKT_STREAM_ERROR, buf, len);
}

static int send_stream_end(stream_id, status)
unsigned long stream_id;
int status;
{
    unsigned char buf[5];

    put_u32(buf, stream_id);
    buf[4] = status;

    return send_packet(PKT_STREAM_END, buf, 5);
}

/* ============================================================================
 * File Operations
 * ============================================================================ */

static void handle_file_read(s, path)
struct stream *s;
char *path;
{
    unsigned char buf[CHUNK_SIZE + 4];
    int n;

    s->file_fp = fopen(path, "rb");
    if (!s->file_fp) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    /* Stream file contents in chunks with flow control */
    put_u32(buf, s->id);
    while ((n = fread(buf + 4, 1, CHUNK_SIZE, s->file_fp)) > 0) {
        if (send_stream_data_fc(buf, 4 + n) < 0) {
            free_stream(s);
            return;
        }
    }

    fclose(s->file_fp);
    s->file_fp = NULL;
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

static void handle_file_write(s, path, mode)
struct stream *s;
char *path;
int mode;
{
    if (mode == 0) mode = 0644;

    s->file_fp = fopen(path, "wb");
    if (!s->file_fp) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    /* Set permissions */
    chmod(path, mode);

    /* Stream is now open, data will come via STREAM_DATA packets */
    /* Don't free_stream - wait for STREAM_END from relay */
}

static void handle_file_write_data(s, data, len)
struct stream *s;
unsigned char *data;
int len;
{
    if (!s->file_fp) {
        send_stream_error(s->id, ERR_INVALID, "No file open");
        return;
    }

    if (fwrite(data, 1, len, s->file_fp) != len) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
        fclose(s->file_fp);
        s->file_fp = NULL;
        free_stream(s);
    }
}

static void handle_file_write_end(s)
struct stream *s;
{
    if (s->file_fp) {
        fclose(s->file_fp);
        s->file_fp = NULL;
    }
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

static void handle_file_stat(s, path)
struct stream *s;
char *path;
{
    unsigned char buf[32];
    struct stat st;
    unsigned char type;

    put_u32(buf, s->id);

    if (stat(path, &st) < 0) {
        buf[4] = 0;  /* exists = false */
        buf[5] = '?';
        put_u32(buf + 6, 0);  /* mode */
        put_u32(buf + 10, 0); put_u32(buf + 14, 0);  /* size (64-bit) */
        put_u32(buf + 18, 0); put_u32(buf + 22, 0);  /* mtime (64-bit) */
    } else {
        if (S_ISREG(st.st_mode)) type = 'f';
        else if (S_ISDIR(st.st_mode)) type = 'd';
        else if (S_ISLNK(st.st_mode)) type = 'l';
        else type = '?';

        buf[4] = 1;  /* exists = true */
        buf[5] = type;
        put_u32(buf + 6, (unsigned long)st.st_mode);
        /* size as 64-bit */
        put_u32(buf + 10, 0);
        put_u32(buf + 14, (unsigned long)st.st_size);
        /* mtime as 64-bit */
        put_u32(buf + 18, 0);
        put_u32(buf + 22, (unsigned long)st.st_mtime);
    }

    if (send_stream_data_fc(buf, 26) < 0) {
        free_stream(s);
        return;
    }
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

static void handle_file_exists(s, path)
struct stream *s;
char *path;
{
    unsigned char buf[6];
    struct stat st;

    put_u32(buf, s->id);
    buf[4] = (stat(path, &st) == 0) ? 1 : 0;

    if (send_stream_data_fc(buf, 5) < 0) {
        free_stream(s);
        return;
    }
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

static void handle_mkdir(s, path)
struct stream *s;
char *path;
{
    if (mkdir(path, 0755) < 0 && errno != EEXIST) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
    } else {
        send_stream_end(s->id, STATUS_OK);
    }
    free_stream(s);
}

static void handle_remove(s, path)
struct stream *s;
char *path;
{
    if (unlink(path) < 0) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
    } else {
        send_stream_end(s->id, STATUS_OK);
    }
    free_stream(s);
}

static void handle_move(s, oldpath, newpath)
struct stream *s;
char *oldpath;
char *newpath;
{
    if (rename(oldpath, newpath) < 0) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
    } else {
        send_stream_end(s->id, STATUS_OK);
    }
    free_stream(s);
}

static void handle_realpath(s, path)
struct stream *s;
char *path;
{
    unsigned char buf[MAX_PATH + 4];
    char resolved[MAX_PATH];

    put_u32(buf, s->id);

    if (realpath(path, resolved) == NULL) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }
    strcpy((char *)buf + 4, resolved);
    if (send_stream_data_fc(buf, 4 + strlen(resolved) + 1) < 0) {
        free_stream(s);
        return;
    }
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

/* ============================================================================
 * Glob Pattern Matching
 *
 * Iterative algorithm with single backtrack point for '*'.
 * Supports: * (any chars), ? (single char), [abc], [a-z], [!abc]
 * O(n*m) worst case, O(n+m) typical. Zero allocations.
 * ============================================================================ */

static int glob_match(p, s)
char *p, *s;
{
    char *star_p, *star_s, *q;
    int match, invert;

    star_p = NULL;
    star_s = NULL;

    while (*s) {
        if (*p == '*') {
            /* Star: save backtrack position, skip consecutive stars */
            while (*p == '*') p++;
            if (!*p) return 1;  /* Trailing * matches everything */
            star_p = p;
            star_s = s;
            continue;
        }

        if (*p == '?') {
            /* Question mark: match any single character */
            p++;
            s++;
            continue;
        }

        if (*p == '[') {
            /* Character class: [abc], [a-z], [!abc] */
            match = 0;
            invert = 0;
            q = p + 1;

            if (*q == '!' || *q == '^') {
                invert = 1;
                q++;
            }

            while (*q && *q != ']') {
                if (q[1] == '-' && q[2] && q[2] != ']') {
                    /* Range: a-z */
                    if ((unsigned char)*s >= (unsigned char)q[0] &&
                        (unsigned char)*s <= (unsigned char)q[2])
                        match = 1;
                    q += 3;
                } else {
                    /* Single character */
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
            /* Fall through to backtrack */
        } else if (*p == *s) {
            /* Literal match */
            p++;
            s++;
            continue;
        }

        /* Mismatch: backtrack to last star */
        if (star_p) {
            p = star_p;
            star_s++;
            s = star_s;
            continue;
        }

        return 0;
    }

    /* String exhausted: pattern must be empty or all stars */
    while (*p == '*') p++;
    return *p == '\0';
}

/* ============================================================================
 * File Find (Glob-based recursive file search)
 *
 * Iterative directory walking with explicit stack.
 * O(depth) memory, streams results as found.
 * ============================================================================ */

#define MAX_DIR_DEPTH 64

static void handle_file_find(s, base_path, pattern)
struct stream *s;
char *base_path;
char *pattern;
{
    DIR *dir_stack[MAX_DIR_DEPTH];
    int path_len[MAX_DIR_DEPTH];
    int depth;
    char path[MAX_PATH];
    struct dirent *ent;
    struct stat st;
    unsigned char buf[MAX_PATH + 4];
    int namelen;

    /* Initialize path */
    strncpy(path, base_path, MAX_PATH - 1);
    path[MAX_PATH - 1] = '\0';

    /* Check if base_path exists */
    if (stat(path, &st) < 0) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    /* If base is a file, just check it */
    if (!S_ISDIR(st.st_mode)) {
        char *name = strrchr(base_path, '/');
        name = name ? name + 1 : base_path;
        if (glob_match(pattern, name)) {
            put_u32(buf, s->id);
            strcpy((char *)buf + 4, base_path);
            if (send_stream_data_fc(buf, 4 + strlen(base_path) + 1) < 0) {
                free_stream(s);
                return;
            }
        }
        send_stream_end(s->id, STATUS_OK);
        free_stream(s);
        return;
    }

    /* Open base directory */
    dir_stack[0] = opendir(path);
    if (!dir_stack[0]) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    path_len[0] = strlen(path);
    depth = 0;
    put_u32(buf, s->id);

    while (depth >= 0) {
        ent = readdir(dir_stack[depth]);

        if (!ent) {
            /* End of directory: pop stack */
            closedir(dir_stack[depth]);
            depth--;
            if (depth >= 0) {
                path[path_len[depth]] = '\0';
            }
            continue;
        }

        /* Skip . and .. */
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }

        /* Build full path */
        namelen = strlen(ent->d_name);
        if (path_len[depth] + 1 + namelen >= MAX_PATH - 1) continue;
        path[path_len[depth]] = '/';
        strcpy(path + path_len[depth] + 1, ent->d_name);

        /* Check if name matches pattern */
        if (glob_match(pattern, ent->d_name)) {
            strcpy((char *)buf + 4, path);
            if (send_stream_data_fc(buf, 4 + strlen(path) + 1) < 0) {
                /* Clean up directory stack on error */
                while (depth >= 0) closedir(dir_stack[depth--]);
                free_stream(s);
                return;
            }
        }

        /* Recurse into directories */
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (depth < MAX_DIR_DEPTH - 1) {
                DIR *subdir = opendir(path);
                if (subdir) {
                    depth++;
                    dir_stack[depth] = subdir;
                    path_len[depth] = strlen(path);
                }
            }
        }
    }

    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

/* ============================================================================
 * Boyer-Moore-Horspool Substring Search
 *
 * O(n/m) average case (sublinear!), O(n*m) worst case.
 * Simple skip table, fast inner loop.
 * ============================================================================ */

static void bm_build_skip(pattern, plen, skip)
char *pattern;
int plen;
int skip[256];
{
    int i;

    for (i = 0; i < 256; i++) {
        skip[i] = plen;
    }

    for (i = 0; i < plen - 1; i++) {
        skip[(unsigned char)pattern[i]] = plen - 1 - i;
    }
}

static char *bm_search(text, tlen, pattern, plen, skip)
char *text;
int tlen;
char *pattern;
int plen;
int skip[256];
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
 *
 * Check for NUL bytes in first 512 bytes.
 * Simple, effective, portable.
 * ============================================================================ */

static int is_binary_file(path)
char *path;
{
    int fd, n, i;
    unsigned char buf[512];

    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;

    n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n <= 0) return 0;

    for (i = 0; i < n; i++) {
        if (buf[i] == '\0') return 1;
    }

    return 0;
}

/* ============================================================================
 * File Search (Grep-like content search)
 *
 * Boyer-Moore-Horspool for fast substring matching.
 * Iterative directory walking, binary file skipping.
 * Streams results as found.
 * ============================================================================ */

#define MAX_LINE_LEN 4096

static int search_in_file(s, filepath, pattern, plen, skip)
struct stream *s;
char *filepath;
char *pattern;
int plen;
int skip[256];
{
    FILE *fp;
    char line[MAX_LINE_LEN];
    unsigned char buf[8 + MAX_PATH + MAX_LINE_LEN];
    unsigned long line_num;
    int pathlen, linelen;
    char *nl;

    fp = fopen(filepath, "r");
    if (!fp) return 0;  /* Skip unreadable files */

    put_u32(buf, s->id);
    pathlen = strlen(filepath);

    line_num = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_num++;

        /* Strip newline */
        nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        linelen = strlen(line);

        /* Search for pattern */
        if (bm_search(line, linelen, pattern, plen, skip)) {
            /* Match format: stream_id(4) + line_num(4) + path(str) + line(str) */
            put_u32(buf + 4, line_num);
            strcpy((char *)buf + 8, filepath);
            strcpy((char *)buf + 8 + pathlen + 1, line);
            if (send_stream_data_fc(buf, 8 + pathlen + 1 + linelen + 1) < 0) {
                fclose(fp);
                return -1;  /* Flow control error */
            }
        }
    }

    fclose(fp);
    return 0;
}

static void handle_file_search(s, base_path, pattern)
struct stream *s;
char *base_path;
char *pattern;
{
    DIR *dir_stack[MAX_DIR_DEPTH];
    int path_len[MAX_DIR_DEPTH];
    int depth;
    char path[MAX_PATH];
    struct dirent *ent;
    struct stat st;
    int skip[256];
    int plen;
    int namelen;

    plen = strlen(pattern);
    if (plen == 0) {
        send_stream_end(s->id, STATUS_OK);
        free_stream(s);
        return;
    }

    /* Build skip table once for all files */
    bm_build_skip(pattern, plen, skip);

    /* Check if base_path exists */
    if (stat(base_path, &st) < 0) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    /* If base is a file, just search it */
    if (S_ISREG(st.st_mode)) {
        if (!is_binary_file(base_path)) {
            if (search_in_file(s, base_path, pattern, plen, skip) < 0) {
                free_stream(s);
                return;
            }
        }
        send_stream_end(s->id, STATUS_OK);
        free_stream(s);
        return;
    }

    /* Initialize path for directory walking */
    strncpy(path, base_path, MAX_PATH - 1);
    path[MAX_PATH - 1] = '\0';

    dir_stack[0] = opendir(path);
    if (!dir_stack[0]) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    path_len[0] = strlen(path);
    depth = 0;

    while (depth >= 0) {
        ent = readdir(dir_stack[depth]);

        if (!ent) {
            /* End of directory: pop stack */
            closedir(dir_stack[depth]);
            depth--;
            if (depth >= 0) {
                path[path_len[depth]] = '\0';
            }
            continue;
        }

        /* Skip . and .. */
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }

        /* Build full path */
        namelen = strlen(ent->d_name);
        if (path_len[depth] + 1 + namelen >= MAX_PATH - 1) continue;
        path[path_len[depth]] = '/';
        strcpy(path + path_len[depth] + 1, ent->d_name);

        if (stat(path, &st) < 0) continue;

        if (S_ISREG(st.st_mode)) {
            /* Regular file: search if not binary */
            if (!is_binary_file(path)) {
                if (search_in_file(s, path, pattern, plen, skip) < 0) {
                    /* Clean up directory stack on error */
                    while (depth >= 0) closedir(dir_stack[depth--]);
                    free_stream(s);
                    return;
                }
            }
        } else if (S_ISDIR(st.st_mode)) {
            /* Directory: push onto stack */
            if (depth < MAX_DIR_DEPTH - 1) {
                DIR *subdir = opendir(path);
                if (subdir) {
                    depth++;
                    dir_stack[depth] = subdir;
                    path_len[depth] = strlen(path);
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

static void handle_dir_list(s, path)
struct stream *s;
char *path;
{
    unsigned char buf[512];
    struct dirent *ent;
    struct stat st;
    char fullpath[MAX_PATH];
    unsigned char type;
    int namelen;

    s->dir_ptr = opendir(path);
    if (!s->dir_ptr) {
        send_stream_error(s->id, ERR_NOT_FOUND, strerror(errno));
        free_stream(s);
        return;
    }

    put_u32(buf, s->id);

    while ((ent = readdir(s->dir_ptr)) != NULL) {
        int pathlen, dnamelen;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        /* Bounds check before sprintf to prevent overflow */
        pathlen = strlen(path);
        dnamelen = strlen(ent->d_name);
        if (pathlen + 1 + dnamelen >= MAX_PATH) {
            continue;  /* Skip entries that would overflow buffer */
        }
        sprintf(fullpath, "%s/%s", path, ent->d_name);
        if (stat(fullpath, &st) < 0) {
            type = '?';
            st.st_size = 0;
            st.st_mtime = 0;
        } else if (S_ISREG(st.st_mode)) {
            type = 'f';
        } else if (S_ISDIR(st.st_mode)) {
            type = 'd';
        } else if (S_ISLNK(st.st_mode)) {
            type = 'l';
        } else {
            type = '?';
        }

        /* Entry format: type(1) + size(8) + mtime(8) + name(null-term) */
        buf[4] = type;
        put_u32(buf + 5, 0);
        put_u32(buf + 9, (unsigned long)st.st_size);
        put_u32(buf + 13, 0);
        put_u32(buf + 17, (unsigned long)st.st_mtime);
        namelen = strlen(ent->d_name);
        if (namelen > sizeof(buf) - 22) namelen = sizeof(buf) - 22;
        memcpy(buf + 21, ent->d_name, namelen);
        buf[21 + namelen] = '\0';

        if (send_stream_data_fc(buf, 21 + namelen + 1) < 0) {
            closedir(s->dir_ptr);
            s->dir_ptr = NULL;
            free_stream(s);
            return;
        }
    }

    closedir(s->dir_ptr);
    s->dir_ptr = NULL;
    send_stream_end(s->id, STATUS_OK);
    free_stream(s);
}

/* ============================================================================
 * Command Execution (Streaming)
 * ============================================================================ */

static void handle_exec(s, command)
struct stream *s;
char *command;
{
    int pipefd[2];
    int pid;

    if (pipe(pipefd) < 0) {
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
        free_stream(s);
        return;
    }

    pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        send_stream_error(s->id, ERR_IO_ERROR, strerror(errno));
        free_stream(s);
        return;
    }

    if (pid == 0) {
        /* Child process */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }

    /* Parent process */
    close(pipefd[1]);
    s->child_pid = pid;
    s->child_fd = pipefd[0];

    /* Set non-blocking */
    fcntl(s->child_fd, F_SETFL, O_NONBLOCK);
}

/*
 * Poll a running exec stream, send any available output.
 * Returns 1 if stream still active, 0 if done, -1 on error.
 */
static int poll_exec_stream(s)
struct stream *s;
{
    unsigned char buf[SMALL_CHUNK + 5];
    int n, status;

    if (s->child_fd < 0) return 0;

    put_u32(buf, s->id);
    buf[4] = CHAN_STDOUT;  /* We merged stdout/stderr */

    n = read(s->child_fd, buf + 5, SMALL_CHUNK);
    if (n > 0) {
        if (send_stream_data_fc(buf, 5 + n) < 0) {
            return -1;  /* Flow control error */
        }
        return 1;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* No data available, check if child still running */
        if (waitpid(s->child_pid, &status, WNOHANG) == 0) {
            return 1;  /* Still running */
        }
        /* Child exited, drain any remaining output */
        while ((n = read(s->child_fd, buf + 5, SMALL_CHUNK)) > 0) {
            if (send_stream_data_fc(buf, 5 + n) < 0) {
                return -1;  /* Flow control error */
            }
        }
    } else {
        /* n == 0 (EOF) or error, wait for child (non-blocking) */
        if (waitpid(s->child_pid, &status, WNOHANG) == 0) {
            /* Child still running, try again later */
            return 1;
        }
    }

    /* Send STREAM_END with exit status */
    close(s->child_fd);
    s->child_fd = -1;
    s->child_pid = -1;

    {
        unsigned char endbuf[9];
        put_u32(endbuf, s->id);
        if (WIFEXITED(status)) {
            endbuf[4] = EXIT_NORMAL;
            put_u32(endbuf + 5, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            endbuf[4] = EXIT_SIGNAL;
            put_u32(endbuf + 5, WTERMSIG(status));
        } else {
            endbuf[4] = EXIT_UNKNOWN;
            put_u32(endbuf + 5, 0);
        }
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
 * Returns pointer to string if null terminator found within bounds, NULL otherwise.
 * If end_out is non-NULL, sets it to point past the null terminator.
 */
static char *safe_string(buf, offset, length, end_out)
unsigned char *buf;
int offset;
int length;
unsigned char **end_out;
{
    unsigned char *start, *p, *limit;

    if (offset >= length) return NULL;

    start = buf + offset;
    limit = buf + length;

    /* Look for null terminator within remaining buffer */
    for (p = start; p < limit; p++) {
        if (*p == '\0') {
            if (end_out) *end_out = p + 1;
            return (char *)start;
        }
    }

    return NULL;  /* No null terminator found */
}

static void handle_stream_open(payload, length)
unsigned char *payload;
int length;
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

    /* Check for duplicate stream ID */
    if (find_stream(stream_id)) {
        send_stream_error(stream_id, ERR_INVALID, "Stream ID already in use");
        return;
    }

    /* Validate first string (path) is null-terminated within payload */
    path = safe_string(payload, 5, length, &path_end);
    if (!path) {
        send_stream_error(stream_id, ERR_INVALID, "Invalid path (no null terminator)");
        return;
    }

    /* Validate path length doesn't exceed MAX_PATH */
    if (strlen(path) >= MAX_PATH) {
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
            /* path(string) + mode(uint16) */
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
            /* path(string) + newpath(string) - validate second string */
            newpath = safe_string(payload, path_end - payload, length, NULL);
            if (!newpath) {
                send_stream_error(stream_id, ERR_INVALID, "Invalid destination path");
                free_stream(s);
                return;
            }
            if (strlen(newpath) >= MAX_PATH) {
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
            /* path(string) + pattern(string) - validate second string */
            newpath = safe_string(payload, path_end - payload, length, NULL);
            if (!newpath) {
                send_stream_error(stream_id, ERR_INVALID, "Invalid search pattern");
                free_stream(s);
                return;
            }
            handle_file_find(s, path, newpath);
            break;

        case STREAM_FILE_SEARCH:
            /* path(string) + pattern(string) - validate second string */
            newpath = safe_string(payload, path_end - payload, length, NULL);
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

/*
 * Send WINDOW_UPDATE to relay to acknowledge received bytes.
 * Called periodically after receiving data to prevent relay from blocking.
 */
static void send_window_update()
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

static void handle_stream_data(payload, length)
unsigned char *payload;
int length;
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
    /* Other stream types don't receive data from relay */

    /* Acknowledge received data for flow control */
    bytes_to_ack += length;
    send_window_update();
}

static void handle_stream_end(payload, length)
unsigned char *payload;
int length;
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

static void handle_stream_cancel(payload, length)
unsigned char *payload;
int length;
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

static void handle_window_update(payload, length)
unsigned char *payload;
int length;
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
 * Main Loop
 * ============================================================================ */

static void main_loop()
{
    fd_set readfds;
    struct timeval tv;
    int maxfd, n, i;
    int rows, cols;
    char input_buf[256];
    unsigned char *payload;
    int length, type;
    unsigned char resize_buf[4];

    enable_raw_mode();
    signal(SIGWINCH, sigwinch_handler);

    /* Send initial terminal size */
    get_terminal_size(&rows, &cols);
    put_u16(resize_buf, rows);
    put_u16(resize_buf + 2, cols);
    send_packet(PKT_TERM_RESIZE, resize_buf, 4);

    maxfd = sockfd;

    while (1) {
        /* Handle window resize */
        if (got_sigwinch) {
            got_sigwinch = 0;
            get_terminal_size(&rows, &cols);
            put_u16(resize_buf, rows);
            put_u16(resize_buf + 2, cols);
            send_packet(PKT_TERM_RESIZE, resize_buf, 4);
        }

        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        FD_SET(STDIN_FILENO, &readfds);

        /* Also watch exec streams */
        for (i = 0; i < MAX_STREAMS; i++) {
            if (streams[i].state != STREAM_STATE_IDLE &&
                streams[i].type == STREAM_EXEC &&
                streams[i].child_fd >= 0) {
                FD_SET(streams[i].child_fd, &readfds);
                if (streams[i].child_fd > maxfd)
                    maxfd = streams[i].child_fd;
            }
        }

        tv.tv_sec = 0;
        tv.tv_usec = 10000;  /* 10ms - short for responsiveness */

        n = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* Handle terminal input */
        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            n = read(STDIN_FILENO, input_buf, sizeof(input_buf));
            if (n > 0) {
                send_packet(PKT_TERM_INPUT, (unsigned char *)input_buf, n);
            }
        }

        /* Handle exec stream output - poll all active exec streams */
        for (i = 0; i < MAX_STREAMS; i++) {
            if (streams[i].state != STREAM_STATE_IDLE &&
                streams[i].type == STREAM_EXEC &&
                streams[i].child_fd >= 0) {
                if (poll_exec_stream(&streams[i]) < 0) {
                    fprintf(stderr, "\r\nFlow control error\r\n");
                    goto done;
                }
            }
        }

        /* Handle packets from relay - process ALL available packets */
        if (FD_ISSET(sockfd, &readfds) || recv_buf_len > 0) {
            /* Keep processing while we have complete packets */
            while (1) {
                type = recv_packet(&payload, &length);
                if (type < 0) {
                    fprintf(stderr, "\r\nConnection closed\r\n");
                    goto done;
                }
                if (type == 0) break;  /* No complete packet, back to select */

                switch (type) {
                    case PKT_TERM_OUTPUT:
                        if (simple_mode) {
                            int filtered_len = filter_simple(payload, length);
                            write(STDOUT_FILENO, payload, filtered_len);
                        } else {
                            write(STDOUT_FILENO, payload, length);
                        }
                        /* Acknowledge received data for flow control */
                        bytes_to_ack += length;
                        send_window_update();
                        break;

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
            }  /* end while(1) packet processing loop */
        }
    }

done:
    disable_raw_mode();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(argc, argv)
int argc;
char *argv[];
{
    char *host = NULL;
    int port = 0;
    int i;

    /* Initialize stream table */
    for (i = 0; i < MAX_STREAMS; i++) {
        streams[i].state = STREAM_STATE_IDLE;
    }

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--simple") == 0) {
            simple_mode = 1;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--resume") == 0) {
            resume_mode = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            /* Try current directory first, then /tmp */
            logfile = fopen("telepresence-v2.log", "w");
            if (logfile) {
                fprintf(stderr, "Logging to telepresence-v2.log\n");
            } else {
                logfile = fopen("/tmp/telepresence-v2.log", "w");
                if (logfile) {
                    fprintf(stderr, "Logging to /tmp/telepresence-v2.log\n");
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

    /* Allocate receive buffer */
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

    if (sockfd >= 0) close(sockfd);
    if (recv_buf) free(recv_buf);
    if (logfile) fclose(logfile);

    return 0;
}
