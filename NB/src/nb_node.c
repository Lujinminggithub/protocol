#include "nb_node_core.inc"
#include "nb_node_session.inc"
#include "nb_node_pool.inc"
#include "nb_node_transport.inc"
#include "nb_node_local.inc"
#include "nb_node_main.inc"

int main(int argc,char** argv){
#ifndef NB_VERSION_PRODUCT
#define NB_VERSION_PRODUCT "V200R001C02"
#endif
#ifndef NB_VERSION_SEMANTIC
#define NB_VERSION_SEMANTIC "2.1.2"
#endif
    if(argc==2&&(!strcmp(argv[1],"--version")||!strcmp(argv[1],"-V"))){
        printf("Newbility Node %s (%s)\n",NB_VERSION_PRODUCT,NB_VERSION_SEMANTIC);return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--shard-dir"))return nb_shard_run(argv[2],nb_node_instance_execute);
    nb_instance_t* instance=calloc(1,sizeof(*instance));if(instance==NULL)return 1;
    nb_instance_init(instance);int status=nb_node_instance_execute(argc,argv,instance);free(instance);return status;
}
