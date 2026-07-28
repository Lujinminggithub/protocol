#include "nb_probe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void){
    uint64_t last=0;
    char line[96];
    assert(nb_probe_progress_format(100,1000000,&last,line,sizeof(line))==0);
    assert(last==1000000);
    assert(nb_probe_progress_format(200,10999999,&last,line,sizeof(line))==0);
    int length=nb_probe_progress_format(300,11000000,&last,line,sizeof(line));
    assert(length==(int)strlen("NBPROBE PROGRESS bytes=300\n"));
    assert(strcmp(line,"NBPROBE PROGRESS bytes=300\n")==0);
    assert(last==11000000);
    assert(nb_probe_progress_format(400,10000000,&last,line,sizeof(line))==0);
    assert(nb_probe_progress_format(400,21000000,NULL,line,sizeof(line))==-1);
    assert(nb_probe_progress_format(400,21000000,&last,line,1)==-1);
    puts("nb_probe tests passed");
    return 0;
}
