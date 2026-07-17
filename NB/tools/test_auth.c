#include "nb_auth.h"

#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void hex(char* out,const unsigned char* in,size_t n){
    static const char h[]="0123456789abcdef";
    for(size_t i=0;i<n;i++){out[i*2]=h[in[i]>>4];out[i*2+1]=h[in[i]&15];}out[n*2]=0;
}

int main(void){
    char path[128];
#ifdef _WIN32
    snprintf(path,sizeof(path),"nb-auth-%ld.tmp",(long)getpid());
#else
    snprintf(path,sizeof(path),"/tmp/nb-auth-%ld",(long)getpid());
#endif
    const unsigned char salt[16]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15};
    unsigned char hash[32];char salt_hex[33],hash_hex[65],err[256];
    if(PKCS5_PBKDF2_HMAC("correct",7,salt,16,100000,EVP_sha256(),32,hash)!=1)return 1;
    hex(salt_hex,salt,16);hex(hash_hex,hash,32);
    FILE* f=fopen(path,"w");if(!f)return 1;
    fprintf(f,"alice:100000:%s:%s\n",salt_hex,hash_hex);fclose(f);chmod(path,0600);
    nb_auth_users_t users;
    int cache_hit=-1;
    int ok=nb_auth_users_load(&users,path,err,sizeof(err))==0&&
        nb_auth_user_verify_cached(&users,"alice",(const unsigned char*)"correct",7,1000,500,&cache_hit)&&cache_hit==0&&
        nb_auth_user_verify_cached(&users,"alice",(const unsigned char*)"correct",7,1200,500,&cache_hit)&&cache_hit==1&&
        nb_auth_user_verify_cached(&users,"alice",(const unsigned char*)"correct",7,2000,500,&cache_hit)&&cache_hit==0&&
        !nb_auth_user_verify(&users,"alice",(const unsigned char*)"wrong",5)&&
        !nb_auth_user_verify_cached(&users,"alice",(const unsigned char*)"wrong",5,2100,500,&cache_hit)&&cache_hit==0&&
        !nb_auth_user_verify(&users,"bob",(const unsigned char*)"correct",7);
#ifndef _WIN32
    chmod(path,0644);
    ok=ok&&nb_auth_users_load(&users,path,err,sizeof(err))!=0;
#endif
    unlink(path);printf("RESULT %s\n",ok?"PASS":"FAIL");return ok?0:1;
}
