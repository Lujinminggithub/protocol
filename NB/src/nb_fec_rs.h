#ifndef NB_FEC_RS_H
#define NB_FEC_RS_H

#include <stddef.h>
#include <stdint.h>

int nb_rs_init(void);

/* 系统码 RS 编码:
 * - src_count: 原始 shard 数
 * - repair_count: 修复 shard 数
 * - src: 原始 shard 指针数组
 * - repair: 修复 shard 指针数组(调用者提供已分配缓冲)
 * - shard_size: 每 shard 固定长度
 */
int nb_rs_encode(size_t src_count, size_t repair_count,
    uint8_t* const* src, uint8_t* const* repair, size_t shard_size);

/* 擦除恢复:
 * - src / src_present: 原始 shard 与是否已收到
 * - repair / repair_present: 修复 shard 与是否已收到
 * - 当返回 0 时，缺失的 src[i] 已被恢复到调用者提供的缓冲中
 */
int nb_rs_recover(size_t src_count, size_t repair_count,
    uint8_t* const* src, const int* src_present,
    uint8_t* const* repair, const int* repair_present,
    size_t shard_size);

#endif
