#include "nb_session_index.h"
#include <assert.h>
#include <stdio.h>

int main(void){
    nb_session_index_t index;nb_session_index_init(&index);int a=1,b=2;
    assert(nb_session_index_put(&index,NB_SESSION_KEY_ID,7,0,0,&a)==0);
    assert(nb_session_index_find(&index,NB_SESSION_KEY_ID,7,0,0)==&a);
    assert(nb_session_index_put(&index,NB_SESSION_KEY_UP_STREAM,0x1234,8,0,&b)==0);
    assert(nb_session_index_find(&index,NB_SESSION_KEY_UP_STREAM,0x1234,8,0)==&b);
    assert(nb_session_index_hash_text("Host.Example")==nb_session_index_hash_text("host.example"));
    nb_session_index_remove_value(&index,&a);assert(nb_session_index_find(&index,NB_SESSION_KEY_ID,7,0,0)==NULL);
    for(unsigned i=0;i<2000;i++)assert(nb_session_index_put(&index,NB_SESSION_KEY_ID,100+i,0,0,&a)==0);
    for(unsigned i=0;i<2000;i++)assert(nb_session_index_find(&index,NB_SESSION_KEY_ID,100+i,0,0)==&a);
    nb_session_index_remove_value(&index,&a);assert(index.count==1);
    puts("RESULT PASS");return 0;
}
