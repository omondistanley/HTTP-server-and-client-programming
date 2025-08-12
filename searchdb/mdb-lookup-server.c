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

#define KeyMax 5
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
static void die(const char *s) {
	perror(s);
	exit(1);
}
int main( int argc, char **argv) {
	// ignore SIGPIPE so that we don’t terminate when we call
	// send() on a disconnected socket.
	if (signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		die("signal() failed");
	
	if(argc != 3) {
		fprintf(stderr, "Usage: %s <database_file> <Server port>\n", argv[0]);
		exit(1);
	}
	unsigned short port = atoi(argv[2]);
	int server_socket;
	if((server_socket = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		die("socket failed\n");
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
	struct sockaddr_in client_address;
	int client_socket = 0;
	socklen_t clientLen = sizeof(client_address);
	int len = 0;
	char buffer[1000] = {0};
	while(1) {
		client_socket = accept(server_socket, (struct sockaddr *)&client_address, &clientLen);
		if(client_socket < 0) {
			die("accept failed\n");
		}
		fprintf(stderr, "\nconnection started from: %s\n", inet_ntoa(client_address.sin_addr));
		//while((received = recv(client_socket, buffer, sizeof(buffer), 0)) > 0){
			FILE *database = fdopen(client_socket, "rb");
			if(database == NULL) {
				die("fopen failed");
			}
			char *filename = argv[1];
   		FILE *fp = fopen(filename, "rb");
    		if (fp == NULL)
        	die(filename);
    		/*
     		* read all records into memory
    	 	*/
    		struct List list;
    		initList(&list);

    		int loaded = loadmdb(fp, &list);
    		if (loaded < 0)
        		die("loadmdb");
    		fclose(fp);
    		/*
     		* lookup loop
     		*/
    		char line[1000];
    		char key[KeyMax + 1];
    		while (fgets(line, sizeof(line), database) != NULL) {
        		// must null-terminate the string manually after strncpy().
        		strncpy(key, line, sizeof(key) - 1);
        		key[sizeof(key) - 1] = '\0';
        		// if newline is there, remove it.
        		size_t last = strlen(key) - 1;
        		if (key[last] == '\n')
           	 	key[last] = '\0';
        		// traverse the list, printing out the matching records
        		struct Node *node = list.head;
        		int recNo = 1;
        		while (node) {
            			struct MdbRec *rec = (struct MdbRec *)node->data;
            			if (strstr(rec->name, key) || strstr(rec->msg, key)) {
		     			len = sprintf(line, "%4d. {%s},said {%s}\n", recNo, rec->name, rec->msg);
                
					write(client_socket, &line, len);
                		
            			}
            		node = node->next;
            		recNo++;
       		 	}
			//Ensuring there's a blank line after the lookup is done.
			write(client_socket, "\n",1);
		
    		}
    		// see if fgets() produced error
    		if (ferror(stdin))
        		die("stdin");
    		/*
     		* clean up and quit
     		*/
    		freemdb(&list);
		if(send(client_socket, &buffer, sizeof(buffer), 0) < 0) {
			fprintf(stderr, "ERR: send failed\n");
		}

		fclose(database);
		//}
		close(client_socket);
                fprintf(stderr, "connection terminated from: %s\n", inet_ntoa(client_address.sin_addr));
	}
	return 0;
}
