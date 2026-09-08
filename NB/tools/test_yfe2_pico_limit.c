#include "picoquic.h"
#include "picoquic_internal.h"

#include <stdio.h>
#include <string.h>

#define CHECK(value) do { if(!(value)){ \
    fprintf(stderr,"check failed line %d: %s\n",__LINE__,#value);return 1; } } while(0)

int main(void){
    picoquic_cnx_t connection;
    picoquic_path_t path;
    picoquic_path_t* paths[1];
    picoquic_cnx_t connection_before;
    picoquic_path_t path_before;
    picoquic_nb_limit_state_t state;
    memset(&connection,0,sizeof(connection));
    memset(&path,0,sizeof(path));
    paths[0]=&path;connection.path=paths;connection.nb_paths=1;
    path.delivered_limited_index=7;
    connection.nb_trains_blocked_cwin=3;
    connection.nb_trains_blocked_others=4;
    connection_before=connection;path_before=path;
    CHECK(picoquic_get_nb_limit_state(&connection,&state)==0);
    CHECK(state.app_limited==1&&state.send_queue_full==7);
    CHECK(memcmp(&connection,&connection_before,sizeof(connection))==0);
    CHECK(memcmp(&path,&path_before,sizeof(path))==0);
    CHECK(picoquic_get_nb_limit_state(NULL,&state)!=0);
    puts("nb_yfe2_pico_limit_test: ok");
    return 0;
}
