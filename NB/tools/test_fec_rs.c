/* nb_fec_rs 正确性交叉验证:
 * 对每组 (K,R) 穷举所有丢包组合(2^(K+R) 个 bitmap):
 *   - 幸存分片数 >= K: 必须恢复成功且字节完全一致(MDS 性质)
 *   - 幸存分片数 <  K: 必须返回失败(不能谎报成功)
 * 覆盖生产档 K=4/R=2 及文档自适应档 K=8/R=2、K=6/R=2。
 * 多种 shard_size(含非 8 倍数)+多轮随机数据。
 */
#include "../src/nb_fec_rs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXK 8
#define MAXR 2
#define MAXN (MAXK + MAXR)

/* 返回该配置下的失败次数 */
static int test_config(size_t K, size_t R, size_t shard_size, unsigned seed){
    int fails = 0;
    uint8_t src[MAXK][2048], repair[MAXR][2048];
    uint8_t src_saved[MAXK][2048];
    uint8_t* src_ptr[MAXK]; uint8_t* rep_ptr[MAXR];
    for(size_t i=0;i<K;i++){ src_ptr[i]=src[i]; }
    for(size_t r=0;r<R;r++){ rep_ptr[r]=repair[r]; }

    srand(seed);
    for(size_t i=0;i<K;i++)
        for(size_t b=0;b<shard_size;b++){ src[i][b]=(uint8_t)(rand()&0xff); src_saved[i][b]=src[i][b]; }

    if(nb_rs_encode(K, R, src_ptr, rep_ptr, shard_size)!=0){
        printf("  [FAIL] encode error K=%zu R=%zu sz=%zu\n", K,R,shard_size); return 1;
    }
    uint8_t rep_saved[MAXR][2048];
    for(size_t r=0;r<R;r++) memcpy(rep_saved[r], repair[r], shard_size);

    size_t N = K + R;
    for(unsigned lost=0; lost < (1u<<N); lost++){
        int src_present[MAXK], rep_present[MAXR];
        size_t survivors = 0;
        for(size_t i=0;i<K;i++){ int p = !((lost>>i)&1u); src_present[i]=p; survivors+=p; }
        for(size_t r=0;r<R;r++){ int p = !((lost>>(K+r))&1u); rep_present[r]=p; survivors+=p; }

        /* 恢复前: 把"丢失"的分片清零(模拟数据不可用),幸存的保持原值 */
        uint8_t s[MAXK][2048], rp[MAXR][2048];
        uint8_t* sp[MAXK]; uint8_t* rpp[MAXR];
        for(size_t i=0;i<K;i++){ sp[i]=s[i]; if(src_present[i]) memcpy(s[i],src_saved[i],shard_size); else memset(s[i],0,shard_size); }
        for(size_t r=0;r<R;r++){ rpp[r]=rp[r]; if(rep_present[r]) memcpy(rp[r],rep_saved[r],shard_size); else memset(rp[r],0,shard_size); }

        int rc = nb_rs_recover(K, R, sp, src_present, rpp, rep_present, shard_size);

        if(survivors >= K){
            /* 应成功且源分片完全还原 */
            if(rc!=0){
                printf("  [FAIL] K=%zu R=%zu sz=%zu lost=0x%x survivors=%zu -> recover returned %d (expected 0)\n",
                    K,R,shard_size,lost,survivors,rc); fails++; continue;
            }
            for(size_t i=0;i<K;i++){
                if(memcmp(s[i], src_saved[i], shard_size)!=0){
                    printf("  [FAIL] K=%zu R=%zu sz=%zu lost=0x%x src[%zu] MISMATCH after recover\n",
                        K,R,shard_size,lost,i); fails++; break;
                }
            }
        } else {
            /* 幸存不足,必须失败(不能谎报成功) */
            if(rc==0){
                printf("  [FAIL] K=%zu R=%zu sz=%zu lost=0x%x survivors=%zu -> recover returned 0 (expected fail)\n",
                    K,R,shard_size,lost,survivors); fails++;
            }
        }
    }
    return fails;
}

int main(void){
    nb_rs_init();
    struct { size_t K, R; } cfgs[] = { {4,2}, {8,2}, {6,2}, {2,2}, {1,2}, {3,2} };
    size_t sizes[] = { 1, 7, 8, 15, 960, 1024 };
    int total_fail = 0, total_case = 0;

    for(size_t c=0;c<sizeof(cfgs)/sizeof(cfgs[0]);c++){
        for(size_t z=0;z<sizeof(sizes)/sizeof(sizes[0]);z++){
            for(unsigned seed=1; seed<=4; seed++){
                int f = test_config(cfgs[c].K, cfgs[c].R, sizes[z], seed);
                total_fail += f; total_case++;
                if(f==0)
                    printf("  [ok]   K=%zu R=%zu sz=%zu seed=%u : all erasure patterns pass\n",
                        cfgs[c].K, cfgs[c].R, sizes[z], seed);
            }
        }
    }
    printf("\n==== %s : %d/%d config-cases passed, %d failures ====\n",
        total_fail==0 ? "PASS" : "FAIL",
        total_case - (total_fail?1:0)*0 - 0, total_case, total_fail);
    /* 简洁总结 */
    printf("RESULT: %s\n", total_fail==0 ? "ALL_PASS" : "HAS_FAILURE");
    return total_fail==0 ? 0 : 1;
}
