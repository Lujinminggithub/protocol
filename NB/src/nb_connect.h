#ifndef NB_CONNECT_H
#define NB_CONNECT_H

#include <stdint.h>

/* 建连截止时间为开始时间后 5 秒；开始时间 0 或 UINT64_MAX 表示未开始，溢出时饱和。 */
uint64_t nb_connect_deadline_us(uint64_t started_at_us);
/* 仅已开始且当前时间达到截止时间时返回 1。 */
int nb_connect_expired(uint64_t started_at_us, uint64_t now_us);

#endif
