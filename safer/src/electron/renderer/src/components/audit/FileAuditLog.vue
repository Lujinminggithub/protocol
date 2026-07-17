<script setup lang="ts">
import DeliveryOperations from '@/components/audit/DeliveryOperations.vue'
import { useAuditStore } from '@/stores/audit'
import { useAudit } from '@/composables/useAudit'
import { onMounted, ref, computed } from 'vue'

const store = useAuditStore()
const { init } = useAudit()
const searchQuery = ref('')
const filterType = ref<string>('all')

function isNetworkEvent(type: string): boolean {
  return type.startsWith('network') ||
    type.startsWith('http') ||
    type.startsWith('ftp') ||
    type.startsWith('sni') ||
    type.startsWith('websocket')
}

const filteredLogs = computed(() => {
  let logs = store.logs
  if (filterType.value !== 'all') {
    logs = logs.filter(log => filterType.value === 'network'
      ? isNetworkEvent(log.type)
      : log.type.startsWith(filterType.value))
  }
  if (searchQuery.value) {
    const q = searchQuery.value.toLowerCase()
    logs = logs.filter(log =>
      log.processName.toLowerCase().includes(q) ||
      log.details.toLowerCase().includes(q) ||
      log.type.includes(q)
    )
  }
  return logs
})

onMounted(() => {
  init()
})
</script>

<template>
  <div class="audit-panel">
    <DeliveryOperations />

    <div class="audit-header">
      <h2 class="section-title">审计日志</h2>
      <div class="audit-controls">
        <select v-model="filterType" class="filter-select">
          <option value="all">全部类型</option>
          <option value="file">文件事件</option>
          <option value="network">网络事件</option>
          <option value="registry">注册表事件</option>
        </select>
        <input
          v-model="searchQuery"
          type="text"
          placeholder="搜索日志..."
          class="search-input"
        />
        <button class="clear-btn" @click="store.clearLogs()">清空日志</button>
      </div>
    </div>

    <!-- 统计卡片 -->
    <div class="audit-stats">
      <div class="audit-stat-card">
        <div class="stat-num">{{ store.stats.totalEvents }}</div>
        <div class="stat-label">总事件</div>
      </div>
      <div class="audit-stat-card">
        <div class="stat-num">{{ store.stats.fileEvents }}</div>
        <div class="stat-label">文件事件</div>
      </div>
      <div class="audit-stat-card">
        <div class="stat-num">{{ store.stats.networkEvents }}</div>
        <div class="stat-label">网络事件</div>
      </div>
      <div class="audit-stat-card">
        <div class="stat-num">{{ store.stats.blockedEvents }}</div>
        <div class="stat-label">已拦截</div>
      </div>
    </div>

    <!-- 日志表格 -->
    <div class="log-table-wrapper">
      <table class="log-table">
        <thead>
          <tr>
            <th v-if="!store.loading">时间</th>
            <th v-else>加载中...</th>
            <th>类型</th>
            <th>进程</th>
            <th>PID</th>
            <th>操作</th>
            <th>详情</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="log in filteredLogs" :key="log.id">
            <td class="time-cell">{{ new Date(log.timestamp).toLocaleString('zh-CN') }}</td>
            <td><span class="type-badge">{{ store.getTypeLabel(log.type) }}</span></td>
            <td>{{ log.processName }}</td>
            <td>{{ log.pid }}</td>
            <td><span class="action-badge" :class="log.action">{{ store.getActionBadge(log.action) }}</span></td>
            <td class="details-cell">{{ log.details }}</td>
          </tr>
          <tr v-if="filteredLogs.length === 0">
            <td :colspan="7" class="empty-state">暂无审计日志</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<style scoped>
.audit-panel {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.audit-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.section-title {
  font-size: 16px;
  font-weight: 600;
  color: #ccc;
  margin: 0;
}

.audit-controls {
  display: flex;
  gap: 12px;
  align-items: center;
}

.filter-select {
  padding: 6px 12px;
  background: #16213e;
  border: 1px solid #1a3a5c;
  border-radius: 6px;
  color: #ccc;
  font-size: 13px;
  outline: none;
}

.filter-select:focus {
  border-color: #00d4ff;
}

.search-input {
  padding: 6px 12px;
  background: #16213e;
  border: 1px solid #1a3a5c;
  border-radius: 6px;
  color: #ccc;
  font-size: 13px;
  width: 200px;
  outline: none;
}

.search-input:focus {
  border-color: #00d4ff;
}

.clear-btn {
  padding: 6px 12px;
  background: #2d1a1a;
  border: 1px solid #dc2626;
  border-radius: 6px;
  color: #ff6b6b;
  cursor: pointer;
  font-size: 13px;
}

.clear-btn:hover {
  background: #dc2626;
  color: #fff;
}

.audit-stats {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 12px;
}

.audit-stat-card {
  padding: 14px;
  background: #16213e;
  border: 1px solid #1a3a5c;
  border-radius: 10px;
  text-align: center;
}

.stat-num {
  font-size: 24px;
  font-weight: 700;
  color: #00d4ff;
}

.stat-label {
  font-size: 12px;
  color: #888;
  margin-top: 4px;
}

.log-table-wrapper {
  background: #16213e;
  border: 1px solid #1a3a5c;
  border-radius: 10px;
  overflow-x: auto;
}

.log-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 13px;
}

.log-table th {
  padding: 10px 12px;
  text-align: left;
  background: #1a2744;
  color: #888;
  font-weight: 500;
  font-size: 12px;
  position: sticky;
  top: 0;
}

.log-table td {
  padding: 10px 12px;
  color: #ccc;
  border-top: 1px solid #1a3a5c;
}

.time-cell {
  color: #888;
  font-size: 12px;
  white-space: nowrap;
}

.type-badge {
  display: inline-block;
  padding: 2px 8px;
  background: #0f3460;
  border-radius: 4px;
  font-size: 12px;
  color: #4ade80;
}

.action-badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 12px;
}

.action-badge.allowed {
  background: #0f3460;
  color: #4ade80;
}

.action-badge.blocked {
  background: #2d1a1a;
  color: #ff6b6b;
}

.action-badge.logged {
  background: #1a1a40;
  color: #818cf8;
}

.details-cell {
  color: #888;
  font-size: 12px;
  max-width: 300px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.empty-state {
  text-align: center;
  color: #666;
  padding: 40px !important;
}
</style>
