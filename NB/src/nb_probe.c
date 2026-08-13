#include "nb_probe.h"

#include <stdio.h>
#include <string.h>

static const uint8_t NB_PROBE_MAGIC[4]={'N','B','P','1'};
static const uint8_t NB_PROBE_SOURCE_MAGIC[4]={'N','B','P','2'};

static int header_format(const uint8_t magic[4],uint64_t expected_bytes,uint8_t* output,size_t output_size){
    if(output==NULL||output_size<NB_PROBE_HEADER_SIZE||expected_bytes==0||expected_bytes>NB_PROBE_MAX_BYTES)return -1;
    memcpy(output,magic,4);for(size_t index=0;index<8;index++)output[4+index]=(uint8_t)(expected_bytes>>(56-index*8));
    return (int)NB_PROBE_HEADER_SIZE;
}

static int header_parse(const uint8_t magic[4],const uint8_t* input,size_t input_size,uint64_t* expected_bytes){
    if(input==NULL||input_size!=NB_PROBE_HEADER_SIZE||expected_bytes==NULL||memcmp(input,magic,4)!=0)return -1;
    uint64_t value=0;for(size_t index=0;index<8;index++)value=(value<<8)|input[4+index];
    if(value==0||value>NB_PROBE_MAX_BYTES)return -1;
    *expected_bytes=value;return 0;
}

int nb_probe_header_format(uint64_t expected_bytes, uint8_t* output, size_t output_size){
    return header_format(NB_PROBE_MAGIC,expected_bytes,output,output_size);
}

int nb_probe_header_parse(const uint8_t* input, size_t input_size, uint64_t* expected_bytes){
    return header_parse(NB_PROBE_MAGIC,input,input_size,expected_bytes);
}

int nb_probe_source_header_format(uint64_t expected_bytes,uint8_t* output,size_t output_size){
    return header_format(NB_PROBE_SOURCE_MAGIC,expected_bytes,output,output_size);
}
int nb_probe_source_header_parse(const uint8_t* input,size_t input_size,uint64_t* expected_bytes){
    return header_parse(NB_PROBE_SOURCE_MAGIC,input,input_size,expected_bytes);
}
void nb_probe_source_fill(uint64_t offset,uint8_t* output,size_t length){
    if(!output)return;
    for(size_t index=0;index<length;index++)output[index]=(uint8_t)(((offset+index)*13+29)&0xff);
}

uint64_t nb_probe_hash_update(uint64_t hash, const uint8_t* data, size_t length){
    if(data==NULL&&length!=0)return hash;
    for(size_t index=0;index<length;index++){
        hash^=data[index];
        hash*=1099511628211ULL;
    }
    return hash;
}

int nb_probe_ack_format(uint64_t received_bytes, uint64_t hash,
    char* output, size_t output_size){
    if(output==NULL||output_size==0)return -1;
    int length=snprintf(output,output_size,"NBPROBE OK bytes=%llu hash=%016llx\n",
        (unsigned long long)received_bytes,(unsigned long long)hash);
    return length>0&&(size_t)length<output_size?length:-1;
}

int nb_probe_progress_format(uint64_t received_bytes, uint64_t now_us,
    uint64_t* last_progress_at, char* output, size_t output_size){
    if(last_progress_at==NULL||output==NULL||output_size==0)return -1;
    if(*last_progress_at==0){*last_progress_at=now_us;return 0;}
    if(now_us<*last_progress_at||now_us-*last_progress_at<NB_PROBE_PROGRESS_INTERVAL_US)return 0;
    int length=snprintf(output,output_size,"NBPROBE PROGRESS bytes=%llu\n",
        (unsigned long long)received_bytes);
    if(length<=0||(size_t)length>=output_size)return -1;
    *last_progress_at=now_us;
    return length;
}
