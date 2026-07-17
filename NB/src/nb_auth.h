#ifndef NB_AUTH_H
#define NB_AUTH_H

#include <stddef.h>
#include <stdint.h>

#define NB_AUTH_MAX_USERS 64
#define NB_AUTH_NAME_MAX 64
#define NB_AUTH_CACHE_MAX 16

typedef struct {
    char name[NB_AUTH_NAME_MAX];
    unsigned iterations;
    unsigned char salt[32];
    size_t salt_len;
    unsigned char hash[32];
} nb_auth_user_t;

typedef struct {
    unsigned char fingerprint[32];
    uint64_t expires_at;
} nb_auth_cache_entry_t;

typedef struct {
    nb_auth_user_t users[NB_AUTH_MAX_USERS];
    size_t count;
    unsigned char cache_key[32];
    nb_auth_cache_entry_t cache[NB_AUTH_CACHE_MAX];
} nb_auth_users_t;

int nb_auth_check_private_file(const char* path, char* error, size_t error_cap);
int nb_auth_users_load(nb_auth_users_t* out, const char* path, char* error, size_t error_cap);
int nb_auth_user_verify(const nb_auth_users_t* users, const char* name,
    const unsigned char* password, size_t password_len);
int nb_auth_user_verify_cached(nb_auth_users_t* users, const char* name,
    const unsigned char* password, size_t password_len, uint64_t now,
    uint64_t ttl, int* cache_hit);

#endif
