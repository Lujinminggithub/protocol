#include "nb_node_core.inc"
#include "nb_node_session.inc"
#include "nb_node_pool.inc"
#include "nb_node_transport.inc"
#include "nb_node_local.inc"
#include "nb_node_main.inc"

int main(int argc,char** argv){
    if(argc==3&&!strcmp(argv[1],"--shard-dir"))return nb_shard_run(argv[2],nb_node_instance_execute);
    nb_instance_t* instance=calloc(1,sizeof(*instance));if(instance==NULL)return 1;
    nb_instance_init(instance);int status=nb_node_instance_execute(argc,argv,instance);free(instance);return status;
}
