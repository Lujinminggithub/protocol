#ifndef NB_SESSION_INDEX_H
#define NB_SESSION_INDEX_H

#include <stddef.h>
#include <stdint.h>

#define NB_SESSION_INDEX_CAP 4096u

enum {
    NB_SESSION_KEY_ID = 1,
    NB_SESSION_KEY_UP_STREAM = 2,
    NB_SESSION_KEY_DOWN_STREAM = 3,
    NB_SESSION_KEY_UDP_UP = 4,
    NB_SESSION_KEY_UDP_DOWN = 5,
    NB_SESSION_KEY_UDP_CHILD = 6
};

typedef struct {
    uint64_t a,b,c;
    void* value;
    uint8_t kind;
    uint8_t state;
} nb_session_index_entry_t;

typedef struct {
    nb_session_index_entry_t entries[NB_SESSION_INDEX_CAP];
    size_t count;
} nb_session_index_t;

void nb_session_index_init(nb_session_index_t* index);
int nb_session_index_put(nb_session_index_t* index,uint8_t kind,uint64_t a,uint64_t b,uint64_t c,void* value);
void* nb_session_index_find(const nb_session_index_t* index,uint8_t kind,uint64_t a,uint64_t b,uint64_t c);
void nb_session_index_remove_value(nb_session_index_t* index,const void* value);
uint64_t nb_session_index_hash_text(const char* text);

#endif
