/*
 * claude-telepresence client v2
 *
 * Binary streaming protocol client for legacy Unix systems.
 * Written in K&R C for maximum portability.
 *
 * Build:
 *   HP-UX (K&R):  cc -o claude-telepresence client_v2.c
 *   HP-UX (ANSI): cc -Aa -o claude-telepresence client_v2.c
 *   Solaris:      cc -o claude-telepresence client_v2.c -lsocket -lnsl
 *   IRIX/AIX:     cc -o claude-telepresence client_v2.c
 *   Linux:        gcc -o claude-telepresence client_v2.c
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
#define MAX_PACKET_SIZE     (16 * 1024 * 1024)  /* 16 MB */
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
        if (n <= 0) return -1;
        sent += n;
    }

    /* Send payload */
    if (length > 0 && payload != NULL) {
        sent = 0;
        while (sent < length) {
            n = write(sockfd, payload + sent, length - sent);
            if (n <= 0) return -1;
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

    /* Stream file contents in chunks */
    put_u32(buf, s->id);
    while ((n = fread(buf + 4, 1, CHUNK_SIZE, s->file_fp)) > 0) {
        if (send_packet(PKT_STREAM_DATA, buf, 4 + n) < 0) {
            free_stream(s);
            return;
        }
        /* TODO: Check flow control window */
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
        send_packet(PKT_STREAM_DATA, buf, 26);
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
        send_packet(PKT_STREAM_DATA, buf, 26);
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

    send_packet(PKT_STREAM_DATA, buf, 5);
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
    } else {
        strcpy((char *)buf + 4, resolved);
        send_packet(PKT_STREAM_DATA, buf, 4 + strlen(resolved) + 1);
        send_stream_end(s->id, STATUS_OK);
    }
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
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

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

        send_packet(PKT_STREAM_DATA, buf, 21 + namelen + 1);
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
 * Returns 1 if stream still active, 0 if done.
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
        send_packet(PKT_STREAM_DATA, buf, 5 + n);
        return 1;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        /* No data available, check if child still running */
        if (waitpid(s->child_pid, &status, WNOHANG) == 0) {
            return 1;  /* Still running */
        }
        /* Child exited, drain any remaining output */
        while ((n = read(s->child_fd, buf + 5, SMALL_CHUNK)) > 0) {
            send_packet(PKT_STREAM_DATA, buf, 5 + n);
        }
    } else {
        /* n == 0 (EOF) or error, wait for child */
        waitpid(s->child_pid, &status, 0);
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

static void handle_stream_open(payload, length)
unsigned char *payload;
int length;
{
    unsigned long stream_id;
    int stream_type;
    struct stream *s;
    char *path, *newpath;
    int mode;

    if (length < 5) return;

    stream_id = get_u32(payload);
    stream_type = payload[4];

    /* Check for duplicate stream ID */
    if (find_stream(stream_id)) {
        send_stream_error(stream_id, ERR_INVALID, "Stream ID already in use");
        return;
    }

    s = alloc_stream(stream_id);
    if (!s) {
        send_stream_error(stream_id, ERR_NO_MEMORY, "Too many streams");
        return;
    }
    s->type = stream_type;

    path = (char *)payload + 5;

    switch (stream_type) {
        case STREAM_FILE_READ:
            handle_file_read(s, path);
            break;

        case STREAM_FILE_WRITE:
            /* path(string) + mode(uint16) */
            mode = 0;
            {
                char *end = path + strlen(path) + 1;
                if (end + 2 <= (char *)payload + length) {
                    mode = get_u16((unsigned char *)end);
                }
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
            newpath = path + strlen(path) + 1;
            handle_move(s, path, newpath);
            break;

        case STREAM_REALPATH:
            handle_realpath(s, path);
            break;

        case STREAM_FILE_FIND:
        case STREAM_FILE_SEARCH:
            /* TODO: Implement find/search */
            send_stream_error(stream_id, ERR_INVALID, "Not implemented");
            free_stream(s);
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

        /* Handle exec stream output */
        for (i = 0; i < MAX_STREAMS; i++) {
            if (streams[i].state != STREAM_STATE_IDLE &&
                streams[i].type == STREAM_EXEC &&
                streams[i].child_fd >= 0 &&
                FD_ISSET(streams[i].child_fd, &readfds)) {
                poll_exec_stream(&streams[i]);
            }
        }

        /* Also poll exec streams that might have exited */
        for (i = 0; i < MAX_STREAMS; i++) {
            if (streams[i].state != STREAM_STATE_IDLE &&
                streams[i].type == STREAM_EXEC &&
                streams[i].child_fd >= 0) {
                poll_exec_stream(&streams[i]);
            }
        }

        /* Handle packets from relay */
        if (FD_ISSET(sockfd, &readfds)) {
            type = recv_packet(&payload, &length);
            if (type < 0) {
                fprintf(stderr, "\r\nConnection closed\r\n");
                break;
            }
            if (type == 0) continue;

            switch (type) {
                case PKT_TERM_OUTPUT:
                    write(STDOUT_FILENO, payload, length);
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
            logfile = fopen("/tmp/telepresence-v2.log", "w");
            if (logfile) {
                fprintf(stderr, "Logging to /tmp/telepresence-v2.log\n");
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
