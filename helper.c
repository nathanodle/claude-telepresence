/*
 * telepresence-helper - Multi-tool helper for Claude telepresence
 *
 * Usage:
 *   helper <socket> exec <command>           Execute command
 *   helper <socket> read <path>              Read file
 *   helper <socket> write <path>             Write file (content from stdin)
 *   helper <socket> stat <path>              Get file info
 *   helper <socket> lstat <path>             Get file info (no symlink follow)
 *   helper <socket> exists <path>            Check if path exists
 *   helper <socket> access <path> [rwx]      Check file permissions
 *   helper <socket> ls <path>                List directory
 *   helper <socket> mkdir <path>             Create directory
 *   helper <socket> rm <path>                Remove file
 *   helper <socket> mv <old> <new>           Rename/move file
 *   helper <socket> realpath <path>          Resolve path
 *
 * Connects to Unix socket, sends request, parses response, outputs result.
 *
 * Build: gcc -o telepresence-helper helper.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define MAX_RESPONSE (100 * 1024 * 1024)  /* 100MB max response */
#define MAX_INPUT (10 * 1024 * 1024)      /* 10MB max stdin input */

static int request_id = 0;

/* Simple JSON string escaping */
static void json_escape(const char *in, char *out, size_t outlen) {
    size_t i = 0;
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
            /* Skip other control characters */
        } else {
            out[i++] = c;
        }
        in++;
    }
    out[i] = '\0';
}

/* Simple JSON string extraction (handles escapes) */
static char *json_get_string(const char *json, const char *key, char *out, size_t outlen) {
    char search[256];
    char *start, *end;
    size_t len;

    snprintf(search, sizeof(search), "\"%s\":", key);
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
                        default: *w++ = *r++; break;
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

    snprintf(search, sizeof(search), "\"%s\":", key);
    start = strstr(json, search);
    if (!start) return 0;

    start += strlen(search);
    while (*start == ' ' || *start == '\t') start++;

    return strtol(start, NULL, 10);
}

/* Send request to relay and get response */
static char *send_request(const char *socket_path, const char *request) {
    int sock;
    struct sockaddr_un addr;
    char *response;
    size_t response_len = 0;
    size_t response_cap = 65536;
    ssize_t n;
    char buf[65536];

    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        fprintf(stderr, "socket(): %s\n", strerror(errno));
        return NULL;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect(): %s\n", strerror(errno));
        close(sock);
        return NULL;
    }

    size_t req_len = strlen(request);
    if (write(sock, request, req_len) != (ssize_t)req_len ||
        write(sock, "\n", 1) != 1) {
        fprintf(stderr, "write(): %s\n", strerror(errno));
        close(sock);
        return NULL;
    }

    response = malloc(response_cap);
    if (!response) {
        close(sock);
        return NULL;
    }

    while (1) {
        n = read(sock, buf, sizeof(buf));
        if (n <= 0) break;

        while (response_len + n >= response_cap) {
            response_cap *= 2;
            if (response_cap > MAX_RESPONSE) {
                free(response);
                close(sock);
                return NULL;
            }
            response = realloc(response, response_cap);
            if (!response) {
                close(sock);
                return NULL;
            }
        }

        memcpy(response + response_len, buf, n);
        response_len += n;

        if (memchr(buf, '\n', n)) break;
    }

    close(sock);
    response[response_len] = '\0';
    return response;
}

