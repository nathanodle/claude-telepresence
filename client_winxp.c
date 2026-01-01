/*
 * claude-telepresence client for Windows XP/2000
 *
 * Windows port of the telepresence client.
 * Connects to relay server, handles terminal I/O, executes operations locally.
 *
 * Written for maximum compatibility with Windows 2000/XP.
 *
 * Build with MinGW:
 *   gcc -o claude-telepresence.exe client_winxp.c -lws2_32
 *
 * Build with Visual C++ 6.0:
 *   cl /nologo client_winxp.c ws2_32.lib
 *
 * Build with Visual Studio 2003/2005:
 *   cl /nologo client_winxp.c ws2_32.lib
 */

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>

/* Windows headers */
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#include <fcntl.h>
#include <direct.h>
#include <sys/types.h>
#include <sys/stat.h>

/* Link with Winsock library */
#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE (10 * 1024 * 1024)  /* 10MB buffer for large files */
#define MAX_PATH_LEN 4096

/* Global state */
static SOCKET sockfd = INVALID_SOCKET;
static HANDLE hStdin = INVALID_HANDLE_VALUE;
static HANDLE hStdout = INVALID_HANDLE_VALUE;
static DWORD orig_console_mode = 0;
static int raw_mode = 0;
static int simple_mode = 0;  /* Filter fancy terminal effects */
static int resume_mode = 0;  /* Resume previous session */
static FILE *logfile = NULL; /* Debug log */

/* Buffers */
static char *recv_buffer = NULL;
static char *send_buffer = NULL;
static char *json_buffer = NULL;
static char *result_buffer = NULL;

/* Spinner state for animated -\|/ */
static int spinner_state = 0;
static const char spinner_chars[] = "-\\|/";

/* ============================================================================
 * Terminal handling for Windows
 * ============================================================================ */

static void disable_raw_mode(void) {
    if (raw_mode && hStdin != INVALID_HANDLE_VALUE) {
        SetConsoleMode(hStdin, orig_console_mode);
        raw_mode = 0;
    }
}

static void enable_raw_mode(void) {
    DWORD mode;

    hStdin = GetStdHandle(STD_INPUT_HANDLE);
    hStdout = GetStdHandle(STD_OUTPUT_HANDLE);

    if (hStdin == INVALID_HANDLE_VALUE) return;

    if (!GetConsoleMode(hStdin, &orig_console_mode)) return;

    /* Disable line input, echo, and processed input */
    mode = orig_console_mode;
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    mode |= ENABLE_WINDOW_INPUT;

    if (!SetConsoleMode(hStdin, mode)) return;

    /* Enable virtual terminal processing for output if available (Windows 10+) */
    /* This is optional - XP doesn't support it but it doesn't hurt to try */
    {
        DWORD out_mode;
        if (GetConsoleMode(hStdout, &out_mode)) {
            out_mode |= 0x0004;  /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */
            SetConsoleMode(hStdout, out_mode);  /* Ignore failure on XP */
        }
    }

    raw_mode = 1;
    atexit(disable_raw_mode);
}

static void get_terminal_size(int *rows, int *cols) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    if (GetConsoleScreenBufferInfo(hStdout, &csbi)) {
        *cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        *rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        return;
    }
    *rows = 24;
    *cols = 80;
}

