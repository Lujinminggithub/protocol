#include "../src/nb_shard.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void){
    char path[]="/tmp/nb-shard-test-XXXXXX";int fd=mkstemp(path);if(fd<0)return 1;
    const char* text="schema=1\ninstance_id=line-1\ncontrol_path=/tmp/line-1.ctl\narg=-r\narg=entry\narg=-l\narg=1080\nenv.NB_CC=cubic\n";
    if(write(fd,text,strlen(text))!=(ssize_t)strlen(text)||close(fd)!=0||chmod(path,0600)!=0)return 2;
    nb_shard_config_t config;char error[128];int rc=nb_shard_config_load(path,&config,error,sizeof(error));unlink(path);
    if(rc!=0||strcmp(config.instance_id,"line-1")||strcmp(config.control_path,"/tmp/line-1.ctl")||config.argument_count!=4||config.environment_count!=1||config.fingerprint==0)return 3;
    puts("RESULT PASS");return 0;
}
