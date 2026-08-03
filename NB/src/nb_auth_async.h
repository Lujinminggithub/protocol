#ifndef NB_AUTH_ASYNC_H
#define NB_AUTH_ASYNC_H

#include <stddef.h>
#include <stdint.h>

#include "nb_auth.h"

typedef struct {
    uint32_t session_id;
    int valid;
    char username[NB_AUTH_NAME_MAX];
    unsigned char fingerprint[NB_AUTH_FINGERPRINT_LEN];
    uint64_t work_us;
} nb_auth_async_result_t;

typedef struct nb_auth_async nb_auth_async_t;

nb_auth_async_t* nb_auth_async_create(const nb_auth_users_t* users,int worker_count);
void nb_auth_async_destroy(nb_auth_async_t* auth);
int nb_auth_async_context_result_fd(nb_auth_async_t* auth);
int nb_auth_async_context_submit(nb_auth_async_t* auth,uint32_t session_id,const char* username,
    const unsigned char* password,size_t password_len);
int nb_auth_async_context_pop(nb_auth_async_t* auth,nb_auth_async_result_t* result);

int nb_auth_async_init(const nb_auth_users_t* users, int worker_count);
int nb_auth_async_result_fd(void);
int nb_auth_async_submit(uint32_t session_id, const char* username,
    const unsigned char* password, size_t password_len);
int nb_auth_async_pop(nb_auth_async_result_t* result);

#endif
