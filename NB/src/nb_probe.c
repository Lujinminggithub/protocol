#include "nb_probe.h"

#include <stdio.h>

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
