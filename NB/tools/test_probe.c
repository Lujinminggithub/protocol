#include "nb_probe.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void){
    uint64_t last=0;
    char line[96];
    uint8_t header[NB_PROBE_HEADER_SIZE];uint64_t expected=0;
    assert(nb_probe_header_format(262144,header,sizeof(header))==(int)NB_PROBE_HEADER_SIZE);
    assert(nb_probe_header_parse(header,sizeof(header),&expected)==0&&expected==262144);
    header[0]='X';assert(nb_probe_header_parse(header,sizeof(header),&expected)==-1);
    assert(nb_probe_header_format(0,header,sizeof(header))==-1);
    const uint8_t payload[]={0x61,0x62,0x63};
    uint64_t hash=nb_probe_hash_update(NB_PROBE_HASH_INITIAL,payload,sizeof(payload));
    assert(hash==0xe71fa2190541574bULL);
    int ack=nb_probe_ack_format(3,hash,line,sizeof(line));
    assert(ack==(int)strlen("NBPROBE OK bytes=3 hash=e71fa2190541574b\n"));
    assert(strcmp(line,"NBPROBE OK bytes=3 hash=e71fa2190541574b\n")==0);
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
