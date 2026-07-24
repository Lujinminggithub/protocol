#include "nb_auth.h"

#include <errno.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define NB_AUTH_MIN_ITERATIONS 100000u
#define NB_AUTH_MAX_ITERATIONS 2000000u

static int fail(char* error,size_t cap,const char* fmt,...){
    if(error&&cap){va_list ap;va_start(ap,fmt);vsnprintf(error,cap,fmt,ap);va_end(ap);}
    return -1;
}

static int hex_decode(unsigned char* out,size_t cap,const char* text,size_t* out_len){
    size_t n=strlen(text);
    if((n&1u)||n/2u>cap)return -1;
    for(size_t i=0;i<n;i+=2){
        int hi=text[i]>='0'&&text[i]<='9'?text[i]-'0':text[i]>='a'&&text[i]<='f'?text[i]-'a'+10:text[i]>='A'&&text[i]<='F'?text[i]-'A'+10:-1;
        int lo=text[i+1]>='0'&&text[i+1]<='9'?text[i+1]-'0':text[i+1]>='a'&&text[i+1]<='f'?text[i+1]-'a'+10:text[i+1]>='A'&&text[i+1]<='F'?text[i+1]-'A'+10:-1;
        if(hi<0||lo<0)return -1;
        out[i/2]=(unsigned char)((hi<<4)|lo);
    }
    *out_len=n/2u;return 0;
}

int nb_auth_check_private_file(const char* path,char* error,size_t error_cap){
    struct stat st;
    if(path==NULL||path[0]==0)return fail(error,error_cap,"missing path");
    if(stat(path,&st)!=0)return fail(error,error_cap,"stat %s: %s",path,strerror(errno));
    if(!S_ISREG(st.st_mode))return fail(error,error_cap,"%s is not a regular file",path);
#ifndef _WIN32
    if(st.st_uid!=geteuid())return fail(error,error_cap,"%s must be owned by uid %lu",path,(unsigned long)geteuid());
    if((st.st_mode&0077)!=0)return fail(error,error_cap,"%s permissions must be 0600 or stricter",path);
#endif
    return 0;
}

int nb_auth_users_load(nb_auth_users_t* out,const char* path,char* error,size_t error_cap){
    if(out==NULL)return fail(error,error_cap,"missing output");
    if(nb_auth_check_private_file(path,error,error_cap)!=0)return -1;
    FILE* f=fopen(path,"r");if(f==NULL)return fail(error,error_cap,"open %s: %s",path,strerror(errno));
    nb_auth_users_t next;memset(&next,0,sizeof(next));
    if(RAND_bytes(next.cache_key,sizeof(next.cache_key))!=1){fclose(f);return fail(error,error_cap,"authentication cache key generation failed");}
    char line[512];unsigned line_no=0;
    while(fgets(line,sizeof(line),f)){
        line_no++;line[strcspn(line,"\r\n")]=0;
        char* p=line;while(*p==' '||*p=='\t')p++;
        if(*p==0||*p=='#')continue;
        if(next.count>=NB_AUTH_MAX_USERS){fclose(f);return fail(error,error_cap,"too many users");}
        char* save=NULL;char* name=strtok_r(p,":",&save);char* iter=strtok_r(NULL,":",&save);
        char* salt=strtok_r(NULL,":",&save);char* hash=strtok_r(NULL,":",&save);char* extra=strtok_r(NULL,":",&save);
        if(!name||!iter||!salt||!hash||extra){fclose(f);return fail(error,error_cap,"invalid user record at line %u",line_no);}
        size_t name_len=strlen(name);char* end=NULL;unsigned long rounds=strtoul(iter,&end,10);
        if(name_len==0||name_len>=NB_AUTH_NAME_MAX||*end||rounds<NB_AUTH_MIN_ITERATIONS||rounds>NB_AUTH_MAX_ITERATIONS){
            fclose(f);return fail(error,error_cap,"invalid user or iterations at line %u",line_no);
        }
        for(size_t i=0;i<next.count;i++)if(strcmp(next.users[i].name,name)==0){fclose(f);return fail(error,error_cap,"duplicate user at line %u",line_no);}
        nb_auth_user_t* u=&next.users[next.count];memcpy(u->name,name,name_len+1);u->iterations=(unsigned)rounds;
        size_t hash_len=0;
        if(hex_decode(u->salt,sizeof(u->salt),salt,&u->salt_len)!=0||u->salt_len<16||
            hex_decode(u->hash,sizeof(u->hash),hash,&hash_len)!=0||hash_len!=sizeof(u->hash)){
            fclose(f);return fail(error,error_cap,"invalid salt or hash at line %u",line_no);
        }
        next.count++;
    }
    if(ferror(f)){fclose(f);return fail(error,error_cap,"read %s failed",path);}
    fclose(f);if(next.count==0)return fail(error,error_cap,"no users in %s",path);
    *out=next;return 0;
}

