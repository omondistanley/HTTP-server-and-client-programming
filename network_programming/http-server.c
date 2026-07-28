#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <ctype.h>
#include <fcntl.h>
#include <time.h>
#include <signal.h>
#include <limits.h>
#include <inttypes.h>
#include <stdint.h>
#include <strings.h>

#define MAX_URI_LEN 2048
#define MAX_PATH_LEN 4096
#define MAX_REQUEST_LEN 8192
#define MAX_HEADER_LINE_LEN 8192
#define MAX_HEADER_BYTES 32768
#define MAX_FORM_BODY_LEN 4096
#define MAX_FORM_VALUE_LEN 4096
#define MAX_NAME_LEN 15
#define MAX_MSG_LEN 23
#define MAX_SEARCH_KEY_LEN 1000
#define BACKEND_TIMEOUT_SEC 5
#define CLIENT_TIMEOUT_SEC 30

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int hex_value(unsigned char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/*
 * Decodes exactly src_len bytes. Malformed escapes, embedded NUL bytes, and
 * output truncation are rejected rather than silently accepted.
 */
static int url_decode_component(
    const char *src,
    size_t src_len,
    char *dest,
    size_t dest_size,
    size_t *decoded_len
) {
    size_t in = 0;
    size_t out = 0;

    if (!src || !dest || dest_size == 0) return -1;

    while (in < src_len) {
        unsigned char value;
        if (src[in] == '%') {
            if (in + 2 >= src_len) return -1;
            int high = hex_value((unsigned char)src[in + 1]);
            int low = hex_value((unsigned char)src[in + 2]);
            if (high < 0 || low < 0) return -1;
            value = (unsigned char)((high << 4) | low);
            in += 3;
        } else {
            value = (src[in] == '+') ? ' ' : (unsigned char)src[in];
            in++;
        }

        if (value == '\0' || out + 1 >= dest_size) return -1;
        dest[out++] = (char)value;
    }

    dest[out] = '\0';
    if (decoded_len) *decoded_len = out;
    return 0;
}

static void html_escape(const char *src, char *dest, size_t dest_size) {
    const char *p = src;
    char *q = dest;
    size_t remaining = dest_size - 1;
    
    while (*p && remaining > 0) {
        if (*p == '<' && remaining >= 4) {
            strncpy(q, "&lt;", 4);
            q += 4;
            remaining -= 4;
        } else if (*p == '>' && remaining >= 4) {
            strncpy(q, "&gt;", 4);
            q += 4;
            remaining -= 4;
        } else if (*p == '&' && remaining >= 5) {
            strncpy(q, "&amp;", 5);
            q += 5;
            remaining -= 5;
        } else if (*p == '"' && remaining >= 6) {
            strncpy(q, "&quot;", 6);
            q += 6;
            remaining -= 6;
        } else if (*p == '\'' && remaining >= 6) {
            strncpy(q, "&#39;", 5);
            q += 5;
            remaining -= 5;
        } else {
            *q++ = *p++;
            remaining--;
        }
    }
    *q = '\0';
}

/*
 * Parses application/x-www-form-urlencoded data and query strings.
 * Returns 0 on success, 1 when the field is absent, and -1 for malformed,
 * duplicate, NUL-containing, or over-capacity values.
 */
static int parse_parameter(
    const char *data,
    const char *field,
    char *value,
    size_t value_size,
    size_t *value_len
) {
    const char *segment;
    size_t field_len;
    int found = 0;

    if (!data || !field || !value || value_size == 0) return -1;
    field_len = strlen(field);
    segment = data;

    while (*segment) {
        const char *end = strchr(segment, '&');
        if (!end) end = segment + strlen(segment);
        const char *eq = memchr(segment, '=', (size_t)(end - segment));

        if (eq && (size_t)(eq - segment) == field_len &&
            strncmp(segment, field, field_len) == 0) {
            if (found) return -1;
            if (url_decode_component(
                    eq + 1,
                    (size_t)(end - (eq + 1)),
                    value,
                    value_size,
                    value_len) < 0) {
                return -1;
            }
            found = 1;
        }

        if (*end == '\0') break;
        segment = end + 1;
    }

    return found ? 0 : 1;
}

static int validate_text_value(
    const char *value,
    size_t value_len,
    size_t max_len,
    int forbid_pipe
) {
    int has_non_space = 0;
    if (!value || value_len == 0 || value_len > max_len) return -1;

    for (size_t i = 0; i < value_len; i++) {
        unsigned char c = (unsigned char)value[i];
        if (c < 32 || c == 127 || (forbid_pipe && c == '|')) return -1;
        if (!isspace(c)) has_non_space = 1;
    }
    return has_non_space ? 0 : -1;
}

static void trim_whitespace(char *value, size_t *value_len) {
    size_t start = 0;
    size_t end = *value_len;

    while (start < end && isspace((unsigned char)value[start])) start++;
    while (end > start && isspace((unsigned char)value[end - 1])) end--;

    if (start > 0 && end > start) {
        memmove(value, value + start, end - start);
    }
    *value_len = end - start;
    value[*value_len] = '\0';
}

static int parse_positive_u64(const char *text, uint64_t *result) {
    char *end;
    uintmax_t value;

    if (!text || !*text || !result) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!isdigit(*p)) return -1;
    }
    errno = 0;
    value = strtoumax(text, &end, 10);
    if (errno != 0 || *end != '\0' || value == 0 || value > UINT64_MAX) {
        return -1;
    }
    *result = (uint64_t)value;
    return 0;
}

