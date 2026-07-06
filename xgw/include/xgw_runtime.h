#ifndef XGW_RUNTIME_H
#define XGW_RUNTIME_H

/* 单节点运行主循环入口。 */

#include "xgw_config.h"

#include <stddef.h>

/* 启动节点主循环。 */
int xgw_runtime_run(const xgw_runtime_config_t *config, char *error, size_t error_len);

/* 调度器聚合自检：随机状态对拍 O(N) 快路径与 O(N^2) _ref 实现，逐值等价返回 0。 */
int xgw_runtime_sched_selftest(char *error, size_t error_len);

#endif
