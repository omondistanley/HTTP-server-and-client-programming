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

#define MAX_URI_LEN 2048
#define MAX_PATH_LEN 4096
#define MAX_REQUEST_LEN 8192
#define BACKEND_TIMEOUT_SEC 5
#define CLIENT_TIMEOUT_SEC 30

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int url_decode(const char *src, char *dest, size_t dest_size) {
    const char *p = src;
    char *q = dest;
    size_t remaining = dest_size - 1;
    
    while (*p && remaining > 0) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            int val;
            sscanf(p + 1, "%2x", &val);
            *q++ = (char)val;
            p += 3;
            remaining--;
        } else if (*p == '+') {
            *q++ = ' ';
            p++;
            remaining--;
        } else {
            *q++ = *p++;
            remaining--;
        }
    }
    *q = '\0';
    return (q - dest);
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

static int parse_form_data(const char *data, const char *field, char *value, size_t value_size) {
    char field_name[256];
    char field_value[1024];
    const char *p = data;
    
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) break;
        
        size_t name_len = eq - p;
        if (name_len >= sizeof(field_name)) name_len = sizeof(field_name) - 1;
        strncpy(field_name, p, name_len);
        field_name[name_len] = '\0';
        
        p = eq + 1;
        const char *amp = strchr(p, '&');
        size_t val_len;
        if (amp) {
            val_len = amp - p;
        } else {
            val_len = strlen(p);
        }
        if (val_len >= sizeof(field_value)) val_len = sizeof(field_value) - 1;
        strncpy(field_value, p, val_len);
        field_value[val_len] = '\0';
        
        char decoded[1024];
        url_decode(field_value, decoded, sizeof(decoded));
        
        if (strcmp(field_name, field) == 0) {
            strncpy(value, decoded, value_size - 1);
            value[value_size - 1] = '\0';
            return 0;
        }
        
        if (amp) {
            p = amp + 1;
        } else {
            break;
        }
    }
    return -1;
}

static int read_post_body(FILE *fp, char *body, size_t body_size) {
    char line[1024];
    size_t content_length = 0;
    
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, "Content-Length:", 15) == 0) {
            content_length = (size_t)atoi(line + 15);
        }
        if (strcmp(line, "\r\n") == 0 || strcmp(line, "\n") == 0) {
            break;
        }
    }
    
    if (content_length == 0 || content_length >= body_size) {
        return -1;
    }
    
    size_t n = fread(body, 1, content_length, fp);
    body[n] = '\0';
    return (int)n;
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
    char *serverIP;
    char *serverName;
    char *serverPort;
    struct sockaddr_in backendAddr;
};

