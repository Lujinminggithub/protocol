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

int nb_auth_async_init(const nb_auth_users_t* users, int worker_count);
int nb_auth_async_result_fd(void);
int nb_auth_async_submit(uint32_t session_id, const char* username,
    const unsigned char* password, size_t password_len);
int nb_auth_async_pop(nb_auth_async_result_t* result);

#endif
