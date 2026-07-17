<script setup lang="ts">
import { useAuditStore } from '@/stores/audit'
import { ref, computed, onMounted } from 'vue'

const store = useAuditStore()
const searchQuery = ref('')

const networkLogs = computed(() => {
  const logs = store.logs.filter(log =>
    log.type.startsWith('network') ||
    log.type.startsWith('http') ||
    log.type.startsWith('ftp') ||
    log.type.startsWith('sni') ||
    log.type.startsWith('websocket')
  )
  if (!searchQuery.value) return logs
  const q = searchQuery.value.toLowerCase()
  return logs.filter(log =>
    log.processName.toLowerCase().includes(q) ||
    log.details.toLowerCase().includes(q) ||
    log.type.toLowerCase().includes(q)
  )
})
</script>

<template>
  <div class="net-audit-panel">
    <div class="panel-header">
      <h3>🌐 网络审计日志</h3>
      <input
        v-model="searchQuery"
        type="text"
        placeholder="搜索网络事件..."
        class="search-input"
      />
    </div>

    <div class="log-table-wrapper">
      <table class="log-table">
        <thead>
          <tr>
            <th>时间</th>
            <th>类型</th>
            <th>进程</th>
            <th>URL/地址</th>
            <th>操作</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="log in networkLogs" :key="log.id">
            <td class="time-cell">{{ new Date(log.timestamp).toLocaleString('zh-CN') }}</td>
            <td><span class="type-badge">{{ store.getTypeLabel(log.type) }}</span></td>
            <td>{{ log.processName }}</td>
            <td class="url-cell">{{ log.details }}</td>
            <td><span class="action-badge" :class="log.action">{{ store.getActionBadge(log.action) }}</span></td>
          </tr>
          <tr v-if="networkLogs.length === 0">
            <td colspan="5" class="empty-state">暂无网络审计日志</td>
          </tr>
        </tbody>
      </table>
    </div>
  </div>
</template>

<style scoped>
.net-audit-panel {
  background: #16213e;
  border: 1px solid #1a3a5c;
  border-radius: 12px;
  overflow: hidden;
}

.panel-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 12px 16px;
  background: #1a2744;
}

.panel-header h3 {
  font-size: 14px;
  font-weight: 500;
  color: #ccc;
  margin: 0;
}

.search-input {
  padding: 6px 12px;
  background: #0f3460;
  border: 1px solid #1a3a5c;
  border-radius: 6px;
  color: #ccc;
  font-size: 12px;
  width: 200px;
  outline: none;
}

.search-input:focus {
  border-color: #00d4ff;
}

.log-table-wrapper {
  max-height: 300px;
  overflow-y: auto;
}

.log-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
}

.log-table th {
  padding: 8px 12px;
  text-align: left;
  background: #1a2744;
  color: #888;
  font-weight: 500;
  position: sticky;
  top: 0;
}

.log-table td {
  padding: 8px 12px;
  color: #ccc;
  border-top: 1px solid #1a2744;
}

.time-cell {
  color: #888;
  white-space: nowrap;
}

.type-badge {
  display: inline-block;
  padding: 2px 8px;
  background: #0f3460;
  border-radius: 4px;
  font-size: 11px;
  color: #4ade80;
}

.url-cell {
  color: #818cf8;
  font-family: 'Consolas', monospace;
  font-size: 11px;
  max-width: 250px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.action-badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 11px;
}

.action-badge.allowed { background: #0f3460; color: #4ade80; }
.action-badge.blocked { background: #2d1a1a; color: #ff6b6b; }
.action-badge.logged { background: #1a1a40; color: #818cf8; }

.empty-state {
  text-align: center;
  color: #666;
  padding: 30px !important;
}
</style>