static int reconnect_backend(struct BackendConnection *backend) {
    if (backend->fp) {
        fclose(backend->fp);
        backend->fp = NULL;
    }
    if (backend->sock >= 0) {
        close(backend->sock);
        backend->sock = -1;
    }
    
    struct hostent *he;
    if((he = gethostbyname(backend->serverName)) == NULL) {
        return -1;
    }
    backend->serverIP = inet_ntoa(*(struct in_addr *)he->h_addr);
    
    if((backend->sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        return -1;
    }
    
    set_socket_timeout(backend->sock, BACKEND_TIMEOUT_SEC);
    
    memset(&backend->backendAddr, 0, sizeof(backend->backendAddr));
    backend->backendAddr.sin_family = AF_INET;
    backend->backendAddr.sin_addr.s_addr = inet_addr(backend->serverIP);
    backend->backendAddr.sin_port = htons(atoi(backend->serverPort));
    
    if(connect(backend->sock, (struct sockaddr *)&backend->backendAddr, sizeof(backend->backendAddr)) < 0) {
        close(backend->sock);
        backend->sock = -1;
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
    
    unsigned short server_port = atoi(argv[1]);
    if (server_port == 0) {
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
    backend_conn.serverPort = argv[4];
    backend_conn.sock = -1;
    backend_conn.fp = NULL;
    backend_conn.serverIP = NULL;
    
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
        
        char buf[MAX_REQUEST_LEN];
        if(fgets(buf, sizeof(buf), fp) == NULL) {
            fclose(fp); 
            close(clntsock);
            continue;
        }
        
        if (strlen(buf) >= MAX_REQUEST_LEN - 1) {
            char header[] = "HTTP/1.0 414 URI Too Long\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>414 URI Too Long</h1></body></html>\n";
            send(clntsock, header, strlen(header), 0);
            fclose(fp);
            close(clntsock);
            continue;
        }
        
        char *requestLine = buf;
        char *token_separators = "\t \r\n";
        char *method = strtok(requestLine, token_separators);
        char *requestURI_full = strtok(NULL, token_separators);
        char *httpVersion = strtok(NULL, token_separators);
        char resp[64] = {0};
        
        char requestURI[MAX_URI_LEN];
        if (requestURI_full) {
            char *query_start = strchr(requestURI_full, '?');
            if (query_start) {
                size_t path_len = query_start - requestURI_full;
                if (path_len >= sizeof(requestURI)) path_len = sizeof(requestURI) - 1;
                strncpy(requestURI, requestURI_full, path_len);
                requestURI[path_len] = '\0';
            } else {
                strncpy(requestURI, requestURI_full, sizeof(requestURI) - 1);
                requestURI[sizeof(requestURI) - 1] = '\0';
            }
        } else {
            requestURI[0] = '\0';
        }

        if (!method || !requestURI || !httpVersion) {
            char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "400 Bad Request");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method ? method : "-", requestURI ? requestURI : "-", httpVersion ? httpVersion : "-", resp);
            fclose(fp); close(clntsock);
            continue;
        }
        
        if (strlen(requestURI) >= MAX_URI_LEN) {
            char header[] = "HTTP/1.0 414 URI Too Long\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>414 URI Too Long</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "414 URI Too Long");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }
        
        int is_get = (strcmp(method, "GET") == 0);
        int is_post = (strcmp(method, "POST") == 0);
        if (!is_get && !is_post) {
            char header[] = "HTTP/1.0 501 Not Implemented\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>501 Not Implemented</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "501 Not Implemented");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }
        if (strncmp(httpVersion, "HTTP/1.0", 8) != 0 && strncmp(httpVersion, "HTTP/1.1", 8) != 0) {
            char header[] = "HTTP/1.0 505 HTTP Version Not Supported\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>505 HTTP Version Not Supported</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "505 HTTP Version Not Supported");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }
        if (requestURI[0] != '/' || forbidden_dotdot(requestURI)) {
            char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "400 Bad Request");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }

        if (is_get && strcmp(requestURI, "/mdb-lookup") == 0) {
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
            fclose(fp); close(clntsock);
            continue;
        }
        else if (is_get && requestURI_full && strncmp(requestURI_full, "/mdb-lookup?key=", 16) == 0) {
            char *encoded_key = requestURI_full + 16;
            char decoded_key[1024];
            url_decode(encoded_key, decoded_key, sizeof(decoded_key));
            
            // Trim leading and trailing whitespace from search key
            char *trimmed_key = decoded_key;
            // Skip leading whitespace
            while (*trimmed_key && isspace((unsigned char)*trimmed_key)) {
                trimmed_key++;
            }
            // Trim trailing whitespace
            char *end = trimmed_key + strlen(trimmed_key) - 1;
            while (end > trimmed_key && isspace((unsigned char)*end)) {
                *end = '\0';
                end--;
            }
            
            if (strlen(trimmed_key) == 0 || strlen(trimmed_key) > 1000) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid key</h1></body></html>\n";
                snprintf(resp, sizeof(resp), "400 Bad Request");
                send(clntsock, header, strlen(header), 0);
                fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                    method, requestURI, httpVersion, resp);
                fclose(fp); close(clntsock);
                continue;
            }

            const char *form =
                "<!DOCTYPE html>\n"
                "<html><head><title>Database Search</title></head><body>\n"
                "<h1>mdb-lookup</h1>\n"
                "<p>\n"
                "<form method=GET action=/mdb-lookup>\n"
                "lookup: <input type=text name=key value=\"";
            char header[4096];
            char escaped_key[1024];
            html_escape(trimmed_key, escaped_key, sizeof(escaped_key));
            snprintf(header, sizeof(header),
                "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n%s%s\">\n"
                "<input type=submit>\n"
                "</form>\n"
                "<p>\n"
                "<a href=\"/mdb-list\">List All Records</a> | <a href=\"/mdb-add\">Add New Record</a>\n"
                "<p>\n"
                "<table border=\"1\" cellpadding=\"5\" cellspacing=\"0\">\n"
                "<tr><th>#</th><th>Record</th></tr>\n", form, escaped_key);
            send(clntsock, header, strlen(header), 0);

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                fprintf(stderr, "Backend connection lost, reconnecting...\n");
                if (reconnect_backend(&backend_conn) < 0) {
                    char error_msg[] = "<tr><td colspan=2>Error: Backend server unavailable</td></tr>\n";
                    send(clntsock, error_msg, strlen(error_msg), 0);
                    send(clntsock, "</table>\n", 9, 0);
                    snprintf(resp, sizeof(resp), "503 Service Unavailable");
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp); close(clntsock);
                    continue;
                }
            }

            if (fprintf(backend_conn.fp, "%s\n", trimmed_key) < 0 || fflush(backend_conn.fp) != 0) {
                fprintf(stderr, "Error writing to backend, reconnecting...\n");
                if (reconnect_backend(&backend_conn) < 0) {
                    char error_msg[] = "<tr><td colspan=2>Error: Backend server unavailable</td></tr>\n";
                    send(clntsock, error_msg, strlen(error_msg), 0);
                    send(clntsock, "</table>\n", 9, 0);
                    snprintf(resp, sizeof(resp), "503 Service Unavailable");
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp); close(clntsock);
                    continue;
                }
                fprintf(backend_conn.fp, "%s\n", trimmed_key);
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
                if (send(clntsock, rowbuf, strlen(rowbuf), 0) < 0) {
                    fprintf(stderr, "Error sending to client\n");
                    break;
                }
                found_any = 1;
            }
            
            // Reset timeout to default after reading
            timeout.tv_sec = BACKEND_TIMEOUT_SEC;
            timeout.tv_usec = 0;
            setsockopt(backend_conn.sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            
            if (ferror(backend_conn.fp)) {
                fprintf(stderr, "Error reading from backend for search key: %s\n", trimmed_key);
            }
            
            if (!found_any) {
                if (got_empty_line) {
                    char not_found_msg[] = "<tr><td colspan=\"2\"><strong>ENTRY NOT FOUND</strong></td></tr>\n";
                    send(clntsock, not_found_msg, strlen(not_found_msg), 0);
                    fprintf(stderr, "Search for '%s' returned no matches\n", trimmed_key);
                } else if (feof(backend_conn.fp)) {
                    char error_msg[] = "<tr><td colspan=\"2\">Error: Database connection closed</td></tr>\n";
                    send(clntsock, error_msg, strlen(error_msg), 0);
                    fprintf(stderr, "Backend connection closed during search for: %s\n", trimmed_key);
                } else {
                    char error_msg[] = "<tr><td colspan=\"2\">Error: No response from database</td></tr>\n";
                    send(clntsock, error_msg, strlen(error_msg), 0);
                    fprintf(stderr, "No response received for search key: %s\n", trimmed_key);
                }
            } else {
                fprintf(stderr, "Search for '%s' returned %d result(s)\n", trimmed_key, row - 1);
            }
            
            send(clntsock, "</table>\n</body></html>\n", 24, 0);
            snprintf(resp, sizeof(resp), "200 OK");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }

        if (is_get && strcmp(requestURI, "/mdb-list") == 0) {
            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp); close(clntsock);
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

            send(clntsock, "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n", 44, 0);
            send(clntsock, html, strlen(html), 0);

            char line[1024];
            while (fgets(line, sizeof(line), backend_conn.fp)) {
                if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) break;
                size_t llen = strlen(line);
                if (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r')) line[--llen] = '\0';
                
                int id;
                char name[16], msg[24];
                const char *p = line;
                while (*p == ' ' || *p == '\t') p++;
                if (sscanf(p, "%d. {%15[^}]},said {%23[^}]}", &id, name, msg) == 3) {
                    char escaped_name[64], escaped_msg[64];
                    html_escape(name, escaped_name, sizeof(escaped_name));
                    html_escape(msg, escaped_msg, sizeof(escaped_msg));
                    
                    char row[512];
                    snprintf(row, sizeof(row),
                        "<tr><td>%d</td><td>%s</td><td>%s</td>"
                        "<td><a href=\"/mdb-edit?id=%d\">Edit</a> | "
                        "<form method=POST action=/mdb-delete style=display:inline>"
                        "<input type=hidden name=id value=%d>"
                        "<input type=submit value=Delete onclick=\"return confirm('Delete this record?')\">"
                        "</form></td></tr>\n",
                        id, escaped_name, escaped_msg, id, id);
                    send(clntsock, row, strlen(row), 0);
                }
            }
            send(clntsock, "</table></body></html>\n", 23, 0);
            snprintf(resp, sizeof(resp), "200 OK");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
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
            fclose(fp); close(clntsock);
            continue;
        }

        if (is_post && strcmp(requestURI, "/mdb-add") == 0) {
            char post_body[4096];
            if (read_post_body(fp, post_body, sizeof(post_body)) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            char name[16], msg[24];
            if (parse_form_data(post_body, "name", name, sizeof(name)) < 0 ||
                parse_form_data(post_body, "msg", msg, sizeof(msg)) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Missing fields</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            if (strlen(name) > 15 || strlen(msg) > 23) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Field too long</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp); close(clntsock);
                    continue;
                }
            }

            fprintf(backend_conn.fp, "ADD %s|%s\n", name, msg);
            fflush(backend_conn.fp);

            char response[256];
            if (fgets(response, sizeof(response), backend_conn.fp) && strncmp(response, "OK", 2) == 0) {
                fprintf(backend_conn.fp, "SAVE\n");
                fflush(backend_conn.fp);
                fgets(response, sizeof(response), backend_conn.fp);

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
            fclose(fp); close(clntsock);
            continue;
        }

        if (is_get && strncmp(requestURI, "/mdb-edit?id=", 13) == 0) {
            int edit_id = atoi(requestURI + 13);
            if (edit_id < 1) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid ID</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp); close(clntsock);
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
                
                int id;
                if (sscanf(line, "%d. {%15[^}]},said {%23[^}]}", &id, name, msg) == 3 && id == edit_id) {
                    found = 1;
                    break;
                }
            }

            if (!found) {
                char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            char escaped_name[64], escaped_msg[64];
            html_escape(name, escaped_name, sizeof(escaped_name));
            html_escape(msg, escaped_msg, sizeof(escaped_msg));

            char form[2048];
            snprintf(form, sizeof(form),
                "<!DOCTYPE html>\n"
                "<html><head><title>Edit Record</title></head><body>\n"
                "<h1>Edit Record #%d</h1>\n"
                "<p><a href=\"/mdb-list\">Back to List</a></p>\n"
                "<form method=POST action=/mdb-update>\n"
                "<input type=hidden name=id value=%d>\n"
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
            fclose(fp); close(clntsock);
            continue;
        }

        if (is_post && strcmp(requestURI, "/mdb-update") == 0) {
            char post_body[4096];
            if (read_post_body(fp, post_body, sizeof(post_body)) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            char id_str[16], name[16], msg[24];
            if (parse_form_data(post_body, "id", id_str, sizeof(id_str)) < 0 ||
                parse_form_data(post_body, "name", name, sizeof(name)) < 0 ||
                parse_form_data(post_body, "msg", msg, sizeof(msg)) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Missing fields</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            int id = atoi(id_str);
            if (id < 1 || strlen(name) > 15 || strlen(msg) > 23) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid data</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp); close(clntsock);
                    continue;
                }
            }

            fprintf(backend_conn.fp, "UPDATE %d|%s|%s\n", id, name, msg);
            fflush(backend_conn.fp);

            char response[256];
            if (fgets(response, sizeof(response), backend_conn.fp) && strncmp(response, "OK", 2) == 0) {
                fprintf(backend_conn.fp, "SAVE\n");
                fflush(backend_conn.fp);
                fgets(response, sizeof(response), backend_conn.fp);

                clearerr(backend_conn.fp);

                char header[] = "HTTP/1.0 302 Found\r\nLocation: /mdb-list\r\n\r\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "302 Found");
            } else {
                char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>404 Not Found: Record not found</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "404 Not Found");
            }
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }

        if (is_post && strcmp(requestURI, "/mdb-delete") == 0) {
            char post_body[4096];
            if (read_post_body(fp, post_body, sizeof(post_body)) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            char id_str[16];
            if (parse_form_data(post_body, "id", id_str, sizeof(id_str)) < 0) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Missing ID</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            int id = atoi(id_str);
            if (id < 1) {
                char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>400 Bad Request: Invalid ID</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                fclose(fp); close(clntsock);
                continue;
            }

            if (backend_conn.fp == NULL || feof(backend_conn.fp) || ferror(backend_conn.fp)) {
                if (reconnect_backend(&backend_conn) < 0) {
                    char header[] = "HTTP/1.0 503 Service Unavailable\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>503 Service Unavailable</h1></body></html>\n";
                    send(clntsock, header, strlen(header), 0);
                    fclose(fp); close(clntsock);
                    continue;
                }
            }

            fprintf(backend_conn.fp, "DELETE %d\n", id);
            fflush(backend_conn.fp);

            char response[256];
            if (fgets(response, sizeof(response), backend_conn.fp) && strncmp(response, "OK", 2) == 0) {
                fprintf(backend_conn.fp, "SAVE\n");
                fflush(backend_conn.fp);
                fgets(response, sizeof(response), backend_conn.fp);

                clearerr(backend_conn.fp);

                char header[] = "HTTP/1.0 302 Found\r\nLocation: /mdb-list\r\n\r\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "302 Found");
            } else {
                char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>404 Not Found: Record not found</h1></body></html>\n";
                send(clntsock, header, strlen(header), 0);
                snprintf(resp, sizeof(resp), "404 Not Found");
            }
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
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
            fclose(fp); close(clntsock);
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
                    fclose(fp); close(clntsock);
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
                    fclose(fp); close(clntsock);
                    continue;
                }
                if (stat(path, &st) != 0) {
                    char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
                    snprintf(resp, sizeof(resp), "404 Not Found");
                    send(clntsock, header, strlen(header), 0);
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp); close(clntsock);
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
                fclose(fp); close(clntsock);
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
                fclose(fp); close(clntsock);
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
            send(clntsock, header, strlen(header), 0);
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), file)) > 0) {
                send(clntsock, buf, n, 0);
            }
            fclose(file);
            snprintf(resp, sizeof(resp), "200 OK");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        } else {
            char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
            snprintf(resp, sizeof(resp), "404 Not Found");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }
    }
    close(servsock);
    if (backend_conn.fp) {
        fclose(backend_conn.fp);
    }
    if (backend_conn.sock >= 0) {
        close(backend_conn.sock);
    }
    return 0;
}