static int parse_port(const char *text, unsigned short *result) {
    char *end;
    unsigned long value;

    if (!text || !*text || !result) return -1;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (!isdigit(*p)) return -1;
    }
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < 1 || value > 65535) return -1;
    *result = (unsigned short)value;
    return 0;
}

enum HttpLineResult {
    HTTP_LINE_OK = 0,
    HTTP_LINE_EOF,
    HTTP_LINE_TOO_LONG,
    HTTP_LINE_NUL
};

enum PostReadResult {
    POST_READ_OK = 0,
    POST_READ_BAD_REQUEST,
    POST_READ_LENGTH_REQUIRED,
    POST_READ_PAYLOAD_TOO_LARGE,
    POST_READ_UNSUPPORTED_MEDIA_TYPE,
    POST_READ_UNSUPPORTED_TRANSFER_ENCODING,
    POST_READ_HEADERS_TOO_LARGE
};

/*
 * Reads one HTTP line without relying on strlen() to detect its end. This is
 * important for rejecting raw NUL bytes instead of treating the bytes after
 * them as a separate, invisible part of the request.
 */
static enum HttpLineResult read_http_line(
    FILE *fp,
    char *line,
    size_t line_size,
    size_t max_wire_length,
    size_t *wire_length
) {
    size_t used = 0;
    size_t total = 0;
    int c;

    if (!fp || !line || line_size == 0) return HTTP_LINE_EOF;

    while ((c = fgetc(fp)) != EOF) {
        total++;
        if (c == '\0') return HTTP_LINE_NUL;
        if (total > max_wire_length || used + 1 >= line_size) {
            return HTTP_LINE_TOO_LONG;
        }
        if (c == '\n') {
            line[used] = '\0';
            if (wire_length) *wire_length = total;
            return HTTP_LINE_OK;
        }
        line[used++] = (char)c;
    }

    if (wire_length) *wire_length = total;
    return HTTP_LINE_EOF;
}

static int is_header_name_char(unsigned char c) {
    if (isalnum(c)) return 1;
    return strchr("!#$%&'*+-.^_`|~", c) != NULL;
}

static int parse_content_length_value(const char *value, size_t *result) {
    const unsigned char *p = (const unsigned char *)value;
    size_t parsed = 0;
    int saw_digit = 0;

    if (!value || !result) return -1;
    while (*p == ' ' || *p == '\t') p++;

    while (isdigit(*p)) {
        unsigned int digit = (unsigned int)(*p - '0');
        if (parsed > (SIZE_MAX - digit) / 10) return -1;
        parsed = parsed * 10 + digit;
        saw_digit = 1;
        p++;
    }

    while (*p == ' ' || *p == '\t') p++;
    if (!saw_digit || *p != '\0') return -1;
    *result = parsed;
    return 0;
}

static int is_form_content_type(const char *value) {
    static const char expected[] = "application/x-www-form-urlencoded";
    const char *start = value;
    const char *media_end;
    const char *end;

    if (!value) return 0;
    while (*start == ' ' || *start == '\t') start++;
    end = start + strlen(start);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) end--;

    media_end = memchr(start, ';', (size_t)(end - start));
    if (!media_end) media_end = end;
    while (media_end > start &&
           (media_end[-1] == ' ' || media_end[-1] == '\t')) {
        media_end--;
    }

    return (size_t)(media_end - start) == sizeof(expected) - 1 &&
           strncasecmp(start, expected, sizeof(expected) - 1) == 0;
}

static enum PostReadResult read_post_body(
    FILE *fp,
    char *body,
    size_t body_size,
    size_t *body_length
) {
    char line[MAX_HEADER_LINE_LEN + 1];
    size_t total_header_bytes = 0;
    size_t content_length = 0;
    int saw_content_length = 0;
    int saw_content_type = 0;
    int valid_content_type = 0;
    int saw_transfer_encoding = 0;
    int reached_end_of_headers = 0;

    if (!fp || !body || body_size < MAX_FORM_BODY_LEN + 1) {
        return POST_READ_BAD_REQUEST;
    }

    while (!reached_end_of_headers) {
        size_t wire_length = 0;
        enum HttpLineResult line_result = read_http_line(
            fp,
            line,
            sizeof(line),
            MAX_HEADER_LINE_LEN,
            &wire_length);

        if (line_result == HTTP_LINE_TOO_LONG) {
            return POST_READ_HEADERS_TOO_LARGE;
        }
        if (line_result != HTTP_LINE_OK) {
            return POST_READ_BAD_REQUEST;
        }
        if (wire_length > MAX_HEADER_BYTES - total_header_bytes) {
            return POST_READ_HEADERS_TOO_LARGE;
        }
        total_header_bytes += wire_length;

        size_t line_length = strlen(line);
        if (line_length > 0 && line[line_length - 1] == '\r') {
            line[--line_length] = '\0';
        }
        if (memchr(line, '\r', line_length) != NULL) {
            return POST_READ_BAD_REQUEST;
        }
        if (line_length == 0) {
            reached_end_of_headers = 1;
            break;
        }
        if (line[0] == ' ' || line[0] == '\t') {
            return POST_READ_BAD_REQUEST;
        }

        char *colon = strchr(line, ':');
        if (!colon || colon == line) return POST_READ_BAD_REQUEST;
        for (char *p = line; p < colon; p++) {
            if (!is_header_name_char((unsigned char)*p)) {
                return POST_READ_BAD_REQUEST;
            }
        }

        *colon = '\0';
        char *value = colon + 1;
        while (*value == ' ' || *value == '\t') value++;
        char *value_end = value + strlen(value);
        while (value_end > value &&
               (value_end[-1] == ' ' || value_end[-1] == '\t')) {
            *--value_end = '\0';
        }
        for (char *p = value; *p; p++) {
            unsigned char c = (unsigned char)*p;
            if ((c < 32 && c != '\t') || c == 127) {
                return POST_READ_BAD_REQUEST;
            }
        }

        if (strcasecmp(line, "Content-Length") == 0) {
            if (saw_content_length ||
                parse_content_length_value(value, &content_length) < 0) {
                return POST_READ_BAD_REQUEST;
            }
            saw_content_length = 1;
        } else if (strcasecmp(line, "Content-Type") == 0) {
            if (saw_content_type) return POST_READ_BAD_REQUEST;
            saw_content_type = 1;
            valid_content_type = is_form_content_type(value);
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            saw_transfer_encoding = 1;
        }
    }

    if (saw_transfer_encoding && saw_content_length) {
        return POST_READ_BAD_REQUEST;
    }
    if (saw_transfer_encoding) {
        return POST_READ_UNSUPPORTED_TRANSFER_ENCODING;
    }
    if (!saw_content_length) return POST_READ_LENGTH_REQUIRED;
    if (content_length > MAX_FORM_BODY_LEN) {
        return POST_READ_PAYLOAD_TOO_LARGE;
    }

    size_t received = 0;
    while (received < content_length) {
        size_t n = fread(body + received, 1, content_length - received, fp);
        if (n == 0) return POST_READ_BAD_REQUEST;
        received += n;
    }
    if (!saw_content_type || !valid_content_type) {
        return POST_READ_UNSUPPORTED_MEDIA_TYPE;
    }
    if (memchr(body, '\0', received) != NULL) {
        return POST_READ_BAD_REQUEST;
    }
    body[received] = '\0';
    if (body_length) *body_length = received;
    return POST_READ_OK;
}