/* Commands */
static int cmd_exec(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    char *stdout_buf;
    char *stderr_buf;
    long status;
    size_t cmd_len;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> exec <command>\n");
        return 1;
    }

    const char *command = argv[0];
    cmd_len = strlen(command);
    escaped = malloc(cmd_len * 2 + 1);
    request = malloc(cmd_len * 2 + 256);
    stdout_buf = malloc(MAX_RESPONSE);
    stderr_buf = malloc(MAX_RESPONSE);

    if (!escaped || !request || !stdout_buf || !stderr_buf) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(command, escaped, cmd_len * 2 + 1);
    snprintf(request, cmd_len * 2 + 256,
             "{\"id\":%d,\"type\":\"cp.exec\",\"params\":{\"command\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) {
        free(stdout_buf);
        free(stderr_buf);
        return 1;
    }

    /* Parse response */
    json_get_string(response, "stdout", stdout_buf, MAX_RESPONSE);
    json_get_string(response, "stderr", stderr_buf, MAX_RESPONSE);
    status = json_get_int(response, "status");

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        free(stdout_buf);
        free(stderr_buf);
        return 1;
    }

    /* Output */
    if (stdout_buf[0]) printf("%s", stdout_buf);
    if (stderr_buf[0]) fprintf(stderr, "%s", stderr_buf);

    free(response);
    free(stdout_buf);
    free(stderr_buf);
    return (int)status;
}

static int cmd_read(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    char *content;
    size_t path_len;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> read <path>\n");
        return 1;
    }

    const char *path = argv[0];
    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);
    content = malloc(MAX_RESPONSE);

    if (!escaped || !request || !content) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.readFile\",\"params\":{\"path\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) {
        free(content);
        return 1;
    }

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        free(content);
        return 1;
    }

    /* Get content - it's in "result" field as a JSON string */
    json_get_string(response, "result", content, MAX_RESPONSE);
    printf("%s", content);

    free(response);
    free(content);
    return 0;
}

static int cmd_write(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped_path;
    char *escaped_data;
    char *input;
    char *response;
    size_t path_len, input_len = 0, input_cap = 65536;
    ssize_t n;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> write <path> (content from stdin)\n");
        return 1;
    }

    const char *path = argv[0];

    /* Read stdin */
    input = malloc(input_cap);
    if (!input) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    while ((n = read(STDIN_FILENO, input + input_len, input_cap - input_len - 1)) > 0) {
        input_len += n;
        if (input_len >= input_cap - 1) {
            input_cap *= 2;
            if (input_cap > MAX_INPUT) {
                fprintf(stderr, "Input too large\n");
                free(input);
                return 1;
            }
            input = realloc(input, input_cap);
            if (!input) {
                fprintf(stderr, "Out of memory\n");
                return 1;
            }
        }
    }
    input[input_len] = '\0';

    path_len = strlen(path);
    escaped_path = malloc(path_len * 2 + 1);
    escaped_data = malloc(input_len * 2 + 1);
    request = malloc(path_len * 2 + input_len * 2 + 512);

    if (!escaped_path || !escaped_data || !request) {
        fprintf(stderr, "Out of memory\n");
        free(input);
        return 1;
    }

    json_escape(path, escaped_path, path_len * 2 + 1);
    json_escape(input, escaped_data, input_len * 2 + 1);

    snprintf(request, path_len * 2 + input_len * 2 + 512,
             "{\"id\":%d,\"type\":\"fs.writeFile\",\"params\":{\"path\":\"%s\",\"data\":\"%s\"}}",
             ++request_id, escaped_path, escaped_data);

    free(input);
    free(escaped_path);
    free(escaped_data);

    response = send_request(socket_path, request);
    free(request);

    if (!response) return 1;

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        return 1;
    }

    free(response);
    return 0;
}

static int cmd_stat(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    size_t path_len;
    long size, mtime, isdir, isfile;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> stat <path>\n");
        return 1;
    }

    const char *path = argv[0];
    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);

    if (!escaped || !request) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.stat\",\"params\":{\"path\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) return 1;

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        return 1;
    }

    /* Parse stat result */
    size = json_get_int(response, "size");
    mtime = json_get_int(response, "mtime");
    isdir = json_get_int(response, "isDirectory");
    isfile = json_get_int(response, "isFile");

    printf("size: %ld\n", size);
    printf("mtime: %ld\n", mtime);
    printf("type: %s\n", isdir ? "directory" : (isfile ? "file" : "other"));

    free(response);
    return 0;
}

