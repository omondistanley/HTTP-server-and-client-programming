/*
 * mylist.c - Simple linked list implementation
 */

#include "mylist.h"
#include <stdlib.h>

void initList(struct List *list) {
    list->head = NULL;
}

struct Node *addAfter(struct List *list, struct Node *prevNode, void *data) {
    struct Node *newNode = (struct Node *)malloc(sizeof(struct Node));
    if (!newNode)
        return NULL;
    
    newNode->data = data;
    
    if (prevNode == NULL) {
        // Add at the beginning
        newNode->next = list->head;
        list->head = newNode;
    } else {
        // Add after prevNode
        newNode->next = prevNode->next;
        prevNode->next = newNode;
    }
    
    return newNode;
}

void traverseList(struct List *list, void (*func)(void *)) {
    struct Node *node = list->head;
    while (node) {
        if (func && node->data) {
            func(node->data);
        }
        node = node->next;
    }
}

void removeAllNodes(struct List *list) {
    struct Node *node = list->head;
    while (node) {
        struct Node *next = node->next;
        free(node);
        node = next;
    }
    list->head = NULL;
}