static ssize_t send_all(int sock, const void *buffer, size_t length, int flags) {
    const unsigned char *cursor = (const unsigned char *)buffer;
    size_t sent = 0;

    while (sent < length) {
        ssize_t n = send(sock, cursor + sent, length - sent, flags);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return -1;
    }
    return (ssize_t)sent;
}

/*
 * All response writes in this file use the complete-write semantics above.
 * Keeping the call signature identical prevents accidental raw send() calls.
 */
#define send(sock, buffer, length, flags) send_all((sock), (buffer), (length), (flags))

static int send_error_page(
    int sock,
    const char *status,
    const char *extra_headers
) {
    char response[1024];
    int length = snprintf(
        response,
        sizeof(response),
        "HTTP/1.0 %s\r\n"
        "Content-Type: text/html\r\n"
        "%s"
        "Connection: close\r\n"
        "\r\n"
        "<!DOCTYPE html><html><body><h1>%s</h1></body></html>\n",
        status,
        extra_headers ? extra_headers : "",
        status);

    if (length < 0 || (size_t)length >= sizeof(response)) return -1;
    return send(sock, response, (size_t)length, 0) < 0 ? -1 : 0;
}

static const char *allowed_db_methods(const char *path) {
    if (strcmp(path, "/mdb-lookup") == 0 ||
        strcmp(path, "/mdb-list") == 0 ||
        strcmp(path, "/mdb-edit") == 0) {
        return "GET";
    }
    if (strcmp(path, "/mdb-add") == 0) return "GET, POST";
    if (strcmp(path, "/mdb-update") == 0 ||
        strcmp(path, "/mdb-delete") == 0) {
        return "POST";
    }
    return NULL;
}

static int db_method_is_allowed(const char *allowed, const char *method) {
    if (!allowed || !method) return 0;
    if (strcmp(method, "GET") == 0) {
        return strcmp(allowed, "GET") == 0 ||
               strcmp(allowed, "GET, POST") == 0;
    }
    if (strcmp(method, "POST") == 0) {
        return strcmp(allowed, "POST") == 0 ||
               strcmp(allowed, "GET, POST") == 0;
    }
    return 0;
}

static const char *post_read_status(enum PostReadResult result) {
    switch (result) {
        case POST_READ_BAD_REQUEST:
            return "400 Bad Request";
        case POST_READ_LENGTH_REQUIRED:
            return "411 Length Required";
        case POST_READ_PAYLOAD_TOO_LARGE:
            return "413 Payload Too Large";
        case POST_READ_UNSUPPORTED_MEDIA_TYPE:
            return "415 Unsupported Media Type";
        case POST_READ_UNSUPPORTED_TRANSFER_ENCODING:
            return "501 Not Implemented";
        case POST_READ_HEADERS_TOO_LARGE:
            return "431 Request Header Fields Too Large";
        case POST_READ_OK:
            break;
    }
    return "500 Internal Server Error";
}

static int set_socket_timeout(int sock, int seconds) {
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        return -1;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        return -1;
    }
    return 0;
}

static int forbidden_dotdot(const char *uri) {
    if (strstr(uri, "/../") != NULL) return 1;
    size_t len = strlen(uri);
    if (len >= 3 && strcmp(uri + len - 3, "/..") == 0) return 1;
    if (strstr(uri, "..") != NULL) return 1;
    if (strlen(uri) != len) return 1;
    for (size_t i = 0; i < len; i++) {
        if (uri[i] < 32 && uri[i] != '\t' && uri[i] != '\r' && uri[i] != '\n') {
            return 1;
        }
    }
    return 0;
}

static int validate_file_path(const char *web_root, const char *uri, char *path, size_t path_size) {
    if (uri[0] != '/') {
        return -1;
    }
    
    int ret = snprintf(path, path_size, "%s%s", web_root, uri);
    if (ret < 0 || ret >= (int)path_size) {
        return -1;
    }
    
    if (strstr(path, "/../") != NULL || strstr(path, "..") != NULL) {
        return -1;
    }
    
    return 0;
}

