#include <stdio.h>
#include "mylist.h"
#include "mdb.h"
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#define KeyMax 1000
#define MAX_LINE_LEN 1000
#define MAX_RESPONSE_LEN 1200
#define MAX_NAME_LEN 15
#define MAX_MSG_LEN 23
int loadmdb(FILE *fp, struct List *dest)
{
    /*
     * read all records into memory
     */
    struct MdbRec r;
    struct Node *node = NULL;
    int count = 0;
    while (fread(&r, sizeof(r), 1, fp) == 1) {
        // allocate memory for a new record and copy into it the one
        // that was just read from the database.
        struct MdbRec *rec = (struct MdbRec *)malloc(sizeof(r));
        if (!rec)
            return -1;
        memcpy(rec, &r, sizeof(r));
        // add the record to the linked list.
        node = addAfter(dest, node, rec);
        if (node == NULL)
            return -1;
        count++;
    }
    // see if fread() produced error
    if (ferror(fp))
        return -1;
    return count;
}
void freemdb(struct List *list)
{
    // free all the records
    traverseList(list, &free);
    removeAllNodes(list);
}

static int save_database(const char *filename, struct List *list) {
    char tempfile[1024];
    snprintf(tempfile, sizeof(tempfile), "%s.tmp", filename);
    
    FILE *fp = fopen(tempfile, "wb");
    if (!fp) {
        return -1;
    }
    
    struct Node *node = list->head;
    int count = 0;
    while (node) {
        struct MdbRec *rec = (struct MdbRec *)node->data;
        if (fwrite(rec, sizeof(struct MdbRec), 1, fp) != 1) {
            fclose(fp);
            unlink(tempfile);
            return -1;
        }
        count++;
        node = node->next;
    }
    
    if (fclose(fp) != 0) {
        unlink(tempfile);
        return -1;
    }
    
    if (rename(tempfile, filename) != 0) {
        unlink(tempfile);
        return -1;
    }
    
    return count;
}

static int add_record(struct List *list, const char *name, const char *msg) {
    struct MdbRec *rec = (struct MdbRec *)malloc(sizeof(struct MdbRec));
    if (!rec) return -1;
    
    memset(rec, 0, sizeof(struct MdbRec));
    strncpy(rec->name, name, MAX_NAME_LEN);
    rec->name[MAX_NAME_LEN] = '\0';
    strncpy(rec->msg, msg, MAX_MSG_LEN);
    rec->msg[MAX_MSG_LEN] = '\0';
    
    struct Node *node = addAfter(list, NULL, rec);
    if (!node) {
        free(rec);
        return -1;
    }
    
    return 0;
}

static int delete_record(struct List *list, int index) {
    if (index < 1) return -1;
    
    struct Node *node = list->head;
    int count = 1;
    
    if (index == 1) {
        if (node) {
            list->head = node->next;
            free(node->data);
            free(node);
            return 0;
        }
        return -1;
    }
    
    while (node && count < index - 1) {
        node = node->next;
        count++;
    }
    
    if (!node || !node->next) return -1;
    
    struct Node *to_delete = node->next;
    node->next = to_delete->next;
    free(to_delete->data);
    free(to_delete);
    return 0;
}

static int update_record(struct List *list, int index, const char *name, const char *msg) {
    if (index < 1) return -1;
    
    struct Node *node = list->head;
    int count = 1;
    
    while (node && count < index) {
        node = node->next;
        count++;
    }
    
    if (!node) return -1;
    
    struct MdbRec *rec = (struct MdbRec *)node->data;
    memset(rec, 0, sizeof(struct MdbRec));
    strncpy(rec->name, name, MAX_NAME_LEN);
    rec->name[MAX_NAME_LEN] = '\0';
    strncpy(rec->msg, msg, MAX_MSG_LEN);
    rec->msg[MAX_MSG_LEN] = '\0';
    
    return 0;
}

