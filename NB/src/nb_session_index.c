#include "nb_session_index.h"

#include <string.h>

static uint64_t mix64(uint64_t x){
    x^=x>>30;x*=UINT64_C(0xbf58476d1ce4e5b9);x^=x>>27;
    x*=UINT64_C(0x94d049bb133111eb);return x^(x>>31);
}

static size_t slot_for(uint8_t kind,uint64_t a,uint64_t b,uint64_t c){
    return (size_t)(mix64(a)^mix64(b+kind)^mix64(c+UINT64_C(0x9e3779b97f4a7c15)))&(NB_SESSION_INDEX_CAP-1u);
}

static int key_equal(const nb_session_index_entry_t* e,uint8_t kind,uint64_t a,uint64_t b,uint64_t c){
    return e->state==1&&e->kind==kind&&e->a==a&&e->b==b&&e->c==c;
}

void nb_session_index_init(nb_session_index_t* index){if(index)memset(index,0,sizeof(*index));}

int nb_session_index_put(nb_session_index_t* index,uint8_t kind,uint64_t a,uint64_t b,uint64_t c,void* value){
    if(index==NULL||kind==0||value==NULL)return -1;
    size_t slot=slot_for(kind,a,b,c),tomb=NB_SESSION_INDEX_CAP;
    for(size_t probe=0;probe<NB_SESSION_INDEX_CAP;probe++){
        nb_session_index_entry_t* e=&index->entries[(slot+probe)&(NB_SESSION_INDEX_CAP-1u)];
        if(key_equal(e,kind,a,b,c)){e->value=value;return 0;}
        if(e->state==2&&tomb==NB_SESSION_INDEX_CAP)tomb=(slot+probe)&(NB_SESSION_INDEX_CAP-1u);
        if(e->state==0){
            if(tomb!=NB_SESSION_INDEX_CAP)e=&index->entries[tomb];
            e->kind=kind;e->a=a;e->b=b;e->c=c;e->value=value;e->state=1;index->count++;return 0;
        }
    }
    return -1;
}

void* nb_session_index_find(const nb_session_index_t* index,uint8_t kind,uint64_t a,uint64_t b,uint64_t c){
    if(index==NULL||kind==0)return NULL;
    size_t slot=slot_for(kind,a,b,c);
    for(size_t probe=0;probe<NB_SESSION_INDEX_CAP;probe++){
        const nb_session_index_entry_t* e=&index->entries[(slot+probe)&(NB_SESSION_INDEX_CAP-1u)];
        if(e->state==0)return NULL;
        if(key_equal(e,kind,a,b,c))return e->value;
    }
    return NULL;
}

void nb_session_index_remove_value(nb_session_index_t* index,const void* value){
    if(index==NULL||value==NULL)return;
    for(size_t i=0;i<NB_SESSION_INDEX_CAP;i++)if(index->entries[i].state==1&&index->entries[i].value==value){
        index->entries[i].state=2;index->entries[i].value=NULL;if(index->count>0)index->count--;
    }
}

uint64_t nb_session_index_hash_text(const char* text){
    uint64_t hash=UINT64_C(1469598103934665603);if(text==NULL)return hash;
    for(const unsigned char* p=(const unsigned char*)text;*p;p++){unsigned char c=*p;if(c>='A'&&c<='Z')c=(unsigned char)(c+32);hash^=c;hash*=UINT64_C(1099511628211);}
    return hash;
}