static int cmd_exists(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    size_t path_len;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> exists <path>\n");
        return 1;
    }

    const char *path = argv[0];
    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);

    if (!escaped || !request) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.exists\",\"params\":{\"path\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) return 1;

    /* Result is true/false */
    if (strstr(response, "true")) {
        printf("true\n");
        free(response);
        return 0;
    } else {
        printf("false\n");
        free(response);
        return 1;
    }
}

static int cmd_ls(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    char *result;
    size_t path_len;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> ls <path>\n");
        return 1;
    }

    const char *path = argv[0];
    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);
    result = malloc(MAX_RESPONSE);

    if (!escaped || !request || !result) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.readdir\",\"params\":{\"path\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) {
        free(result);
        return 1;
    }

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        free(result);
        return 1;
    }

    /* Result is array - just print the raw result for now */
    /* TODO: Parse JSON array properly */
    json_get_string(response, "result", result, MAX_RESPONSE);
    printf("%s\n", result);

    free(response);
    free(result);
    return 0;
}

static int cmd_mkdir(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    size_t path_len;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> mkdir <path>\n");
        return 1;
    }

    const char *path = argv[0];
    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);

    if (!escaped || !request) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.mkdir\",\"params\":{\"path\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) return 1;

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        return 1;
    }

    free(response);
    return 0;
}

static int cmd_rm(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    size_t path_len;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> rm <path>\n");
        return 1;
    }

    const char *path = argv[0];
    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);

    if (!escaped || !request) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.unlink\",\"params\":{\"path\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) return 1;

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        return 1;
    }

    free(response);
    return 0;
}

static int cmd_mv(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped_old;
    char *escaped_new;
    char *response;
    size_t old_len, new_len;

    if (argc < 2) {
        fprintf(stderr, "Usage: helper <socket> mv <old_path> <new_path>\n");
        return 1;
    }

    const char *old_path = argv[0];
    const char *new_path = argv[1];
    old_len = strlen(old_path);
    new_len = strlen(new_path);
    escaped_old = malloc(old_len * 2 + 1);
    escaped_new = malloc(new_len * 2 + 1);
    request = malloc(old_len * 2 + new_len * 2 + 512);

    if (!escaped_old || !escaped_new || !request) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(old_path, escaped_old, old_len * 2 + 1);
    json_escape(new_path, escaped_new, new_len * 2 + 1);
    snprintf(request, old_len * 2 + new_len * 2 + 512,
             "{\"id\":%d,\"type\":\"fs.rename\",\"params\":{\"oldPath\":\"%s\",\"newPath\":\"%s\"}}",
             ++request_id, escaped_old, escaped_new);

    response = send_request(socket_path, request);
    free(escaped_old);
    free(escaped_new);
    free(request);

    if (!response) return 1;

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        return 1;
    }

    free(response);
    return 0;
}

static int cmd_realpath(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    char *result;
    size_t path_len;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> realpath <path>\n");
        return 1;
    }

    const char *path = argv[0];
    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);
    result = malloc(4096);

    if (!escaped || !request || !result) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.realpath\",\"params\":{\"path\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) {
        free(result);
        return 1;
    }

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        free(result);
        return 1;
    }

    json_get_string(response, "result", result, 4096);
    printf("%s\n", result);

    free(response);
    free(result);
    return 0;
}

static int cmd_lstat(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    size_t path_len;
    long size, mtime, isdir, isfile, islink;

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> lstat <path>\n");
        return 1;
    }

    const char *path = argv[0];
    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);

    if (!escaped || !request) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.lstat\",\"params\":{\"path\":\"%s\"}}",
             ++request_id, escaped);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) return 1;

    /* Check for error */
    if (strstr(response, "\"error\"")) {
        char errbuf[1024];
        json_get_string(response, "error", errbuf, sizeof(errbuf));
        fprintf(stderr, "%s\n", errbuf);
        free(response);
        return 1;
    }

    /* Parse lstat result */
    size = json_get_int(response, "size");
    mtime = json_get_int(response, "mtime");
    isdir = json_get_int(response, "isDirectory");
    isfile = json_get_int(response, "isFile");
    islink = json_get_int(response, "isSymbolicLink");

    printf("size: %ld\n", size);
    printf("mtime: %ld\n", mtime);
    printf("type: %s\n", islink ? "symlink" : (isdir ? "directory" : (isfile ? "file" : "other")));

    free(response);
    return 0;
}

