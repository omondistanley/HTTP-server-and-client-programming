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

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static int forbidden_dotdot(const char *uri) {
    if (strstr(uri, "/../") != NULL) return 1;
    size_t len = strlen(uri);
    if (len >= 3 && strcmp(uri + len - 3, "/..") == 0) return 1;
    return 0;
}

int main(int argc, char **argv) {
    char *serverIP;
    int sock;
    struct sockaddr_in serverAddr;
    if(argc != 5) {
        fprintf(stderr, "usage: %s <server_port> <web_root> <mdb-lookup-host> <mdb-lookup-port>\n", argv[0]);
        exit(1);
    }
    unsigned short server_port = atoi(argv[1]);

    // Setup persistent connection to backend mdb-lookup-server
    struct hostent *he;
    char *serverName = argv[3];
    char *serverPort = argv[4];
    if((he = gethostbyname(serverName)) == NULL) {
        die("gethostbyname failed");
    }
    serverIP = inet_ntoa(*(struct in_addr *)he->h_addr);
    int backend;
    struct sockaddr_in backendAddr;
    if((backend = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
        die("socket failed");
    }
    memset(&backendAddr, 0, sizeof(backendAddr));
    backendAddr.sin_family = AF_INET;
    backendAddr.sin_addr.s_addr = inet_addr(serverIP);
    backendAddr.sin_port = htons(atoi(serverPort));
    if(connect(backend, (struct sockaddr *)&backendAddr, sizeof(backendAddr)) < 0) {
        die("connect to backend failed");
    }
    FILE *backend_fp = fdopen(backend, "r+");
    if(!backend_fp) die("fdopen backend failed");

    // Create a listening socket, the server socket.
    int servsock;
    if((servsock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
        die("socket failed");
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
        if ((clntsock = accept(servsock, (struct sockaddr *) &clntaddr, &clntlen)) < 0)
            die("accept failed");
        FILE *fp = fdopen(clntsock, "rb");
        if(fp == NULL) {
            die("fdopen failed");
        }
        char buf[4096];
        if(fgets(buf, sizeof(buf), fp) == NULL) {
            fclose(fp); 
			close(clntsock);
			continue;
        }
        char *requestLine = buf;
        char *token_separators = "\t \r\n";
        char *method = strtok(requestLine, token_separators);
        char *requestURI = strtok(NULL, token_separators);
        char *httpVersion = strtok(NULL, token_separators);
        char resp[4096] = {0};

        // Robust request validation
        if (!method || !requestURI || !httpVersion) {
            char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
            strcpy(resp, "400 Bad Request");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method ? method : "-", requestURI ? requestURI : "-", httpVersion ? httpVersion : "-", resp);
            fclose(fp); close(clntsock);
            continue;
        }
        if (strcmp(method, "GET") != 0) {
            char header[] = "HTTP/1.0 501 Not Implemented\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>501 Not Implemented</h1></body></html>\n";
            strcpy(resp, "501 Not Implemented");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }
        if (strncmp(httpVersion, "HTTP/1.0", 8) != 0 && strncmp(httpVersion, "HTTP/1.1", 8) != 0) {
            char header[] = "HTTP/1.0 501 Not Implemented\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>501 Not Implemented</h1></body></html>\n";
            strcpy(resp, "501 Not Implemented");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }
        if (requestURI[0] != '/' || forbidden_dotdot(requestURI)) {
            char header[] = "HTTP/1.0 400 Bad Request\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>400 Bad Request</h1></body></html>\n";
            strcpy(resp, "400 Bad Request");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }

        // mdb-lookup form
        if (strcmp(requestURI, "/mdb-lookup") == 0) {
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
            strcpy(resp, "200 OK");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }
        else if (strncmp(requestURI, "/mdb-lookup?key=", 16) == 0) {
            // Extract the key from the URI
            char *key = requestURI + 16;
            // NOTE: For a real server, URL decode key here if needed

            // Send the HTML form first
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
                "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n\r\n%s<table border>\n", form);
            send(clntsock, header, strlen(header), 0);

            // Send the key to the backend mdb-lookup-server (persistent connection)
            fprintf(backend_fp, "%s\n", key);
            fflush(backend_fp);

            // Read and format results as HTML table
            char line[1024];
            int row = 1;
            while (fgets(line, sizeof(line), backend_fp)) {
                if (strcmp(line, "\n") == 0 || strcmp(line, "\r\n") == 0) break;
                size_t llen = strlen(line);
                if (llen > 0 && (line[llen-1] == '\n' || line[llen-1] == '\r')) line[--llen] = '\0';
                char rowbuf[1200];
                snprintf(rowbuf, sizeof(rowbuf), "<tr><td>%d</td><td>%s</td></tr>\n", row++, line);
                send(clntsock, rowbuf, strlen(rowbuf), 0);
            }
            send(clntsock, "</table>\n", 9, 0);
            strcpy(resp, "200 OK");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }

        // Serve static files from web root
        char *web_root = argv[2];
        char path[4096];
        snprintf(path, sizeof(path), "%s%s", web_root, requestURI);

        struct stat st;
        if (stat(path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                // If URI doesn't end with '/', send 403
                if (requestURI[strlen(requestURI) - 1] != '/') {
                    char header[] = "HTTP/1.0 403 Forbidden\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>403 Forbidden</h1></body></html>\n";
                    strcpy(resp, "403 Forbidden");
                    send(clntsock, header, strlen(header), 0);
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp); close(clntsock);
                    continue;
                }
                // Append index.html for directory
                strncat(path, "index.html", sizeof(path) - strlen(path) - 1);
                if (stat(path, &st) != 0) {
                    char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                    "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
                    strcpy(resp, "404 Not Found");
                    send(clntsock, header, strlen(header), 0);
                    fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                        method, requestURI, httpVersion, resp);
                    fclose(fp); close(clntsock);
                    continue;
                }
            }
            // Serve file in 4096-byte chunks
            FILE *file = fopen(path, "rb");
            if (!file) {
                char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                                "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
                strcpy(resp, "404 Not Found");
                send(clntsock, header, strlen(header), 0);
                fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                    method, requestURI, httpVersion, resp);
                fclose(fp); close(clntsock);
                continue;
            }
            // Guess content-type (basic)
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
            strcpy(resp, "200 OK");
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        } else {
            char header[] = "HTTP/1.0 404 Not Found\r\nContent-Type: text/html\r\n\r\n"
                            "<!DOCTYPE html><html><body><h1>404 Not Found</h1></body></html>\n";
            strcpy(resp, "404 Not Found");
            send(clntsock, header, strlen(header), 0);
            fprintf(stdout, "%s \"%s %s %s\" %s\n", inet_ntoa(clntaddr.sin_addr),
                method, requestURI, httpVersion, resp);
            fclose(fp); close(clntsock);
            continue;
        }
    }
    close(servsock);
    fclose(backend_fp);
    return 0;
}