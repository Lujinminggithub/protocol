#include "../src/nb_ring.h"

#include <stdio.h>
#include <string.h>

int main(void){
    nb_ring_t q={0};uint8_t a[7000],b[9000],out[14000];
    for(size_t i=0;i<sizeof(a);i++)a[i]=(uint8_t)(i%251u);
    for(size_t i=0;i<sizeof(b);i++)b[i]=(uint8_t)((i+17u)%251u);
    if(nb_ring_append(&q,a,sizeof(a),20000)!=0)return 1;
    if(nb_ring_copyout(&q,out,3000)!=3000||memcmp(out,a,3000)!=0)return 2;
    if(nb_ring_append(&q,b,sizeof(b),20000)!=0)return 3;
    if(nb_ring_copyout(&q,out,13000)!=13000)return 4;
    if(memcmp(out,a+3000,4000)!=0||memcmp(out+4000,b,sizeof(b))!=0)return 5;
    if(q.len!=0||nb_ring_append(&q,b,sizeof(b),8000)==0)return 6;
    nb_ring_dispose(&q);puts("RESULT PASS");return 0;
}