static void list_all_records(struct List *list, int client_socket) {
    struct Node *node = list->head;
    int recNo = 1;
    while (node) {
        struct MdbRec *rec = (struct MdbRec *)node->data;
        char response[MAX_RESPONSE_LEN];
        int len = snprintf(response, sizeof(response), "%4d. {%s},said {%s}\n", 
            recNo, rec->name, rec->msg);
        if (len > 0 && len < (int)sizeof(response)) {
            write(client_socket, response, len);
        }
        node = node->next;
        recNo++;
    }
    write(client_socket, "\n", 1);
}

static void die(const char *s) {
	perror(s);
	exit(1);
}
int main( int argc, char **argv) {
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		die("signal() failed");
	
	if(argc != 3) {
		fprintf(stderr, "Usage: %s <database_file> <Server port>\n", argv[0]);
		exit(1);
	}
	
	char *filename = argv[1];
	if (filename == NULL || strlen(filename) == 0 || strlen(filename) > 1024) {
		fprintf(stderr, "Error: Invalid database filename\n");
		exit(1);
	}
	
	char *port_str = argv[2];
	unsigned short port = atoi(port_str);
	if (port == 0 || port > 65535) {
		fprintf(stderr, "Error: Invalid port number (must be 1-65535)\n");
		exit(1);
	}
	int server_socket;
	if((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		die("socket failed\n");
	}
	// Set SO_REUSEADDR to allow immediate rebinding after server restart
	int reuse = 1;
	if(setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
		die("setsockopt failed\n");
	}
	struct sockaddr_in server_address;
	memset(&server_address, 0, sizeof(server_address));
	server_address.sin_family = AF_INET;
	server_address.sin_port = htons(port);
	server_address.sin_addr.s_addr = htonl(INADDR_ANY);
	if(bind(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) <0) {
		die("bind failed\n");
	}
	if((listen(server_socket, 10)) < 0) {
		die("listen failed\n");
	}
	FILE *fp = fopen(filename, "rb");
	if (fp == NULL)
		die(filename);
	
	struct List list;
	initList(&list);
	
	int loaded = loadmdb(fp, &list);
	if (loaded < 0)
		die("loadmdb");
	fclose(fp);
	fprintf(stderr, "Loaded %d records from database\n", loaded);
	
	char db_filename[1024];
	strncpy(db_filename, filename, sizeof(db_filename) - 1);
	db_filename[sizeof(db_filename) - 1] = '\0';

	struct sockaddr_in client_address;
	int client_socket = 0;
	socklen_t clientLen = sizeof(client_address);
	while(1) {
		client_socket = accept(server_socket, (struct sockaddr *)&client_address, &clientLen);
		if(client_socket < 0) {
			die("accept failed\n");
		}
		fprintf(stderr, "\nconnection started from: %s\n", inet_ntoa(client_address.sin_addr));
		
		FILE *database = fdopen(client_socket, "rb");
		if(database == NULL) {
			close(client_socket);
			fprintf(stderr, "fdopen failed, closing connection\n");
			continue;
		}
		char line[MAX_LINE_LEN];
		while (fgets(line, sizeof(line), database) != NULL) {
			size_t len = strlen(line);
			if (len > 0 && line[len - 1] == '\n') {
				line[len - 1] = '\0';
				len--;
			}
			if (len > 0 && line[len - 1] == '\r') {
				line[len - 1] = '\0';
				len--;
			}
			
			if (len == 0) continue;
			
			if (strncmp(line, "SEARCH ", 7) == 0) {
				char *key = line + 7;
				struct Node *node = list.head;
				int recNo = 1;
				while (node) {
					struct MdbRec *rec = (struct MdbRec *)node->data;
					if (strstr(rec->name, key) || strstr(rec->msg, key)) {
						char response[MAX_RESPONSE_LEN];
						int rlen = snprintf(response, sizeof(response), "%4d. {%s},said {%s}\n", 
							recNo, rec->name, rec->msg);
						if (rlen > 0 && rlen < (int)sizeof(response)) {
							write(client_socket, response, rlen);
						}
					}
					node = node->next;
					recNo++;
				}
				write(client_socket, "\n", 1);
			}
			else if (strncmp(line, "ADD ", 4) == 0) {
				char *data = line + 4;
				char *pipe = strchr(data, '|');
				if (!pipe) {
					write(client_socket, "ERROR: Invalid ADD format\n", 26);
					continue;
				}
				*pipe = '\0';
				char *name = data;
				char *msg = pipe + 1;
				
				while (*name == ' ') name++;
				while (*msg == ' ') msg++;
				
				if (strlen(name) > MAX_NAME_LEN || strlen(msg) > MAX_MSG_LEN) {
					write(client_socket, "ERROR: Name or message too long\n", 32);
					continue;
				}
				
				if (add_record(&list, name, msg) == 0) {
					write(client_socket, "OK\n", 3);
				} else {
					write(client_socket, "ERROR: Failed to add record\n", 28);
				}
			}
			else if (strncmp(line, "DELETE ", 7) == 0) {
				int id = atoi(line + 7);
				if (delete_record(&list, id) == 0) {
					write(client_socket, "OK\n", 3);
				} else {
					write(client_socket, "ERROR: Record not found\n", 24);
				}
			}
			else if (strncmp(line, "UPDATE ", 7) == 0) {
				char *data = line + 7;
				char *pipe1 = strchr(data, '|');
				if (!pipe1) {
					write(client_socket, "ERROR: Invalid UPDATE format\n", 29);
					continue;
				}
				*pipe1 = '\0';
				int id = atoi(data);
				char *pipe2 = strchr(pipe1 + 1, '|');
				if (!pipe2) {
					write(client_socket, "ERROR: Invalid UPDATE format\n", 29);
					continue;
				}
				*pipe2 = '\0';
				char *name = pipe1 + 1;
				char *msg = pipe2 + 1;
				
				while (*name == ' ') name++;
				while (*msg == ' ') msg++;
				
				if (strlen(name) > MAX_NAME_LEN || strlen(msg) > MAX_MSG_LEN) {
					write(client_socket, "ERROR: Name or message too long\n", 32);
					continue;
				}
				
				if (update_record(&list, id, name, msg) == 0) {
					write(client_socket, "OK\n", 3);
				} else {
					write(client_socket, "ERROR: Record not found\n", 24);
				}
			}
			else if (strcmp(line, "LIST") == 0) {
				list_all_records(&list, client_socket);
			}
			else if (strcmp(line, "SAVE") == 0) {
				if (save_database(db_filename, &list) >= 0) {
					write(client_socket, "OK\n", 3);
				} else {
					write(client_socket, "ERROR: Failed to save\n", 22);
				}
			}
			else {
				char *key = line;
				struct Node *node = list.head;
				int recNo = 1;
				while (node) {
					struct MdbRec *rec = (struct MdbRec *)node->data;
					if (strstr(rec->name, key) || strstr(rec->msg, key)) {
						char response[MAX_RESPONSE_LEN];
						int rlen = snprintf(response, sizeof(response), "%4d. {%s},said {%s}\n", 
							recNo, rec->name, rec->msg);
						if (rlen > 0 && rlen < (int)sizeof(response)) {
							write(client_socket, response, rlen);
						}
					}
					node = node->next;
					recNo++;
				}
				write(client_socket, "\n", 1);
			}
		}
		if (ferror(database)) {
			fprintf(stderr, "Error reading from client connection\n");
		}

		fclose(database);
		close(client_socket);
		fprintf(stderr, "connection terminated from: %s\n", inet_ntoa(client_address.sin_addr));
	}
	
	freemdb(&list);
	close(server_socket);
	return 0;
}
