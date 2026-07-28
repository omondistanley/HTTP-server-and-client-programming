#ifndef _MDB_H_
#define _MDB_H_

#include <stdint.h>

struct MdbRec {
    uint64_t id;
    char name[16];
    char msg[24];
};

#endif
