#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdint.h>

static void die(const char *msg) {
	perror(msg);
	exit(1);
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

static ssize_t send_all(int socket_fd, const void *buffer, size_t length) {
	const unsigned char *cursor = (const unsigned char *)buffer;
	size_t sent = 0;

	while (sent < length) {
		ssize_t n = send(socket_fd, cursor + sent, length - sent, 0);
		if (n > 0) {
			sent += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR) continue;
		return -1;
	}
	return (ssize_t)sent;
}

static int parse_content_length(const char *value, uint64_t *result) {
	const unsigned char *cursor = (const unsigned char *)value;
	uint64_t parsed = 0;
	int saw_digit = 0;

	if (!value || !result) return -1;
	while (*cursor == ' ' || *cursor == '\t') cursor++;
	while (isdigit(*cursor)) {
		unsigned int digit = (unsigned int)(*cursor - '0');
		if (parsed > (UINT64_MAX - digit) / 10) return -1;
		parsed = parsed * 10 + digit;
		saw_digit = 1;
		cursor++;
	}
	if (!saw_digit) return -1;
	while (*cursor == ' ' || *cursor == '\t') cursor++;
	if (*cursor == '\r') cursor++;
	if (*cursor == '\n') cursor++;
	if (*cursor != '\0') return -1;

	*result = parsed;
	return 0;
}

static void remove_temporary_download(const char *path) {
	if (unlink(path) < 0 && errno != ENOENT) {
		fprintf(stderr, "Warning: Cannot remove temporary download %s: %s\n",
			path, strerror(errno));
	}
}

int main(int argc, char **argv) {
	if(argc != 4) {
		fprintf(stderr, "usage: http-client <host name> <port number> <file path>\n");
		fprintf(stderr, "   ex) http-client www.example.com 80 /index.html\n");
		exit(1);
	}

	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR) {
		die("signal failed");
	}

	char *servname = argv[1];
	if (servname == NULL || strlen(servname) == 0 || strlen(servname) > 255) {
		fprintf(stderr, "Error: Invalid hostname\n");
		exit(1);
	}
	
	char *port_str = argv[2];
	unsigned short port;
	if (parse_port(port_str, &port) < 0) {
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

	char *filename;
	char *last_slash = strrchr(filepath, '/');
	if (last_slash != NULL && last_slash[1] != '\0') {
		filename = last_slash + 1;
	} else if (last_slash != NULL) {
		filename = "index.html";
	} else {
		filename = filepath;
	}

	struct stat destination_status;
	if (lstat(filename, &destination_status) == 0) {
		fprintf(stderr, "Error: Destination file already exists: %s\n", filename);
		return 1;
	}
	if (errno != ENOENT) {
		perror("Error: Cannot inspect destination");
		return 1;
	}

	struct addrinfo hints;
	struct addrinfo *addresses = NULL;
	struct addrinfo *address;
	char service[6];
	int sock = -1;
	int last_connect_error = ECONNREFUSED;

	snprintf(service, sizeof(service), "%u", (unsigned int)port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	int resolver_error = getaddrinfo(servname, service, &hints, &addresses);
	if (resolver_error != 0) {
		fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(resolver_error));
		exit(1);
	}

	for (address = addresses; address != NULL; address = address->ai_next) {
		sock = socket(
			address->ai_family,
			address->ai_socktype,
			address->ai_protocol);
		if (sock < 0) {
			last_connect_error = errno;
			continue;
		}
		if (connect(sock, address->ai_addr, address->ai_addrlen) == 0) {
			break;
		}
		last_connect_error = errno;
		close(sock);
		sock = -1;
	}
	freeaddrinfo(addresses);

	if (sock < 0) {
		errno = last_connect_error;
		die("connect failed");
	}

	FILE *file = fdopen(sock, "rb"); 
		if(file == NULL) {
			close(sock);
			die("fdopen failed");
		}
	char request[4096];
	int req_len = snprintf(request, sizeof(request), "GET %s HTTP/1.0\r\nHost: %s:%d\r\n\r\n", filepath, servname, port);
	if (req_len >= (int)sizeof(request) || req_len < 0) {
		fprintf(stderr, "Error: Request too long\n");
		fclose(file);
		exit(1);
	}
	if (send_all(sock, request, (size_t)req_len) < 0) {
		fclose(file);
		die("send failed");
	}
	char buffer[4096];
	size_t n;

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
	uint64_t expected_length = 0;
	int content_length_seen = 0;
	int headers_complete = 0;
	int headers_failed = 0;
	while((fgets(buffer, sizeof(buffer), file)) != NULL) {
		size_t len = strlen(buffer);
		if (len == 0 || buffer[len - 1] != '\n') {
			headers_failed = 1;
			break;
		}
		if ((len == 2 && buffer[0] == '\r') ||
		    (len == 1 && buffer[0] == '\n')) {
			headers_complete = 1;
			break;
		}
		if (strncasecmp(buffer, "Content-Length:", 15) == 0) {
			if (content_length_seen ||
			    parse_content_length(buffer + 15, &expected_length) < 0) {
				headers_failed = 1;
				break;
			}
			content_length_seen = 1;
		}
	}
	if (!headers_complete || headers_failed || ferror(file)) {
		fprintf(stderr, "Error: Incomplete or malformed HTTP headers\n");
		fclose(file);
		return 1;
	}

	size_t temp_name_size = strlen(filename) + sizeof(".part.XXXXXX") + 1;
	char *temp_name = malloc(temp_name_size);
	if (temp_name == NULL) {
		fprintf(stderr, "Error: Out of memory\n");
		fclose(file);
		return 1;
	}
	int temp_name_length = snprintf(
		temp_name,
		temp_name_size,
		".%s.part.XXXXXX",
		filename);
	if (temp_name_length < 0 || (size_t)temp_name_length >= temp_name_size) {
		fprintf(stderr, "Error: Download filename is too long\n");
		free(temp_name);
		fclose(file);
		return 1;
	}

	int temp_fd = mkstemp(temp_name);
	if (temp_fd < 0) {
		perror("Error: Cannot create temporary download");
		free(temp_name);
		fclose(file);
		return 1;
	}

	FILE *downloadFile = fdopen(temp_fd, "wb");
	if (downloadFile == NULL) {
		perror("Error: Cannot open temporary download");
		close(temp_fd);
		remove_temporary_download(temp_name);
		free(temp_name);
		fclose(file);
		return 1;
	}

	uint64_t total_written = 0;
	int download_failed = 0;
	while ((n = fread(buffer, 1, sizeof(buffer), file)) > 0) {
		if (fwrite(buffer, 1, n, downloadFile) != n) {
			download_failed = 1;
			break;
		}
		if (total_written > UINT64_MAX - n) {
			download_failed = 1;
			break;
		}
		total_written += (uint64_t)n;
	}
	if (ferror(file) ||
	    (content_length_seen && total_written != expected_length)) {
		download_failed = 1;
	}
	if (!download_failed && fflush(downloadFile) != 0) download_failed = 1;
	if (!download_failed) {
		mode_t old_mask = umask(0);
		(void)umask(old_mask);
		if (fchmod(fileno(downloadFile), (mode_t)(0666 & ~old_mask)) < 0) {
			download_failed = 1;
		}
	}
	if (!download_failed && fsync(fileno(downloadFile)) != 0) download_failed = 1;
	if (fclose(downloadFile) != 0) download_failed = 1;
	fclose(file);
	if (download_failed) {
		remove_temporary_download(temp_name);
		free(temp_name);
		fprintf(stderr, "Error: Incomplete or failed download\n");
		return 1;
	}

	if (link(temp_name, filename) < 0) {
		if (errno == EEXIST) {
			fprintf(stderr, "Error: Destination file already exists: %s\n", filename);
		} else {
			perror("Error: Cannot publish download");
		}
		remove_temporary_download(temp_name);
		free(temp_name);
		return 1;
	}
	if (unlink(temp_name) < 0) {
		fprintf(stderr, "Warning: Download completed, but temporary link %s remains\n",
			temp_name);
	}
	free(temp_name);
	return 0;
}
