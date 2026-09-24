#pragma once

#include <stddef.h>


struct DList {
    DList *prev = NULL;
    DList *next = NULL;
};

inline void dlist_init(DList *node) {
    node->prev = node->next = node;
}

inline bool dlist_empty(DList *node) {
    return node->next == node;
}

inline void dlist_detach(DList *node) {
    DList *prev = node->prev;
    DList *next = node->next;
    prev->next = next;
    next->prev = prev;
}

inline void dlist_insert_before(DList *target, DList *rookie) {
    DList *prev = target->prev;
    prev->next = rookie;
    rookie->prev = prev;
    rookie->next = target;
    target->prev = rookie;
}

// error code for TAG_ERR
enum {
    ERR_UNKNOWN = 1,    // unknown command
    ERR_TOO_BIG = 2,    // response too big
    ERR_BAD_TYP = 3,    // unexpected value type
    ERR_BAD_ARG = 4,    // bad arguments
};

// data types of serialized data
enum {
    TAG_NIL = 0,    // nil
    TAG_ERR = 1,    // error code + msg
    TAG_STR = 2,    // string
    TAG_INT = 3,    // int64
    TAG_DBL = 4,    // double
    TAG_ARR = 5,    // array
};

enum {
    T_INIT  = 0,
    T_STR   = 1,    // string
    T_ZSET  = 2,    // sorted set
};