struct BackendConnection {
    int sock;
    FILE *fp;
    char *serverName;
    unsigned short serverPort;
};

static void close_backend(struct BackendConnection *backend) {
    if (backend->fp) {
        fclose(backend->fp);
        backend->fp = NULL;
        backend->sock = -1;
    } else if (backend->sock >= 0) {
        close(backend->sock);
        backend->sock = -1;
    }
}

static int reconnect_backend(struct BackendConnection *backend) {
    struct addrinfo hints;
    struct addrinfo *addresses = NULL;
    struct addrinfo *address;
    char service[6];
    int connected = 0;

    close_backend(backend);

    if (snprintf(
            service,
            sizeof(service),
            "%u",
            (unsigned int)backend->serverPort) < 0) {
        return -1;
    }

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(
            backend->serverName,
            service,
            &hints,
            &addresses) != 0) {
        errno = EHOSTUNREACH;
        return -1;
    }

    for (address = addresses; address; address = address->ai_next) {
        backend->sock = socket(
            address->ai_family,
            address->ai_socktype,
            address->ai_protocol);
        if (backend->sock < 0) {
            continue;
        }

        if (set_socket_timeout(
                backend->sock,
                BACKEND_TIMEOUT_SEC) == 0 &&
            connect(
                backend->sock,
                address->ai_addr,
                address->ai_addrlen) == 0) {
            connected = 1;
            break;
        }

        close(backend->sock);
        backend->sock = -1;
    }
    freeaddrinfo(addresses);

    if (!connected) {
        errno = ECONNREFUSED;
        return -1;
    }
    
    backend->fp = fdopen(backend->sock, "r+");
    if(!backend->fp) {
        close(backend->sock);
        backend->sock = -1;
        return -1;
    }
    
    setvbuf(backend->fp, NULL, _IONBF, 0);
    
    return 0;
}