int nb_auth_user_verify(const nb_auth_users_t* users,const char* name,const unsigned char* password,size_t password_len){
    if(users==NULL||users->count==0||name==NULL||password==NULL||password_len>255)return 0;
    unsigned char derived[32];int found=0,valid=0;
    const nb_auth_user_t* selected=&users->users[0];
    for(size_t i=0;i<users->count;i++){
        const nb_auth_user_t* u=&users->users[i];
        if(strcmp(u->name,name)!=0)continue;
        selected=u;found=1;break;
    }
    if(PKCS5_PBKDF2_HMAC((const char*)password,(int)password_len,selected->salt,(int)selected->salt_len,
        (int)selected->iterations,EVP_sha256(),sizeof(derived),derived)==1){
        valid=CRYPTO_memcmp(derived,selected->hash,sizeof(derived))==0;
    }
    OPENSSL_cleanse(derived,sizeof(derived));
    return found&&valid;
}

int nb_auth_fingerprint(const nb_auth_users_t* users,const char* name,
    const unsigned char* password,size_t password_len,unsigned char out[32]){
    if(users==NULL||name==NULL||password==NULL||password_len>255)return 0;
    EVP_MD_CTX* ctx=EVP_MD_CTX_new();unsigned int out_len=0;
    uint64_t name_len=(uint64_t)strlen(name),pass_len=(uint64_t)password_len;
    int ok=ctx!=NULL&&EVP_DigestInit_ex(ctx,EVP_sha256(),NULL)==1&&
        EVP_DigestUpdate(ctx,users->cache_key,sizeof(users->cache_key))==1&&
        EVP_DigestUpdate(ctx,&name_len,sizeof(name_len))==1&&
        EVP_DigestUpdate(ctx,name,(size_t)name_len)==1&&
        EVP_DigestUpdate(ctx,&pass_len,sizeof(pass_len))==1&&
        EVP_DigestUpdate(ctx,password,password_len)==1&&
        EVP_DigestFinal_ex(ctx,out,&out_len)==1&&out_len==32;
    EVP_MD_CTX_free(ctx);return ok;
}

int nb_auth_cache_lookup(const nb_auth_users_t* users,const unsigned char fingerprint[32],uint64_t now){
    if(users==NULL||fingerprint==NULL)return 0;
    for(size_t i=0;i<NB_AUTH_CACHE_MAX;i++)if(users->cache[i].expires_at>now&&
        CRYPTO_memcmp(users->cache[i].fingerprint,fingerprint,32)==0)return 1;
    return 0;
}

void nb_auth_cache_store(nb_auth_users_t* users,const unsigned char fingerprint[32],uint64_t now,uint64_t ttl){
    if(users==NULL||fingerprint==NULL||ttl==0)return;
    size_t slot=0;uint64_t oldest=UINT64_MAX;
    for(size_t i=0;i<NB_AUTH_CACHE_MAX;i++){
        if(users->cache[i].expires_at<=now){slot=i;oldest=0;}
        else if(oldest!=0&&users->cache[i].expires_at<oldest){oldest=users->cache[i].expires_at;slot=i;}
    }
    memcpy(users->cache[slot].fingerprint,fingerprint,32);
    users->cache[slot].expires_at=(UINT64_MAX-now<ttl)?UINT64_MAX:now+ttl;
}

int nb_auth_user_verify_cached(nb_auth_users_t* users,const char* name,
    const unsigned char* password,size_t password_len,uint64_t now,
    uint64_t ttl,int* cache_hit){
    unsigned char fingerprint[32];
    if(cache_hit)*cache_hit=0;
    if(users==NULL||name==NULL||password==NULL||password_len>255||ttl==0)
        return nb_auth_user_verify(users,name,password,password_len);
    if(!nb_auth_fingerprint(users,name,password,password_len,fingerprint))return 0;
    if(nb_auth_cache_lookup(users,fingerprint,now)){
        if(cache_hit)*cache_hit=1;
        OPENSSL_cleanse(fingerprint,sizeof(fingerprint));return 1;
    }
    int valid=nb_auth_user_verify(users,name,password,password_len);
    if(valid)nb_auth_cache_store(users,fingerprint,now,ttl);
    OPENSSL_cleanse(fingerprint,sizeof(fingerprint));return valid;
}