/* Filter for simple mode: convert Unicode to ASCII and strip color codes */
static size_t filter_terminal_output(const char *in, char *out, size_t outlen) {
    const unsigned char *p = (const unsigned char *)in;
    size_t inlen = strlen(in);
    size_t j = 0;
    size_t i = 0;
    unsigned char c, ch, cmd;
    size_t start;
    int looks_like_sgr, has_question;

    while (i < inlen && j < outlen - 1) {
        c = p[i];

        /* Handle ESC sequences - strip colors, keep everything else */
        if (c == 0x1b && (i + 1) < inlen && p[i + 1] == '[') {
            /* CSI sequence: ESC [ params command */
            start = i;
            i += 2;  /* skip ESC [ */

            looks_like_sgr = 1;
            has_question = 0;

            /* Scan for command letter */
            while (i < inlen && p[i] >= 0x20 && p[i] < 0x40) {
                ch = p[i];
                if (ch == '?') has_question = 1;
                if (ch != ';' && ch != ':' && (ch < '0' || ch > '9')) {
                    looks_like_sgr = 0;
                }
                i++;
            }

            if (i >= inlen) {
                if (looks_like_sgr && !has_question) {
                    break;
                }
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

        /* 3-byte UTF-8: E2 xx xx */
        if (c == 0xE2 && (i + 2) < inlen) {
            unsigned char b1 = p[i + 1];
            unsigned char b2 = p[i + 2];

            /* Box drawing */
            if (b1 == 0x94) {
                if (b2 >= 0x80 && b2 <= 0x84) out[j++] = '-';
                else if (b2 >= 0x82 && b2 <= 0x83) out[j++] = '|';
                else out[j++] = '+';
                i += 3;
                continue;
            }
            if (b1 == 0x95) {
                if (b2 >= 0x90 && b2 <= 0x94) out[j++] = '=';
                else if (b2 >= 0x91 && b2 <= 0x93) out[j++] = '|';
                else out[j++] = '+';
                i += 3;
                continue;
            }

            /* Arrows */
            if (b1 == 0x86) {
                if (b2 == 0x90) out[j++] = '<';
                else if (b2 == 0x91) out[j++] = '^';
                else if (b2 == 0x92) out[j++] = '>';
                else if (b2 == 0x93) out[j++] = 'v';
                else out[j++] = '>';
                i += 3;
                continue;
            }

            /* Geometric shapes */
            if (b1 == 0x96 || b1 == 0x97) {
                if (b1 == 0x97 && b2 == 0x8F) {
                    out[j++] = spinner_chars[spinner_state];
                    spinner_state = (spinner_state + 1) & 3;
                } else {
                    out[j++] = '*';
                }
                i += 3;
                continue;
            }

            /* Checkmarks and X marks */
            if (b1 == 0x9C) {
                if (b2 == 0x93 || b2 == 0x94 || b2 == 0x85) {
                    out[j++] = '+';
                } else if (b2 == 0x97 || b2 == 0x98) {
                    out[j++] = 'x';
                } else if (b2 == 0xA2 || b2 == 0xB3 || b2 == 0xB6 ||
                           b2 == 0xBB || b2 == 0xBD) {
                    out[j++] = spinner_chars[spinner_state];
                    spinner_state = (spinner_state + 1) & 3;
                } else {
                    out[j++] = '*';
                }
                i += 3;
                continue;
            }
            if (b1 == 0x9D) {
                if (b2 == 0x8C) out[j++] = 'x';
                else out[j++] = '*';
                i += 3;
                continue;
            }

            /* General punctuation */
            if (b1 == 0x80) {
                if (b2 == 0xA2) out[j++] = '*';
                else if (b2 == 0xA3) out[j++] = '>';
                else if (b2 >= 0x93 && b2 <= 0x95) out[j++] = '-';
                else if (b2 == 0x98 || b2 == 0x99) out[j++] = '\'';
                else if (b2 == 0x9C || b2 == 0x9D) out[j++] = '"';
                else if (b2 == 0xA6) out[j++] = '.';
                else out[j++] = ' ';
                i += 3;
                continue;
            }

            /* Other E2 sequences */
            out[j++] = '?';
            i += 3;
            continue;
        }

        /* 2-byte UTF-8 */
        if ((c == 0xC2 || c == 0xC3) && (i + 1) < inlen) {
            unsigned char b1 = p[i + 1];
            if (c == 0xC2 && b1 == 0xA0) {
                out[j++] = ' ';
            } else if (c == 0xC2 && b1 == 0xB7) {
                out[j++] = spinner_chars[spinner_state];
                spinner_state = (spinner_state + 1) & 3;
            } else {
                out[j++] = '?';
            }
            i += 2;
            continue;
        }

        /* 4-byte UTF-8: emoji */
        if (c == 0xF0 && (i + 3) < inlen) {
            out[j++] = '*';
            i += 4;
            continue;
        }

        /* Skip other multi-byte */
        if ((c & 0xE0) == 0xC0) { out[j++] = '?'; i += 2; continue; }
        if ((c & 0xF0) == 0xE0) { out[j++] = '?'; i += 3; continue; }
        if ((c & 0xF8) == 0xF0) { out[j++] = '?'; i += 4; continue; }

        i++;
    }

    out[j] = '\0';
    return j;
}

/* ============================================================================
 * Simple JSON helpers
 * ============================================================================ */

static void json_escape_string(const char *in, char *out, size_t outlen) {
    size_t i = 0;
    out[i++] = '"';
    while (*in && i < outlen - 2) {
        unsigned char c = (unsigned char)*in;
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

static char *json_get_string(const char *json, const char *key, char *out, size_t outlen) {
    char search[256];
    char *start, *end;
    size_t len;

    _snprintf(search, sizeof(search), "\"%s\":", key);
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
        {
            char *r = out, *w = out;
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
                            if (r[1] && r[2] && r[3] && r[4]) {
                                char hex[5] = {r[1], r[2], r[3], r[4], 0};
                                unsigned int codepoint = (unsigned int)strtoul(hex, NULL, 16);
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
        }
    } else {
        out[0] = '\0';
    }
    return out;
}

static long json_get_int(const char *json, const char *key) {
    char search[256];
    char *start;

    _snprintf(search, sizeof(search), "\"%s\":", key);
    start = strstr(json, search);
    if (!start) return -1;

    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;

    return atol(start);
}

static int json_get_bool(const char *json, const char *key) {
    char search[256];
    char *start;

    _snprintf(search, sizeof(search), "\"%s\":", key);
    start = strstr(json, search);
    if (!start) return 0;

    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;

    return (strncmp(start, "true", 4) == 0);
}

/* ============================================================================
 * Network functions (Winsock)
 * ============================================================================ */

static int connect_to_relay(const char *host, int port) {
    struct hostent *server;
    struct sockaddr_in serv_addr;
    unsigned long addr;
    WSADATA wsaData;

    /* Initialize Winsock */
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

    /* Try numeric IP first */
    addr = inet_addr(host);
    if (addr != INADDR_NONE) {
        serv_addr.sin_addr.s_addr = addr;
    } else {
        /* Fall back to hostname lookup */
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

    return 0;
}

static int send_message(const char *json) {
    size_t len = strlen(json);
    unsigned char header[4];
    int sent = 0;
    int n;

    /* Big-endian length header */
    header[0] = (unsigned char)((len >> 24) & 0xFF);
    header[1] = (unsigned char)((len >> 16) & 0xFF);
    header[2] = (unsigned char)((len >> 8) & 0xFF);
    header[3] = (unsigned char)(len & 0xFF);

    if (send(sockfd, (char*)header, 4, 0) != 4) return -1;

    while ((size_t)sent < len) {
        n = send(sockfd, json + sent, (int)(len - sent), 0);
        if (n <= 0) return -1;
        sent += n;
    }

    return 0;
}

static int recv_message(char *buf, size_t buflen) {
    unsigned char header[4];
    size_t len;
    int received = 0;
    int n;

    /* Read length header */
    if (recv(sockfd, (char*)header, 4, 0) != 4) return -1;

    len = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
          ((size_t)header[2] << 8) | (size_t)header[3];

    if (len >= buflen) {
        fprintf(stderr, "Message too large: %lu\n", (unsigned long)len);
        return -1;
    }

    while ((size_t)received < len) {
        n = recv(sockfd, buf + received, (int)(len - received), 0);
        if (n <= 0) return -1;
        received += n;
    }
    buf[len] = '\0';

    return (int)len;
}

/* ============================================================================
 * Operation handlers
 * ============================================================================ */

static void handle_fs_readFile(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    FILE *fp;
    long file_len_signed;
    size_t file_len;
    char *content;
    char *encoded;
    size_t enc_len;

    json_get_string(params, "path", path, sizeof(path));

    fp = fopen(path, "rb");
    if (!fp) {
        _snprintf(response, resplen, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
        return;
    }

    fseek(fp, 0, SEEK_END);
    file_len_signed = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    if (file_len_signed < 0 || file_len_signed > 7 * 1024 * 1024) {
        fclose(fp);
        _snprintf(response, resplen, "{\"error\":\"File too large (max 7MB)\"}");
        return;
    }
    file_len = (size_t)file_len_signed;

    content = (char*)malloc(file_len + 1);
    if (!content) {
        fclose(fp);
        _snprintf(response, resplen, "{\"error\":\"Out of memory\"}");
        return;
    }

    fread(content, 1, file_len, fp);
    content[file_len] = '\0';
    fclose(fp);

    /* Base64 encode */
    enc_len = ((file_len + 2) / 3) * 4 + 1;
    encoded = (char*)malloc(enc_len);
    if (!encoded) {
        free(content);
        _snprintf(response, resplen, "{\"error\":\"Out of memory\"}");
        return;
    }

    {
        static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t i, j = 0;
        for (i = 0; i < file_len; i += 3) {
            unsigned int n = ((unsigned char)content[i]) << 16;
            if (i + 1 < file_len) n |= ((unsigned char)content[i+1]) << 8;
            if (i + 2 < file_len) n |= ((unsigned char)content[i+2]);

            encoded[j++] = b64[(n >> 18) & 63];
            encoded[j++] = b64[(n >> 12) & 63];
            encoded[j++] = (i + 1 < file_len) ? b64[(n >> 6) & 63] : '=';
            encoded[j++] = (i + 2 < file_len) ? b64[n & 63] : '=';
        }
        encoded[j] = '\0';
    }

    _snprintf(response, resplen, "{\"result\":\"%s\"}", encoded);

    free(content);
    free(encoded);
}

static void handle_fs_writeFile(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    char *data;
    char *decoded;
    size_t data_len, dec_len;
    int is_buffer;
    FILE *fp;

    json_get_string(params, "path", path, sizeof(path));
    is_buffer = json_get_bool(params, "isBuffer");

    data = (char*)malloc(BUFFER_SIZE);
    if (!data) {
        _snprintf(response, resplen, "{\"error\":\"Out of memory\"}");
        return;
    }
    json_get_string(params, "data", data, BUFFER_SIZE);

    if (is_buffer) {
        /* Base64 decode */
        data_len = strlen(data);
        dec_len = (data_len / 4) * 3;
        decoded = (char*)malloc(dec_len + 1);
        if (!decoded) {
            free(data);
            _snprintf(response, resplen, "{\"error\":\"Out of memory\"}");
            return;
        }

        {
            static const int d[] = {
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
                -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
                -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
                -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1
            };
            size_t i, j = 0;
            for (i = 0; i + 3 < data_len; i += 4) {
                int a = d[(unsigned char)data[i]];
                int b = d[(unsigned char)data[i+1]];
                int c = d[(unsigned char)data[i+2]];
                int e = d[(unsigned char)data[i+3]];
                if (a < 0 || b < 0) break;
                decoded[j++] = (char)((a << 2) | (b >> 4));
                if (c >= 0) decoded[j++] = (char)(((b & 15) << 4) | (c >> 2));
                if (e >= 0) decoded[j++] = (char)(((c & 3) << 6) | e);
            }
            dec_len = j;
        }

        fp = fopen(path, "wb");
        if (!fp) {
            free(data);
            free(decoded);
            _snprintf(response, resplen, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
            return;
        }
        fwrite(decoded, 1, dec_len, fp);
        fclose(fp);
        free(decoded);
    } else {
        fp = fopen(path, "w");
        if (!fp) {
            free(data);
            _snprintf(response, resplen, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
            return;
        }
        fputs(data, fp);
        fclose(fp);
    }

    free(data);
    _snprintf(response, resplen, "{\"result\":true}");
}

static void handle_fs_stat(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    struct _stat st;

    json_get_string(params, "path", path, sizeof(path));

    if (_stat(path, &st) < 0) {
        _snprintf(response, resplen, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
        return;
    }

    _snprintf(response, resplen,
        "{\"result\":{"
        "\"dev\":%lu,\"ino\":%lu,\"mode\":%u,\"nlink\":%u,"
        "\"uid\":0,\"gid\":0,\"rdev\":%lu,\"size\":%lu,"
        "\"blksize\":4096,\"blocks\":%lu,"
        "\"atimeMs\":%lu,\"mtimeMs\":%lu,\"ctimeMs\":%lu,\"birthtimeMs\":%lu,"
        "\"isFile\":%s,\"isDirectory\":%s,\"isSymbolicLink\":false"
        "}}",
        (unsigned long)st.st_dev, (unsigned long)st.st_ino, (unsigned)st.st_mode,
        (unsigned)st.st_nlink,
        (unsigned long)st.st_rdev, (unsigned long)st.st_size,
        (unsigned long)(st.st_size / 512),
        (unsigned long)st.st_atime * 1000, (unsigned long)st.st_mtime * 1000,
        (unsigned long)st.st_ctime * 1000, (unsigned long)st.st_ctime * 1000,
        (st.st_mode & _S_IFREG) ? "true" : "false",
        (st.st_mode & _S_IFDIR) ? "true" : "false"
    );
}

/* Windows doesn't have lstat, so use stat (no symlink distinction) */
static void handle_fs_lstat(const char *params, char *response, size_t resplen) {
    handle_fs_stat(params, response, resplen);
}

static void handle_fs_readdir(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    char search_path[MAX_PATH_LEN + 4];
    WIN32_FIND_DATA ffd;
    HANDLE hFind;
    size_t pos;
    int first = 1;

    json_get_string(params, "path", path, sizeof(path));

    /* Append \* for directory search */
    _snprintf(search_path, sizeof(search_path), "%s\\*", path);

    hFind = FindFirstFile(search_path, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        _snprintf(response, resplen, "{\"error\":\"Cannot open directory\",\"code\":\"ENOENT\"}");
        return;
    }

    pos = _snprintf(response, resplen, "{\"result\":[");

    do {
        char escaped_name[1024];
        char full_path[MAX_PATH_LEN + 256];
        int is_dir;

        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
            continue;

        json_escape_string(ffd.cFileName, escaped_name, sizeof(escaped_name));

        _snprintf(full_path, sizeof(full_path), "%s\\%s", path, ffd.cFileName);
        is_dir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;

        if (pos + 256 >= resplen) break;

        if (!first) response[pos++] = ',';
        first = 0;

        pos += _snprintf(response + pos, resplen - pos,
            "{\"name\":%s,\"isFile\":%s,\"isDirectory\":%s}",
            escaped_name,
            is_dir ? "false" : "true",
            is_dir ? "true" : "false"
        );
    } while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);
    _snprintf(response + pos, resplen - pos, "]}");
}

static void handle_fs_exists(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    DWORD attr;

    json_get_string(params, "path", path, sizeof(path));

    attr = GetFileAttributes(path);
    if (attr != INVALID_FILE_ATTRIBUTES) {
        _snprintf(response, resplen, "{\"result\":true}");
    } else {
        _snprintf(response, resplen, "{\"result\":false}");
    }
}

static void handle_fs_mkdir(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];

    json_get_string(params, "path", path, sizeof(path));

    if (_mkdir(path) < 0 && errno != EEXIST) {
        _snprintf(response, resplen, "{\"error\":\"%s\"}", strerror(errno));
        return;
    }

    _snprintf(response, resplen, "{\"result\":true}");
}

static void handle_fs_unlink(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];

    json_get_string(params, "path", path, sizeof(path));

    if (_unlink(path) < 0) {
        _snprintf(response, resplen, "{\"error\":\"%s\"}", strerror(errno));
        return;
    }

    _snprintf(response, resplen, "{\"result\":true}");
}

static void handle_fs_rename(const char *params, char *response, size_t resplen) {
    char old_path[MAX_PATH_LEN], new_path[MAX_PATH_LEN];

    json_get_string(params, "oldPath", old_path, sizeof(old_path));
    json_get_string(params, "newPath", new_path, sizeof(new_path));

    if (rename(old_path, new_path) < 0) {
        _snprintf(response, resplen, "{\"error\":\"%s\"}", strerror(errno));
        return;
    }

    _snprintf(response, resplen, "{\"result\":true}");
}

static void handle_fs_access(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    int mode;

    json_get_string(params, "path", path, sizeof(path));
    mode = (int)json_get_int(params, "mode");
    if (mode < 0) mode = 0;  /* Existence check */

    if (_access(path, mode) < 0) {
        _snprintf(response, resplen, "{\"error\":\"%s\",\"code\":\"ENOENT\"}", strerror(errno));
        return;
    }

    _snprintf(response, resplen, "{\"result\":true}");
}

static void handle_fs_realpath(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    char resolved[MAX_PATH_LEN];
    char escaped[MAX_PATH_LEN * 2];

    json_get_string(params, "path", path, sizeof(path));

    if (_fullpath(resolved, path, sizeof(resolved)) == NULL) {
        _snprintf(response, resplen, "{\"error\":\"%s\"}", strerror(errno));
        return;
    }

    json_escape_string(resolved, escaped, sizeof(escaped));
    _snprintf(response, resplen, "{\"result\":%s}", escaped);
}

/* Simple wildcard pattern matcher */
static int match_pattern(const char *pattern, const char *str) {
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) return 1;
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
    while (*pattern == '*') pattern++;
    return (*pattern == '\0' && *str == '\0');
}

/* Recursive find helper */
static void find_recursive(const char *dir_path, const char *pattern,
                          char *result, size_t *pos, size_t maxlen, int *count, int max_results,
                          int depth) {
    char search_path[MAX_PATH_LEN + 4];
    WIN32_FIND_DATA ffd;
    HANDLE hFind;
    char full_path[MAX_PATH_LEN + 256];
    char escaped[MAX_PATH_LEN * 2 + 512];

    if (depth > 64) return;
    if (*count >= max_results) return;

    _snprintf(search_path, sizeof(search_path), "%s\\*", dir_path);

    hFind = FindFirstFile(search_path, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (*count >= max_results) break;

        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
            continue;

        _snprintf(full_path, sizeof(full_path), "%s\\%s", dir_path, ffd.cFileName);

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            find_recursive(full_path, pattern, result, pos, maxlen, count, max_results, depth + 1);
        } else {
            if (match_pattern(pattern, ffd.cFileName)) {
                json_escape_string(full_path, escaped, sizeof(escaped));
                if (*pos + strlen(escaped) + 2 < maxlen) {
                    if (*count > 0) result[(*pos)++] = ',';
                    *pos += _snprintf(result + *pos, maxlen - *pos, "%s", escaped);
                    (*count)++;
                }
            }
        }
    } while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);
}

static void handle_fs_find(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    char pattern[256];
    size_t pos;
    int count = 0;

    json_get_string(params, "path", path, sizeof(path));
    json_get_string(params, "pattern", pattern, sizeof(pattern));

    if (!path[0]) strcpy(path, ".");
    if (!pattern[0]) strcpy(pattern, "*");

    pos = _snprintf(response, resplen, "{\"result\":[");
    find_recursive(path, pattern, response, &pos, resplen - 10, &count, 200, 0);
    _snprintf(response + pos, resplen - pos, "]}");
}

/* Static buffers for search */
static char search_line_buf[2048];
static char search_match_buf[8192];
static char search_escaped_buf[16384];

static int skip_directory(const char *name) {
    if (name[0] == '.') return 1;
    if (_stricmp(name, "node_modules") == 0) return 1;
    if (_stricmp(name, "__pycache__") == 0) return 1;
    return 0;
}

static int is_binary_extension(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return 0;
    if (_stricmp(ext, ".exe") == 0 || _stricmp(ext, ".dll") == 0 ||
        _stricmp(ext, ".obj") == 0 || _stricmp(ext, ".lib") == 0 ||
        _stricmp(ext, ".zip") == 0 || _stricmp(ext, ".gz") == 0 ||
        _stricmp(ext, ".jpg") == 0 || _stricmp(ext, ".jpeg") == 0 ||
        _stricmp(ext, ".png") == 0 || _stricmp(ext, ".gif") == 0 ||
        _stricmp(ext, ".pdf") == 0 || _stricmp(ext, ".bin") == 0) {
        return 1;
    }
    return 0;
}

static void search_recursive(const char *dir_path, const char *search_pattern, const char *file_pattern,
                            char *result, size_t *pos, size_t maxlen,
                            int *match_count, int max_matches,
                            int *file_count, int max_files,
                            int depth) {
    char search_path[MAX_PATH_LEN + 4];
    WIN32_FIND_DATA ffd;
    HANDLE hFind;
    char full_path[MAX_PATH_LEN + 256];

    if (depth > 32) return;
    if (*match_count >= max_matches) return;
    if (*file_count >= max_files) return;

    _snprintf(search_path, sizeof(search_path), "%s\\*", dir_path);

    hFind = FindFirstFile(search_path, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (*match_count >= max_matches) break;
        if (*file_count >= max_files) break;

        if (strcmp(ffd.cFileName, ".") == 0 || strcmp(ffd.cFileName, "..") == 0)
            continue;

        _snprintf(full_path, sizeof(full_path), "%s\\%s", dir_path, ffd.cFileName);

        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (skip_directory(ffd.cFileName)) continue;
            search_recursive(full_path, search_pattern, file_pattern, result, pos, maxlen,
                           match_count, max_matches, file_count, max_files, depth + 1);
        } else {
            FILE *fp;
            int line_num;
            char *newline;

            if (file_pattern[0] && !match_pattern(file_pattern, ffd.cFileName)) continue;
            if (is_binary_extension(ffd.cFileName)) continue;

            /* Skip large files */
            if (ffd.nFileSizeLow > 512 * 1024) continue;
            if (ffd.nFileSizeLow == 0) continue;

            (*file_count)++;

            fp = fopen(full_path, "r");
            if (!fp) continue;

            line_num = 0;
            while (fgets(search_line_buf, sizeof(search_line_buf), fp) && *match_count < max_matches) {
                line_num++;
                if (line_num > 5000) break;

                newline = strchr(search_line_buf, '\n');
                if (newline) *newline = '\0';
                newline = strchr(search_line_buf, '\r');
                if (newline) *newline = '\0';

                if (strstr(search_line_buf, search_pattern)) {
                    _snprintf(search_match_buf, sizeof(search_match_buf), "%s:%d:%s",
                            full_path, line_num, search_line_buf);
                    json_escape_string(search_match_buf, search_escaped_buf, sizeof(search_escaped_buf));
                    if (*pos + strlen(search_escaped_buf) + 2 < maxlen) {
                        if (*match_count > 0) result[(*pos)++] = ',';
                        *pos += _snprintf(result + *pos, maxlen - *pos, "%s", search_escaped_buf);
                        (*match_count)++;
                    }
                }
            }

            fclose(fp);
        }
    } while (FindNextFile(hFind, &ffd) != 0);

    FindClose(hFind);
}

static void handle_fs_search(const char *params, char *response, size_t resplen) {
    char path[MAX_PATH_LEN];
    char pattern[512];
    char file_pattern[256];
    size_t pos;
    int count = 0;
    int file_count = 0;

    json_get_string(params, "path", path, sizeof(path));
    json_get_string(params, "pattern", pattern, sizeof(pattern));
    json_get_string(params, "filePattern", file_pattern, sizeof(file_pattern));

    if (!path[0]) strcpy(path, ".");
    if (!pattern[0]) {
        _snprintf(response, resplen, "{\"error\":\"pattern is required\"}");
        return;
    }

    pos = _snprintf(response, resplen, "{\"result\":[");
    search_recursive(path, pattern, file_pattern, response, &pos, resplen - 10, &count, 200, &file_count, 500, 0);
    _snprintf(response + pos, resplen - pos, "]}");
}

static void handle_cp_exec(const char *params, char *response, size_t resplen) {
    char command[MAX_PATH_LEN * 2];
    char wrapped_cmd[MAX_PATH_LEN * 2 + 32];
    char *output;
    char *escaped_output;
    FILE *fp;
    size_t output_len = 0;
    int status;

    json_get_string(params, "command", command, sizeof(command));

    if (logfile) { fprintf(logfile, "[cp.exec] command: %s\n", command); fflush(logfile); }

    output = (char*)malloc(BUFFER_SIZE);
    if (!output) {
        _snprintf(response, resplen, "{\"error\":\"Out of memory\"}");
        return;
    }

    /* On Windows, redirect stderr to stdout */
    _snprintf(wrapped_cmd, sizeof(wrapped_cmd), "%s 2>&1", command);

    fp = _popen(wrapped_cmd, "r");
    if (!fp) {
        free(output);
        _snprintf(response, resplen, "{\"error\":\"%s\",\"status\":-1,\"stdout\":\"\",\"stderr\":\"\"}", strerror(errno));
        return;
    }

    /* Read output */
    {
        size_t max_output = 4 * 1024 * 1024;
        while (output_len < max_output) {
            size_t n = fread(output + output_len, 1, max_output - output_len, fp);
            if (n == 0) break;
            output_len += n;
        }
        output[output_len] = '\0';
    }

    status = _pclose(fp);

    if (logfile) { fprintf(logfile, "[cp.exec] exit status: %d, output_len: %lu\n", status, (unsigned long)output_len); fflush(logfile); }

    escaped_output = (char*)malloc(output_len * 2 + 3);
    if (!escaped_output) {
        free(output);
        _snprintf(response, resplen, "{\"error\":\"Out of memory\"}");
        return;
    }

    json_escape_string(output, escaped_output, output_len * 2 + 3);

    _snprintf(response, resplen, "{\"result\":{\"status\":%d,\"stdout\":%s,\"stderr\":\"\"}}",
             status, escaped_output);

    free(output);
    free(escaped_output);
}

static void handle_request(const char *json, char *response, size_t resplen) {
    char op[64];
    char *params_buf;
    char *params;
    long req_id;
    size_t params_len;

    req_id = json_get_int(json, "id");
    json_get_string(json, "op", op, sizeof(op));

    if (logfile) { fprintf(logfile, "[handle_request] id=%ld op=%s\n", req_id, op); fflush(logfile); }

    params = strstr(json, "\"params\":");
    if (params) {
        params += 9;
        while (*params == ' ') params++;
        params_len = strlen(params);
        params_buf = (char*)malloc(params_len + 1);
        if (!params_buf) {
            _snprintf(response, resplen, "{\"type\":\"response\",\"id\":%ld,\"error\":\"Out of memory\"}", req_id);
            return;
        }
        strncpy(params_buf, params, params_len);
        params_buf[params_len] = '\0';
    } else {
        params_buf = (char*)malloc(3);
        if (!params_buf) {
            _snprintf(response, resplen, "{\"type\":\"response\",\"id\":%ld,\"error\":\"Out of memory\"}", req_id);
            return;
        }
        strcpy(params_buf, "{}");
    }

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
        handle_cp_exec(params_buf, result_buffer, BUFFER_SIZE);
    } else {
        _snprintf(result_buffer, BUFFER_SIZE, "{\"error\":\"Unknown operation: %s\"}", op);
    }

    /* Wrap response with ID and type */
    _snprintf(response, resplen, "{\"type\":\"response\",\"id\":%ld,%s",
             req_id, result_buffer + 1);

    free(params_buf);
}

/* ============================================================================
 * Main loop for Windows
 * ============================================================================ */

static void main_loop(void) {
    int rows, cols;
    int len;
    INPUT_RECORD input_buf[128];
    DWORD events_read;
    char input_str[256];
    int input_len;
    char escaped[512];
    char type[32];
    char *data;
    char *filtered;
    size_t datalen, flen, x;
    fd_set readfds;
    struct timeval tv;
    u_long non_blocking = 1;

    enable_raw_mode();

    /* Make socket non-blocking */
    ioctlsocket(sockfd, FIONBIO, &non_blocking);

    /* Send initial terminal size */
    get_terminal_size(&rows, &cols);
    _snprintf(send_buffer, BUFFER_SIZE, "{\"type\":\"resize\",\"rows\":%d,\"cols\":%d}", rows, cols);
    send_message(send_buffer);

    while (1) {
        /* Check for console input */
        if (WaitForSingleObject(hStdin, 0) == WAIT_OBJECT_0) {
            if (ReadConsoleInput(hStdin, input_buf, 128, &events_read)) {
                DWORD i;
                input_len = 0;

                for (i = 0; i < events_read && input_len < (int)(sizeof(input_str) - 4); i++) {
                    if (input_buf[i].EventType == KEY_EVENT &&
                        input_buf[i].Event.KeyEvent.bKeyDown) {
                        KEY_EVENT_RECORD *key = &input_buf[i].Event.KeyEvent;
                        char ch = key->uChar.AsciiChar;
                        WORD vk = key->wVirtualKeyCode;

                        if (ch != 0) {
                            /* Regular character */
                            input_str[input_len++] = ch;
                        } else {
                            /* Special key - convert to escape sequence */
                            switch (vk) {
                                case VK_UP:
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = 'A';
                                    break;
                                case VK_DOWN:
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = 'B';
                                    break;
                                case VK_RIGHT:
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = 'C';
                                    break;
                                case VK_LEFT:
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = 'D';
                                    break;
                                case VK_HOME:
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = 'H';
                                    break;
                                case VK_END:
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = 'F';
                                    break;
                                case VK_DELETE:
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = '3';
                                    input_str[input_len++] = '~';
                                    break;
                                case VK_PRIOR:  /* Page Up */
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = '5';
                                    input_str[input_len++] = '~';
                                    break;
                                case VK_NEXT:   /* Page Down */
                                    input_str[input_len++] = 0x1b;
                                    input_str[input_len++] = '[';
                                    input_str[input_len++] = '6';
                                    input_str[input_len++] = '~';
                                    break;
                            }
                        }
                    } else if (input_buf[i].EventType == WINDOW_BUFFER_SIZE_EVENT) {
                        /* Terminal resized */
                        get_terminal_size(&rows, &cols);
                        _snprintf(send_buffer, BUFFER_SIZE, "{\"type\":\"resize\",\"rows\":%d,\"cols\":%d}", rows, cols);
                        send_message(send_buffer);
                    }
                }

                if (input_len > 0) {
                    input_str[input_len] = '\0';

                    if (logfile) {
                        int ii;
                        fprintf(logfile, "[INPUT] %d bytes: ", input_len);
                        for (ii = 0; ii < input_len; ii++) {
                            fprintf(logfile, "%02x ", (unsigned char)input_str[ii]);
                        }
                        fprintf(logfile, "\n");
                        fflush(logfile);
                    }

                    json_escape_string(input_str, escaped, sizeof(escaped));
                    _snprintf(send_buffer, BUFFER_SIZE, "{\"type\":\"terminal_input\",\"data\":%s}", escaped);
                    send_message(send_buffer);
                }
            }
        }

        /* Check for socket data */
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        tv.tv_sec = 0;
        tv.tv_usec = 10000;  /* 10ms */

        if (select(0, &readfds, NULL, NULL, &tv) > 0) {
            /* Reset to blocking for recv_message */
            non_blocking = 0;
            ioctlsocket(sockfd, FIONBIO, &non_blocking);

            len = recv_message(recv_buffer, BUFFER_SIZE);

            /* Back to non-blocking */
            non_blocking = 1;
            ioctlsocket(sockfd, FIONBIO, &non_blocking);

            if (len <= 0) {
                fprintf(stderr, "\r\nConnection closed\r\n");
                break;
            }

            json_get_string(recv_buffer, "type", type, sizeof(type));

            if (strcmp(type, "terminal_output") == 0) {
                data = (char*)malloc(len + 1);
                if (data) {
                    DWORD written;
                    json_get_string(recv_buffer, "data", data, len + 1);
                    datalen = strlen(data);

                    if (logfile) {
                        fprintf(logfile, "=== RECV len=%d datalen=%u simple=%d ===\n",
                                len, (unsigned)datalen, simple_mode);
                        fflush(logfile);
                    }

                    if (simple_mode) {
                        filtered = (char*)malloc(len + 1);
                        if (filtered) {
                            flen = filter_terminal_output(data, filtered, len + 1);
                            WriteConsole(hStdout, filtered, (DWORD)flen, &written, NULL);
                            free(filtered);
                        }
                    } else {
                        WriteConsole(hStdout, data, (DWORD)datalen, &written, NULL);
                    }
                    free(data);
                }
            } else if (strcmp(type, "request") == 0) {
                if (logfile) {
                    fprintf(logfile, "=== REQUEST received ===\n");
                    fflush(logfile);
                }
                handle_request(recv_buffer, send_buffer, BUFFER_SIZE);
                send_message(send_buffer);
                if (logfile) {
                    fprintf(logfile, "=== RESPONSE sent ===\n");
                    fflush(logfile);
                }
            }
        }

        Sleep(10);  /* Small delay to prevent CPU spinning */
    }

    disable_raw_mode();
}

/* ============================================================================
 * Main
 * ============================================================================ */

int main(int argc, char *argv[]) {
    const char *host = NULL;
    int port = 0;
    int i;

    /* Parse options */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--simple") == 0) {
            simple_mode = 1;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--resume") == 0) {
            resume_mode = 1;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            logfile = fopen("C:\\telepresence.log", "w");
            if (logfile) {
                fprintf(stderr, "*** Logging enabled: C:\\telepresence.log ***\n");
                fprintf(logfile, "=== Log started ===\n");
                fflush(logfile);
            }
        }
    }

    /* Find host and port */
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
        fprintf(stderr, "  -l, --log      Log to C:\\telepresence.log\n");
        return 1;
    }

    /* Allocate buffers */
    recv_buffer = (char*)malloc(BUFFER_SIZE);
    send_buffer = (char*)malloc(BUFFER_SIZE);
    json_buffer = (char*)malloc(BUFFER_SIZE);
    result_buffer = (char*)malloc(BUFFER_SIZE);

    if (!recv_buffer || !send_buffer || !json_buffer || !result_buffer) {
        fprintf(stderr, "Failed to allocate buffers\n");
        return 1;
    }

    fprintf(stderr, "Connecting to %s:%d (simple=%d, log=%s)...\n",
            host, port, simple_mode, logfile ? "yes" : "no");

    if (connect_to_relay(host, port) < 0) {
        return 1;
    }

    /* Send hello with cwd */
    {
        char cwd[MAX_PATH_LEN];
        char escaped_cwd[MAX_PATH_LEN * 2];
        char *p, *q;

        if (_getcwd(cwd, sizeof(cwd)) == NULL) {
            strcpy(cwd, "C:\\");
        }

        /* Escape cwd for JSON - convert backslashes */
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
            _snprintf(send_buffer, BUFFER_SIZE,
                     "{\"type\":\"hello\",\"cwd\":\"%s\",\"resume\":true}", escaped_cwd);
        } else {
            _snprintf(send_buffer, BUFFER_SIZE,
                     "{\"type\":\"hello\",\"cwd\":\"%s\"}", escaped_cwd);
        }
        send_message(send_buffer);
    }

    fprintf(stderr, "Connected! Starting Claude Code session...\n\n");

    main_loop();

    if (sockfd != INVALID_SOCKET) {
        closesocket(sockfd);
    }
    WSACleanup();

    free(recv_buffer);
    free(send_buffer);
    free(json_buffer);
    free(result_buffer);

    return 0;
}
