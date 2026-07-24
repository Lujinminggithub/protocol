#define _POSIX_C_SOURCE 200809L
#include "nb_auth_async.h"

#include <assert.h>
#include <openssl/evp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static void hex(char* out,const unsigned char* in,size_t n){
    static const char h[]="0123456789abcdef";
    for(size_t i=0;i<n;i++){out[i*2]=h[in[i]>>4];out[i*2+1]=h[in[i]&15];}out[n*2]=0;
}
static uint64_t now_us(void){
    struct timespec ts;assert(clock_gettime(CLOCK_MONOTONIC,&ts)==0);
    return (uint64_t)ts.tv_sec*1000000ULL+(uint64_t)ts.tv_nsec/1000ULL;
}

int main(void){
    char path[128];snprintf(path,sizeof(path),"/tmp/nb-auth-async-%ld",(long)getpid());
    const unsigned char salt[16]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    unsigned char hash[32];char salt_hex[33],hash_hex[65],error[256];
    assert(PKCS5_PBKDF2_HMAC("correct",7,salt,16,100000,EVP_sha256(),32,hash)==1);
    hex(salt_hex,salt,16);hex(hash_hex,hash,32);
    FILE* file=fopen(path,"w");assert(file!=NULL);
    fprintf(file,"alice:100000:%s:%s\n",salt_hex,hash_hex);fclose(file);assert(chmod(path,0600)==0);
    nb_auth_users_t users;assert(nb_auth_users_load(&users,path,error,sizeof(error))==0);unlink(path);
    assert(nb_auth_async_init(&users,4)==0&&nb_auth_async_result_fd()>=0);

    uint64_t submit_started=now_us();
    for(uint32_t id=1;id<=16;id++){
        const unsigned char* password=(const unsigned char*)((id&1)?"correct":"wrong");
        size_t length=(id&1)?7:5;assert(nb_auth_async_submit(id,"alice",password,length)==0);
    }
    assert(now_us()-submit_started<100000ULL);

    int received=0,valid=0;unsigned char valid_fingerprint[NB_AUTH_FINGERPRINT_LEN]={0};
    uint64_t deadline=now_us()+10000000ULL;
    while(received<16&&now_us()<deadline){
        struct pollfd descriptor={nb_auth_async_result_fd(),POLLIN,0};
        assert(poll(&descriptor,1,1000)>=0);char drain[64];while(read(descriptor.fd,drain,sizeof(drain))>0){}
        nb_auth_async_result_t result;
        while(nb_auth_async_pop(&result)){
            assert(result.session_id>=1&&result.session_id<=16&&result.work_us>0);
            assert(result.valid==(int)(result.session_id&1));
            if(result.valid){valid++;memcpy(valid_fingerprint,result.fingerprint,sizeof(valid_fingerprint));}
            received++;
        }
    }
    assert(received==16&&valid==8);
    nb_auth_cache_store(&users,valid_fingerprint,1000,500);
    assert(nb_auth_cache_lookup(&users,valid_fingerprint,1200)==1);
    assert(nb_auth_cache_lookup(&users,valid_fingerprint,1600)==0);
    puts("RESULT PASS");return 0;
}