int main(int argc, char **argv) {
    if(argc != 5) {
        fprintf(stderr, "usage: %s <server_port> <web_root> <mdb-lookup-host> <mdb-lookup-port>\n", argv[0]);
        exit(1);
    }
    
    if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
        die("signal failed");
    }

    unsigned short server_port;
    if (parse_port(argv[1], &server_port) < 0) {
        fprintf(stderr, "Error: Invalid server port\n");
        exit(1);
    }
    
    char *web_root = argv[2];
    if (strlen(web_root) == 0 || strlen(web_root) > MAX_PATH_LEN) {
        fprintf(stderr, "Error: Invalid web root path\n");
        exit(1);
    }

    struct BackendConnection backend_conn;
    backend_conn.serverName = argv[3];
    backend_conn.sock = -1;
    backend_conn.fp = NULL;

    if (parse_port(argv[4], &backend_conn.serverPort) < 0) {
        fprintf(stderr, "Error: Invalid backend port\n");
        exit(1);
    }
    
    if (reconnect_backend(&backend_conn) < 0) {
        die("connect to backend failed");
    }

    int servsock;
    if((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        die("socket failed");
    }
    
    int opt = 1;
    if (setsockopt(servsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        die("setsockopt failed");
    }
    
    struct sockaddr_in servAddr;
    memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servAddr.sin_port = htons(server_port);
    if(bind(servsock, (struct sockaddr *) &servAddr, sizeof(servAddr)) < 0) {
        die("bind failed");
    }
    if(listen(servsock, 5) < 0) {
        die("listen failed, too many requests");
    }

    int clntsock;
    socklen_t clntlen;
    struct sockaddr_in clntaddr;
    while(1) {
        clntlen = sizeof(clntaddr);
        if ((clntsock = accept(servsock, (struct sockaddr *) &clntaddr, &clntlen)) < 0) {
            fprintf(stderr, "accept failed, continuing\n");
            continue;
        }
        
        set_socket_timeout(clntsock, CLIENT_TIMEOUT_SEC);
        
        FILE *fp = fdopen(clntsock, "rb");
        if(fp == NULL) {
            close(clntsock);
            continue;
        }
        
        char buf[MAX_REQUEST_LEN + 1];
        size_t request_line_wire_length = 0;
        enum HttpLineResult request_line_result = read_http_line(
            fp,
            buf,
            sizeof(buf),
            MAX_REQUEST_LEN,
            &request_line_wire_length);

        if (request_line_result != HTTP_LINE_OK) {
            if (request_line_result == HTTP_LINE_TOO_LONG) {
                send_error_page(clntsock, "414 URI Too Long", NULL);
            } else if (request_line_result == HTTP_LINE_NUL ||
                       request_line_wire_length > 0) {
                send_error_page(clntsock, "400 Bad Request", NULL);
            }
            fclose(fp);
            continue;
        }

        char *requestLine = buf;
        char *token_separators = "\t \r\n";
        char *method = strtok(requestLine, token_separators);
        char *requestURI_full = strtok(NULL, token_separators);
        char *httpVersion = strtok(NULL, token_separators);
        char *extra_token = strtok(NULL, token_separators);
        char resp[64] = {0};
        
        char requestURI[MAX_URI_LEN] = {0};
        char queryString[MAX_URI_LEN] = {0};

        if (!method || !requestURI_full || !httpVersion || extra_token) {
            char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "400 Bad Request");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method ? method : "-", requestURI_full ? requestURI_full : "-",
                httpVersion ? httpVersion : "-", resp);
            fclose(fp);
            continue;
        }

        if (strlen(requestURI_full) >= MAX_URI_LEN) {
            char header[] = "HTTP/1.0 414 URI Too Long\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>414 URI Too Long</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "414 URI Too Long");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI_full, httpVersion, resp);
            fclose(fp);
            continue;
        }

        {
            char *query_start = strchr(requestURI_full, '?');
            if (query_start) {
                size_t path_len = query_start - requestURI_full;
                memcpy(requestURI, requestURI_full, path_len);
                requestURI[path_len] = '\0';
                strncpy(queryString, query_start + 1, sizeof(queryString) - 1);
            } else {
                strncpy(requestURI, requestURI_full, sizeof(requestURI) - 1);
            }
        }

        int is_get = (strcmp(method, "GET") == 0);
        int is_post = (strcmp(method, "POST") == 0);
        if (strcmp(httpVersion, "HTTP/1.0") != 0 &&
            strcmp(httpVersion, "HTTP/1.1") != 0) {
            char header[] = "HTTP/1.0 505 HTTP Version Not Supported\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>505 HTTP Version Not Supported</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "505 HTTP Version Not Supported");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        if (requestURI[0] != '/' || forbidden_dotdot(requestURI)) {
            char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "400 Bad Request");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        const char *allowed_methods = allowed_db_methods(requestURI);
        if (!is_get && !is_post) {
            if (allowed_methods) {
                char allow_header[64];
                snprintf(
                    allow_header,
                    sizeof(allow_header),
                    "Allow: %s\r\n",
                    allowed_methods);
                snprintf(resp, sizeof(resp), "405 Method Not Allowed");
                send_error_page(
                    clntsock,
                    "405 Method Not Allowed",
                    allow_header);
            } else {
                snprintf(resp, sizeof(resp), "501 Not Implemented");
                send_error_page(clntsock, "501 Not Implemented", NULL);
            }
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        if (allowed_methods &&
            !db_method_is_allowed(allowed_methods, method)) {
            char allow_header[64];
            snprintf(
                allow_header,
                sizeof(allow_header),
                "Allow: %s\r\n",
                allowed_methods);
            snprintf(resp, sizeof(resp), "405 Method Not Allowed");
            send_error_page(
                clntsock,
                "405 Method Not Allowed",
                allow_header);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        /*
         * Every non-database path falls through to static-file handling, which
         * is deliberately GET-only.
         */
        if (!allowed_methods && is_post) {
            snprintf(resp, sizeof(resp), "405 Method Not Allowed");
            send_error_page(
                clntsock,
                "405 Method Not Allowed",
                "Allow: GET\r\n");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        char post_body[MAX_FORM_BODY_LEN + 1] = {0};
        if (is_post) {
            enum PostReadResult post_result = read_post_body(
                fp,
                post_body,
                sizeof(post_body),
                NULL);
            if (post_result != POST_READ_OK) {
                const char *status = post_read_status(post_result);
                snprintf(resp, sizeof(resp), "%s", status);
                send_error_page(clntsock, status, NULL);
                fprintf(stdout, "%s \"%s %s %s\" %s\n",
                    inet_ntoa(clntaddr.sin_addr),
                    method,
                    requestURI,
                    httpVersion,
                    resp);
                fclose(fp);
                continue;
            }
        }

        if (is_get && strcmp(requestURI, "/mdb-lookup") == 0 && queryString[0] == '\0') {
            const char *form =
                "<!DOCTYPE html>\n"
                "<h1>mdb-lookup</h1>\n"
                "<p>\n"
                "<form method=GET action=/mdb-lookup>\n"
                "lookup: <input type=text name=key>\n"
                "<input type=submit>\n"
                "</form>\n"
                "<p>\n";
            char header[4096];
            snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n%s", form);
            snprintf(resp, sizeof(resp), "200 OK");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }
        else if (is_get && strcmp(requestURI, "/mdb-lookup") == 0) {
            char decoded_key[MAX_FORM_VALUE_LEN];
            size_t key_len = 0;
            int key_result = parse_parameter(
                queryString, "key", decoded_key, sizeof(decoded_key), &key_len);

            if (key_result != 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Missing or malformed key</h1></body></html>\n";
                snprintf(resp, sizeof(resp), "400 Bad Request");
                send(clntsock, header, strlen(header), 0);
                fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                    method, requestURI, httpVersion, resp);
                fclose(fp);
                continue;
            }

            trim_whitespace(decoded_key, &key_len);
            if (validate_text_value(
                    decoded_key, key_len, MAX_SEARCH_KEY_LEN, 0) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid key</h1></body></html>\n";
                snprintf(resp, sizeof(resp), "400 Bad Request");
                send(clntsock, header, strlen(header), 0);
                fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                    method, requestURI, httpVersion, resp);
                fclose(fp);
                continue;
            }

            const char *form =
                "<!DOCTYPE html>\n"
                "<html><head><title>Database Search</title></head><body>\n"
                "<h1>mdb-lookup</h1>\n"
                "<p>\n"
                "<form method=GET action=/mdb-lookup>\n"
                "lookup: <input type=text name=key value=\"";
            char header[8192];
            char escaped_key[6001];
            html_escape(decoded_key, escaped_key, sizeof(escaped_key));
            snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n%s%s\">\n"
                "<input type=submit>\n"
                "</form>\n"
                "<p>\n"
                "<a href=\"/mdb-list\">List All Records</a> | <a href=\"/mdb-add\">Add New Record</a>\n"
                "<p>\n"
                "<table border=\"1\" cellpadding=\"5\" cellspacing=\"0\">\n"
                "<tr><th>#</th><th>Record</th></tr>\n", form, escaped_key);
            if (send(clntsock, header, strlen(header), 0) < 0) {
                fclose(fp);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                fprintf(stderr, "Backend connection lost, reconnecting...\n");
                if (reconnect_backend(&backend_conn) < 0) {
                    char error_msg[] = "<tr><td colspan=2>Error: Backend server unavailable</td></tr>\n";
                    send(clntsock, error_msg, strlen(error_msg), 0);
                    send(clntsock, "</table>\n", 9, 0);
                    snprintf(resp, sizeof(resp), "503 Service Unavailable");
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp);
                    continue;
                }
            }

            if (fprintf(backend_conn.fp, "SEARCH %s\n", decoded_key) < 0 ||
                fflush(backend_conn.fp) != 0) {
                fprintf(stderr, "Error writing to backend, reconnecting...\n");
                if (reconnect_backend(&backend_conn) < 0) {
                    char error_msg[] = "<tr><td colspan=2>Error: Backend server unavailable</td></tr>\n";
                    send(clntsock, error_msg, strlen(error_msg), 0);
                    send(clntsock, "</table>\n", 9, 0);
                    snprintf(resp, sizeof(resp), "503 Service Unavailable");
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp);
                    continue;
                }
                fprintf(backend_conn.fp, "SEARCH %s\n", decoded_key);
                fflush(backend_conn.fp);
            }

            clearerr(backend_conn.fp);
            
            // Set receive timeout to prevent indefinite blocking
            struct timeval timeout;
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;
            setsockopt(backend_conn.sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

            char line[1024];
            int row = 1;
            int found_any = 0;
            int got_empty_line = 0;
            int client_write_ok = 1;
            
            // Read response from backend
            while (fgets(line, sizeof(line), backend_conn.fp)) {
                size_t llen = strlen(line);
                
                while (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r')) {
                    line[--llen] = '\0';
                }
                
                if (llen == 0) {
                    got_empty_line = 1;
                    break;
                }
                
                char escaped_line[1200];
                html_escape(line, escaped_line, sizeof(escaped_line));
                char rowbuf[1400];
                snprintf(rowbuf, sizeof(rowbuf), "<tr><td>%d</td><td>%s</td></tr>\n", row++, escaped_line);
                if (client_write_ok &&
                    send(clntsock, rowbuf, strlen(rowbuf), 0) < 0) {
                    fprintf(stderr, "Error sending to client\n");
                    client_write_ok = 0;
                }
                found_any = 1;
            }
            
            // Reset timeout to default after reading
            timeout.tv_sec = BACKEND_TIMEOUT_SEC;
            timeout.tv_usec = 0;
            setsockopt(backend_conn.sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            
            if (ferror(backend_conn.fp)) {
                fprintf(stderr, "Error reading from backend for search key: %s\n", decoded_key);
            }
            
            if (!found_any) {
                if (got_empty_line) {
                    char not_found_msg[] = "<tr><td colspan=\"2\"><strong>ENTRY NOT FOUND</strong></td></tr>\n";
                    if (client_write_ok) {
                        send(clntsock, not_found_msg, strlen(not_found_msg), 0);
                    }
                    fprintf(stderr, "Search for '%s' returned no matches\n", decoded_key);
                } else if (feof(backend_conn.fp)) {
                    char error_msg[] = "<tr><td colspan=\"2\">Error: Database connection closed</td></tr>\n";
                    if (client_write_ok) {
                        send(clntsock, error_msg, strlen(error_msg), 0);
                    }
                    fprintf(stderr, "Backend connection closed during search for: %s\n", decoded_key);
                } else {
                    char error_msg[] = "<tr><td colspan=\"2\">Error: No response from database</td></tr>\n";
                    if (client_write_ok) {
                        send(clntsock, error_msg, strlen(error_msg), 0);
                    }
                    fprintf(stderr, "No response received for search key: %s\n", decoded_key);
                }
            } else {
                fprintf(stderr, "Search for '%s' returned %d result(s)\n", decoded_key, row - 1);
            }
            
            if (client_write_ok) {
                const char closing[] = "</table>\n</body></html>\n";
                send(clntsock, closing, sizeof(closing) - 1, 0);
            }
            snprintf(resp, sizeof(resp), "200 OK");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        if (is_get && strcmp(requestURI, "/mdb-list") == 0) {
            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp);
                    continue;
                }
            }

            fprintf(backend_conn.fp, "LIST\n");
            fflush(backend_conn.fp);

            char html[8192];
            snprintf(html, sizeof(html),
                "<!DOCTYPE html>\n"
                "<html><head><title>Database Records</title></head><body>\n"
                "<h1>All Database Records</h1>\n"
                "<p><a href=\"/mdb-lookup\">Search</a> | <a href=\"/mdb-add\">Add New</a></p>\n"
                "<table border=\"1\">\n"
                "<tr><th>ID</th><th>Name</th><th>Message</th><th>Actions</th></tr>\n");

            int client_write_ok =
                send(clntsock,
                    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n",
                    44,
                    0) >= 0;
            if (client_write_ok && send(clntsock, html, strlen(html), 0) < 0) {
                client_write_ok = 0;
            }

            char line[1024];
            while (fgets(line, sizeof(line), backend_conn.fp)) {
                if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) break;
                size_t llen = strlen(line);
                if (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r')) line[--llen] = '\0';
                
                uint64_t id;
                char name[16], msg[24];
                const char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (sscanf(
                        p,
                        "%" SCNu64 ". {%15[^}]},said {%23[^}]}",
                        &id,
                        name,
                        msg) == 3) {
                    char escaped_name[64], escaped_msg[64];
                    html_escape(name, escaped_name, sizeof(escaped_name));
                    html_escape(msg, escaped_msg, sizeof(escaped_msg));
                    
                    char row[512];
                    snprintf(row, sizeof(row),
                        "<tr><td>%" PRIu64 "</td><td>%s</td><td>%s</td>"
                        "<td><a href=\"/mdb-edit?id=%" PRIu64 "\">Edit</a> | "
                        "<form method=POST action=/mdb-delete style=display:inline>"
                        "<input type=hidden name=id value=%" PRIu64 ">"
                        "<input type=submit value=Delete onclick=\"return confirm('Delete this record?')\">"
                        "</form></td></tr>\n",
                        id, escaped_name, escaped_msg, id, id);
                    if (client_write_ok &&
                        send(clntsock, row, strlen(row), 0) < 0) {
                        client_write_ok = 0;
                    }
                }
            }
            if (client_write_ok) {
                const char closing[] = "</table></body></html>\n";
                send(clntsock, closing, sizeof(closing) - 1, 0);
            }
            snprintf(resp, sizeof(resp), "200 OK");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        if (is_get && strcmp(requestURI, "/mdb-add") == 0) {
            const char *form =
                "<!DOCTYPE html>\n"
                "<html><head><title>Add Record</title></head><body>\n"
                "<h1>Add New Record</h1>\n"
                "<p><a href=\"/mdb-lookup\">Search</a> | <a href=\"/mdb-list\">List All</a></p>\n"
                "<form method=POST action=/mdb-add>\n"
                "Name (max 15 chars): <input type=text name=name maxlength=15 required><br><br>\n"
                "Message (max 23 chars): <input type=text name=msg maxlength=23 required><br><br>\n"
                "<input type=submit value=Add>\n"
                "</form>\n"
                "</body></html>\n";
            char header[4096];
            snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n%s", form);
            snprintf(resp, sizeof(resp), "200 OK");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        if (is_post && strcmp(requestURI, "/mdb-add") == 0) {
            char name[MAX_FORM_VALUE_LEN], msg[MAX_FORM_VALUE_LEN];
            size_t name_len = 0, msg_len = 0;
            if (parse_parameter(
                    post_body, "name", name, sizeof(name), &name_len) != 0 ||
                parse_parameter(
                    post_body, "msg", msg, sizeof(msg), &msg_len) != 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Missing or malformed fields</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp);
                continue;
            }

            if (validate_text_value(name, name_len, MAX_NAME_LEN, 1) < 0 ||
                validate_text_value(msg, msg_len, MAX_MSG_LEN, 1) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid fields</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp);
                    continue;
                }
            }

            fprintf(backend_conn.fp, "ADD %s|%s\n", name, msg);
            fflush(backend_conn.fp);

            char response[256] = {0};
            if (fgets(response, sizeof(response), backend_conn.fp) && strncmp(response, "OK", 2) == 0) {
                clearerr(backend_conn.fp);

                char header[] = "HTTP/1.0 302 Found\r\nLocation: /mdb-list\r\n\r\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "302 Found");
            } else {
                char header[] = "HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>500 Error: Failed to add record</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "500 Internal Server Error");
            }
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        if (is_get && strcmp(requestURI, "/mdb-edit") == 0) {
            char id_text[64];
            size_t id_len = 0;
            uint64_t edit_id;
            if (parse_parameter(
                    queryString, "id", id_text, sizeof(id_text), &id_len) != 0 ||
                parse_positive_u64(id_text, &edit_id) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid ID</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp);
                    continue;
                }
            }

            fprintf(backend_conn.fp, "LIST\n");
            fflush(backend_conn.fp);

            char line[1024];
            char name[16] = "", msg[24] = "";
            int found = 0;
            while (fgets(line, sizeof(line), backend_conn.fp)) {
                if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) break;
                size_t llen = strlen(line);
                if (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r')) line[--llen] = '\0';
                
                uint64_t id;
                char candidate_name[16], candidate_msg[24];
                if (sscanf(
                        line,
                        "%" SCNu64 ". {%15[^}]},said {%23[^}]}",
                        &id,
                        candidate_name,
                        candidate_msg) == 3 && id == edit_id) {
                    strcpy(name, candidate_name);
                    strcpy(msg, candidate_msg);
                    found = 1;
                }
            }

            if (!found) {
                char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp);
                continue;
            }

            char escaped_name[64], escaped_msg[64];
            html_escape(name, escaped_name, sizeof(escaped_name));
            html_escape(msg, escaped_msg, sizeof(escaped_msg));

            char form[2048];
            snprintf(form, sizeof(form),
                "<!DOCTYPE html>\n"
                "<html><head><title>Edit Record</title></head><body>\n"
                "<h1>Edit Record #%" PRIu64 "</h1>\n"
                "<p><a href=\"/mdb-list\">Back to List</a></p>\n"
                "<form method=POST action=/mdb-update>\n"
                "<input type=hidden name=id value=%" PRIu64 ">\n"
                "Name (max 15 chars): <input type=text name=name value=\"%s\" maxlength=15 required><br><br>\n"
                "Message (max 23 chars): <input type=text name=msg value=\"%s\" maxlength=23 required><br><br>\n"
                "<input type=submit value=Update>\n"
                "</form>\n"
                "</body></html>\n",
                edit_id, edit_id, escaped_name, escaped_msg);
            char header[4096];
            snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n%s", form);
            snprintf(resp, sizeof(resp), "200 OK");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        if (is_post && strcmp(requestURI, "/mdb-update") == 0) {
            char id_str[64], name[MAX_FORM_VALUE_LEN], msg[MAX_FORM_VALUE_LEN];
            size_t id_len = 0, name_len = 0, msg_len = 0;
            if (parse_parameter(
                    post_body, "id", id_str, sizeof(id_str), &id_len) != 0 ||
                parse_parameter(
                    post_body, "name", name, sizeof(name), &name_len) != 0 ||
                parse_parameter(
                    post_body, "msg", msg, sizeof(msg), &msg_len) != 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Missing or malformed fields</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp);
                continue;
            }

            uint64_t id;
            if (parse_positive_u64(id_str, &id) < 0 ||
                validate_text_value(name, name_len, MAX_NAME_LEN, 1) < 0 ||
                validate_text_value(msg, msg_len, MAX_MSG_LEN, 1) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid data</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp);
                    continue;
                }
            }

            fprintf(
                backend_conn.fp,
                "UPDATE %" PRIu64 "|%s|%s\n",
                id,
                name,
                msg);
            fflush(backend_conn.fp);

            char response[256] = {0};
            if (fgets(response, sizeof(response), backend_conn.fp) && strncmp(response, "OK", 2) == 0) {
                clearerr(backend_conn.fp);

                char header[] = "HTTP/1.0 302 Found\r\nLocation: /mdb-list\r\n\r\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "302 Found");
            } else if (strstr(response, "Record not found") != NULL) {
                char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>404 Not Found: Record not found</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "404 Not Found");
            } else {
                char header[] = "HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>500 Internal Server Error: Update was not persisted</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "500 Internal Server Error");
            }
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        if (is_post && strcmp(requestURI, "/mdb-delete") == 0) {
            char id_str[64];
            size_t id_len = 0;
            if (parse_parameter(
                    post_body, "id", id_str, sizeof(id_str), &id_len) != 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Missing or malformed ID</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp);
                continue;
            }

            uint64_t id;
            if (parse_positive_u64(id_str, &id) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid ID</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp);
                    continue;
                }
            }

            fprintf(backend_conn.fp, "DELETE %" PRIu64 "\n", id);
            fflush(backend_conn.fp);

            char response[256] = {0};
            if (fgets(response, sizeof(response), backend_conn.fp) && strncmp(response, "OK", 2) == 0) {
                clearerr(backend_conn.fp);

                char header[] = "HTTP/1.0 302 Found\r\nLocation: /mdb-list\r\n\r\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "302 Found");
            } else if (strstr(response, "Record not found") != NULL) {
                char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>404 Not Found: Record not found</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "404 Not Found");
            } else {
                char header[] = "HTTP/1.0 500 Internal Server Error\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>500 Internal Server Error: Delete was not persisted</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "500 Internal Server Error");
            }
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        char path[MAX_PATH_LEN];
        if (validate_file_path(web_root, requestURI, path, sizeof(path)) < 0) {
            char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid path</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "400 Bad Request");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (requestURI[strlen(requestURI) - 1] != '/') {
                    char header[] = "HTTP/1.0 403 Forbidden\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>403 Forbidden</h1></body></html>\n";
                    snprintf(resp, sizeof(resp), "403 Forbidden");
                    send(clntsock, header, strlen(header), 0);
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp);
                    continue;
                }
                size_t current_len = strlen(path);
                if (current_len + 11 < sizeof(path)) {
                    strncat(path, "index.html", sizeof(path) - current_len - 1);
                } else {
                    char header[] = "HTTP/1.0 414 URI Too Long\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>414 URI Too Long</h1></body></html>\n";
                    snprintf(resp, sizeof(resp), "414 URI Too Long");
                    send(clntsock, header, strlen(header), 0);
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp);
                    continue;
                }
                if (stat(path, &st) != 0) {
                    char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
                    snprintf(resp, sizeof(resp), "404 Not Found");
                    send(clntsock, header, strlen(header), 0);
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp);
                    continue;
                }
            }
            if (st.st_size > 100 * 1024 * 1024) {
                char header[] = "HTTP/1.0 413 Payload Too Large\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>413 Payload Too Large</h1></body></html>\n";
                snprintf(resp, sizeof(resp), "413 Payload Too Large");
                send(clntsock, header, strlen(header), 0);
                fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                    method, requestURI, httpVersion, resp);
                fclose(fp);
                continue;
            }
            
            FILE *file = fopen(path, "rb");
            if (!file) {
                char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
                snprintf(resp, sizeof(resp), "404 Not Found");
                send(clntsock, header, strlen(header), 0);
                fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                    method, requestURI, httpVersion, resp);
                fclose(fp);
                continue;
            }
            const char *ctype = "application/octet-stream";
            if (strstr(path, ".html")) ctype = "text/html";
            else if (strstr(path, ".jpg")) ctype = "image/jpeg";
            else if (strstr(path, ".png")) ctype = "image/png";
            else if (strstr(path, ".gif")) ctype = "image/gif";
            char header[4096];
            snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n",
                ctype, (size_t)st.st_size);
            int client_write_ok =
                send(clntsock, header, strlen(header), 0) >= 0;
            size_t n;
            char file_buffer[MAX_REQUEST_LEN];
            while (client_write_ok &&
                   (n = fread(file_buffer, 1, sizeof(file_buffer), file)) > 0) {
                if (send(clntsock, file_buffer, n, 0) < 0) {
                    client_write_ok = 0;
                }
            }
            fclose(file);
            snprintf(resp, sizeof(resp), "200 OK");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        } else {
            char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "404 Not Found");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp);
            continue;
        }
    }
    close(servsock);
    close_backend(&backend_conn);
    return 0;
}
