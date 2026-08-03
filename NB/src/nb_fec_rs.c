#include "nb_fec_rs.h"

#include <pthread.h>
#include <string.h>

#define NB_RS_GF_SIZE 256
#define NB_RS_PRIM_POLY 0x11d

static uint8_t g_rs_log[NB_RS_GF_SIZE];
static uint8_t g_rs_exp[NB_RS_GF_SIZE * 2];
static pthread_once_t g_rs_once = PTHREAD_ONCE_INIT;

static uint8_t gf_add(uint8_t a, uint8_t b){ return (uint8_t)(a ^ b); }

static uint8_t gf_mul(uint8_t a, uint8_t b){
    if(a == 0 || b == 0) return 0;
    return g_rs_exp[g_rs_log[a] + g_rs_log[b]];
}

static uint8_t gf_inv(uint8_t a){
    if(a == 0) return 0;
    return g_rs_exp[255 - g_rs_log[a]];
}

static uint8_t gf_pow_alpha(size_t p){
    return g_rs_exp[p % 255];
}

static void rs_init_once(void){
    uint16_t x = 1;
    for(int i=0;i<255;i++){
        g_rs_exp[i] = (uint8_t)x;
        g_rs_log[x] = (uint8_t)i;
        x <<= 1;
        if(x & 0x100) x ^= NB_RS_PRIM_POLY;
    }
    for(int i=255;i<NB_RS_GF_SIZE * 2;i++) g_rs_exp[i] = g_rs_exp[i - 255];
    g_rs_log[0] = 0;
}

int nb_rs_init(void){return pthread_once(&g_rs_once,rs_init_once);}

static uint8_t coeff_for(size_t repair_row, size_t src_col){
    /* Vandermonde: row j uses alpha^((j+1)*col), col 0 => 1 */
    return gf_pow_alpha((repair_row + 1) * src_col);
}

int nb_rs_encode(size_t src_count, size_t repair_count,
    uint8_t* const* src, uint8_t* const* repair, size_t shard_size)
{
    if(nb_rs_init() != 0) return -1;
    if(src_count == 0 || repair_count == 0) return 0;
    for(size_t r=0;r<repair_count;r++) memset(repair[r], 0, shard_size);
    for(size_t r=0;r<repair_count;r++){
        for(size_t c=0;c<src_count;c++){
            uint8_t coef = coeff_for(r, c);
            for(size_t i=0;i<shard_size;i++){
                repair[r][i] = gf_add(repair[r][i], gf_mul(coef, src[c][i]));
            }
        }
    }
    return 0;
}

static void matrix_identity(uint8_t* m, size_t n){
    memset(m, 0, n * n);
    for(size_t i=0;i<n;i++) m[i * n + i] = 1;
}

static int matrix_invert(uint8_t* a, uint8_t* inv, size_t n){
    matrix_identity(inv, n);
    for(size_t col=0; col<n; col++){
        size_t pivot = col;
        while(pivot < n && a[pivot * n + col] == 0) pivot++;
        if(pivot == n) return -1;
        if(pivot != col){
            for(size_t k=0;k<n;k++){
                uint8_t t = a[col * n + k]; a[col * n + k] = a[pivot * n + k]; a[pivot * n + k] = t;
                t = inv[col * n + k]; inv[col * n + k] = inv[pivot * n + k]; inv[pivot * n + k] = t;
            }
        }
        uint8_t piv = a[col * n + col];
        uint8_t piv_inv = gf_inv(piv);
        if(piv_inv == 0) return -1;
        for(size_t k=0;k<n;k++){
            a[col * n + k] = gf_mul(a[col * n + k], piv_inv);
            inv[col * n + k] = gf_mul(inv[col * n + k], piv_inv);
        }
        for(size_t row=0; row<n; row++){
            if(row == col) continue;
            uint8_t factor = a[row * n + col];
            if(factor == 0) continue;
            for(size_t k=0;k<n;k++){
                a[row * n + k] = gf_add(a[row * n + k], gf_mul(factor, a[col * n + k]));
                inv[row * n + k] = gf_add(inv[row * n + k], gf_mul(factor, inv[col * n + k]));
            }
        }
    }
    return 0;
}

int nb_rs_recover(size_t src_count, size_t repair_count,
    uint8_t* const* src, const int* src_present,
    uint8_t* const* repair, const int* repair_present,
    size_t shard_size)
{
    if(nb_rs_init() != 0) return -1;
    if(src_count == 0) return 0;

    size_t available = 0;
    for(size_t i=0;i<src_count;i++) available += src_present[i] ? 1u : 0u;
    for(size_t i=0;i<repair_count;i++) available += repair_present[i] ? 1u : 0u;
    if(available < src_count) return -1;

    uint8_t rows[32 * 32];
    uint8_t inv[32 * 32];
    const uint8_t* shard_ptrs[32];
    if(src_count > 32) return -1;

    size_t eq = 0;
    for(size_t i=0;i<src_count && eq<src_count;i++){
        if(!src_present[i]) continue;
        memset(rows + eq * src_count, 0, src_count);
        rows[eq * src_count + i] = 1;
        shard_ptrs[eq] = src[i];
        eq++;
    }
    for(size_t r=0;r<repair_count && eq<src_count;r++){
        if(!repair_present[r]) continue;
        for(size_t c=0;c<src_count;c++) rows[eq * src_count + c] = coeff_for(r, c);
        shard_ptrs[eq] = repair[r];
        eq++;
    }
    if(eq < src_count) return -1;

    uint8_t work[32 * 32];
    memcpy(work, rows, src_count * src_count);
    if(matrix_invert(work, inv, src_count) != 0) return -1;

    for(size_t miss=0; miss<src_count; miss++){
        if(src_present[miss]) continue;
        memset(src[miss], 0, shard_size);
        for(size_t eqi=0; eqi<src_count; eqi++){
            uint8_t coef = inv[miss * src_count + eqi];
            if(coef == 0) continue;
            for(size_t b=0;b<shard_size;b++){
                src[miss][b] = gf_add(src[miss][b], gf_mul(coef, shard_ptrs[eqi][b]));
            }
        }
    }
    return 0;
}
