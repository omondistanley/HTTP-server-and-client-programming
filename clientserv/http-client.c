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
	}

	struct hostent *he;
	char *servname = argv[1];
	if((he = gethostbyname(servname))  == NULL) {
		die("gethostbyname failed");
	}
	//Converting the host name to an IP address. 
	char *serverIP = inet_ntoa(*(struct in_addr *) he->h_addr);
	unsigned short port = atoi(argv[2]);
	char * filepath = argv[3];
	//Picking out the filename from the filepath provided by the user. Filename comes after the last backslash.
	char *filename = strrchr(filepath, '/') + 1;
	int sock;
	//Creating the socket listening on any IP with a two-way connection
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
	//The request using the HTTP protocol
	char request[4096];
	sprintf(request, "GET %s HTTP/1.0\r\nHost: %s:%d\r\n\r\n", filepath, servname, port);
	//fprintf(stderr,"%s", request);
	// Send the HTTP request
	if (send(sock, request, strlen(request), 0) < 0) {
    		die("send failed");
	}
	// Receive and print the response
	char buffer[4096];
	int n;

	//call fgets once to get the first line of the buferr.
	char *status = fgets(buffer, sizeof(buffer), file);
	fprintf(stderr, "%s\n", status);
	if(strstr(status, "OK") == NULL){
		fprintf(stderr, "%s\n", status);
		fclose(file);
		exit(1);
	}
	FILE *downloadFile = fopen(filename, "wb");
        if(downloadFile == NULL) {
                die(filename);
        }
	// Getting rid of the header lines
	while((fgets(buffer, sizeof(buffer), file)) != NULL) {
		int len = strlen(buffer);
		if((len = 1) && (buffer[0]=='\r' && buffer[1] == '\n') ) {
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