static int cmd_access(const char *socket_path, int argc, char **argv) {
    char *request;
    char *escaped;
    char *response;
    size_t path_len;
    int mode = 0;  /* F_OK by default - just check existence */

    if (argc < 1) {
        fprintf(stderr, "Usage: helper <socket> access <path> [mode]\n");
        fprintf(stderr, "  mode: r=read, w=write, x=execute (default: existence)\n");
        return 1;
    }

    const char *path = argv[0];
    if (argc >= 2) {
        const char *m = argv[1];
        while (*m) {
            if (*m == 'r') mode |= 4;      /* R_OK */
            else if (*m == 'w') mode |= 2; /* W_OK */
            else if (*m == 'x') mode |= 1; /* X_OK */
            m++;
        }
    }

    path_len = strlen(path);
    escaped = malloc(path_len * 2 + 1);
    request = malloc(path_len * 2 + 256);

    if (!escaped || !request) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    json_escape(path, escaped, path_len * 2 + 1);
    snprintf(request, path_len * 2 + 256,
             "{\"id\":%d,\"type\":\"fs.access\",\"params\":{\"path\":\"%s\",\"mode\":%d}}",
             ++request_id, escaped, mode);

    response = send_request(socket_path, request);
    free(escaped);
    free(request);

    if (!response) return 1;

    /* Check for error - means no access */
    if (strstr(response, "\"error\"")) {
        printf("no\n");
        free(response);
        return 1;
    }

    printf("yes\n");
    free(response);
    return 0;
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <socket> <command> [args...]\n", prog);
    fprintf(stderr, "\nCommands:\n");
    fprintf(stderr, "  exec <cmd>           Execute shell command\n");
    fprintf(stderr, "  read <path>          Read file contents\n");
    fprintf(stderr, "  write <path>         Write file (content from stdin)\n");
    fprintf(stderr, "  stat <path>          Get file info\n");
    fprintf(stderr, "  lstat <path>         Get file info (no symlink follow)\n");
    fprintf(stderr, "  exists <path>        Check if path exists\n");
    fprintf(stderr, "  access <path> [rwx]  Check file permissions\n");
    fprintf(stderr, "  ls <path>            List directory\n");
    fprintf(stderr, "  mkdir <path>         Create directory\n");
    fprintf(stderr, "  rm <path>            Remove file\n");
    fprintf(stderr, "  mv <old> <new>       Rename/move file\n");
    fprintf(stderr, "  realpath <path>      Resolve path\n");
}

int main(int argc, char *argv[]) {
    const char *socket_path;
    const char *cmd;

    if (argc < 3) {
        usage(argv[0]);
        return 1;
    }

    socket_path = argv[1];
    cmd = argv[2];

    /* Initialize request ID from PID for uniqueness */
    request_id = getpid() * 1000;

    if (strcmp(cmd, "exec") == 0) {
        return cmd_exec(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "read") == 0) {
        return cmd_read(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "write") == 0) {
        return cmd_write(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "stat") == 0) {
        return cmd_stat(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "lstat") == 0) {
        return cmd_lstat(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "exists") == 0) {
        return cmd_exists(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "access") == 0) {
        return cmd_access(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "ls") == 0) {
        return cmd_ls(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "mkdir") == 0) {
        return cmd_mkdir(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "rm") == 0) {
        return cmd_rm(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "mv") == 0) {
        return cmd_mv(socket_path, argc - 3, argv + 3);
    } else if (strcmp(cmd, "realpath") == 0) {
        return cmd_realpath(socket_path, argc - 3, argv + 3);
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage(argv[0]);
        return 1;
    }
}
