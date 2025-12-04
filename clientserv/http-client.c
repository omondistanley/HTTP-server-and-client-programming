#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>

static void die(const char *msg) {
	perror(msg);
	exit(1);
}

int main(int argc, char **argv) {
	if(argc != 4) {
		fprintf(stderr, "usage: http-client <host name> <port number> <file path>\n");
		fprintf(stderr, "   ex) http-client www.example.com 80 /index.html\n");
		exit(1);
	}

	char *servname = argv[1];
	if (servname == NULL || strlen(servname) == 0 || strlen(servname) > 255) {
		fprintf(stderr, "Error: Invalid hostname\n");
		exit(1);
	}
	
	char *port_str = argv[2];
	unsigned short port = atoi(port_str);
	if (port == 0 || port > 65535) {
		fprintf(stderr, "Error: Invalid port number (must be 1-65535)\n");
		exit(1);
	}
	
	char *filepath = argv[3];
	if (filepath == NULL || strlen(filepath) == 0 || strlen(filepath) > 2048) {
		fprintf(stderr, "Error: Invalid file path\n");
		exit(1);
	}
	
	if (strstr(filepath, "..") != NULL) {
		fprintf(stderr, "Error: File path contains invalid characters\n");
		exit(1);
	}

	struct hostent *he;
	if((he = gethostbyname(servname))  == NULL) {
		die("gethostbyname failed");
	}
	char *serverIP = inet_ntoa(*(struct in_addr *) he->h_addr);
	char *filename;
	char *last_slash = strrchr(filepath, '/');
	if (last_slash != NULL && last_slash[1] != '\0') {
		filename = last_slash + 1;
	} else if (last_slash != NULL) {
		filename = "index.html";
	} else {
		filename = filepath;
	}
	int sock;
	if((sock = socket(AF_INET, SOCK_STREAM,0)) < 0) {
		die("sock failed");
	}
	
	struct sockaddr_in servaddr;

	memset(&servaddr, 0, sizeof(servaddr));
	servaddr.sin_family = AF_INET;
	servaddr.sin_addr.s_addr = inet_addr(serverIP);
	servaddr.sin_port = htons(port);

	if(connect(sock, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
	       die("connect failed");
	}

	FILE *file = fdopen(sock, "rb"); 
		if(file == NULL) {
			die("fdopen failed");
		}
	char request[4096];
	int req_len = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s:%d\r\n\r\n", filepath, servname, port);
	if (req_len >= (int)sizeof(request) || req_len < 0) {
		fprintf(stderr, "Error: Request too long\n");
		fclose(file);
		exit(1);
	}
	if (send(sock, request, strlen(request), 0) < 0) {
    		die("send failed");
	}
	char buffer[4096];
	int n;

	char *status = fgets(buffer, sizeof(buffer), file);
	if (status == NULL) {
		fprintf(stderr, "Error: No response from server\n");
		fclose(file);
		exit(1);
	}
	fprintf(stderr, "%s", status);
	
	int status_code = 0;
	if (sscanf(status, "HTTP/%*d.%*d %d", &status_code) != 1) {
		if (sscanf(status, "%*s %d", &status_code) != 1) {
			fprintf(stderr, "Error: Invalid HTTP response format\n");
			fclose(file);
			exit(1);
		}
	}
	
	if (status_code < 200 || status_code >= 300) {
		fprintf(stderr, "Error: HTTP status code %d\n", status_code);
		while((fgets(buffer, sizeof(buffer), file)) != NULL) {
			fprintf(stderr, "%s", buffer);
			int len = strlen(buffer);
			if(len >= 2 && buffer[0]=='\r' && buffer[1] == '\n') break;
			if(len >= 1 && buffer[0] == '\n') break;
		}
		fclose(file);
		exit(1);
	}
	FILE *downloadFile = fopen(filename, "wb");
        if(downloadFile == NULL) {
                die(filename);
        }
	while((fgets(buffer, sizeof(buffer), file)) != NULL) {
		int len = strlen(buffer);
		if((len == 1) && (buffer[0]=='\r' && buffer[1] == '\n') ) {
			break;
		}
		if(len >= 2 && buffer[0]=='\r' && buffer[1] == '\n') {
			break;
		}
		if(len >= 1 && buffer[0] == '\n') {
			break;
		}
	}

	while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		fwrite(buffer, 1, n, downloadFile);
}
	fclose(downloadFile);
	fclose(file);	
	return 0;
}
