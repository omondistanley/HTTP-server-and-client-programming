#ifndef _MYLIST_H_
#define _MYLIST_H_

struct Node {
    void *data;
    struct Node *next;
};

struct List {
    struct Node *head;
};

void initList(struct List *list);
struct Node *addAfter(struct List *list, struct Node *prevNode, void *data);
void traverseList(struct List *list, void (*func)(void *));
void removeAllNodes(struct List *list);

#endif

