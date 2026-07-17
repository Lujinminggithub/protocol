/*
 * RegistryPanel.vue - 注册表监控面板
 * 显示注册表变更事件的实时列表
 */
<script setup lang="ts">
import { ref, onMounted, onUnmounted } from 'vue'

interface RegistryEvent {
  id: number
  keyPath: string
  changeType: string
  timestamp: string
  details: string
}

const events = ref<RegistryEvent[]>([])
const loading = ref(false)
let refreshTimer: ReturnType<typeof setInterval> | null = null
let eventId = 1

const changeTypeLabels: Record<string, string> = {
  value_set: '值已设置',
  value_deleted: '值已删除',
  key_created: '键已创建',
  key_deleted: '键已删除',
  name_changed: '名称已更改'
}

async function refresh() {
  loading.value = true
  try {
    // 通过 N-API 获取最近的注册表变更事件
    // TODO: 当 native addon 就绪后替换
    const mockEvent: RegistryEvent = {
      id: eventId++,
      keyPath: 'HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer',
      changeType: 'value_set',
      timestamp: new Date().toLocaleTimeString('zh-CN'),
      details: '模拟注册表变更事件'
    }
    events.value.unshift(mockEvent)
    if (events.value.length > 100) events.value.pop()
  } finally {
    loading.value = false
  }
}

onMounted(() => {
  refreshTimer = setInterval(refresh, 5000)
})

onUnmounted(() => {
  if (refreshTimer) clearInterval(refreshTimer)
})
</script>

<template>
  <div class="registry-panel">
    <div class="panel-header">
      <h3>📝 注册表监控</h3>
      <div class="panel-actions">
        <button class="btn-refresh" :disabled="loading" @click="refresh()">
          {{ loading ? '加载中...' : '刷新' }}
        </button>
      </div>
    </div>

    <div class="event-list">
      <div v-if="events.length === 0" class="empty-state">
        暂无注册表变更事件
      </div>
      <div
        v-for="event in events"
        :key="event.id"
        class="event-item"
      >
        <div class="event-type">
          <span class="type-badge" :class="event.changeType">
            {{ changeTypeLabels[event.changeType] || event.changeType }}
          </span>
        </div>
        <div class="event-content">
          <div class="event-path">{{ event.keyPath }}</div>
          <div class="event-details">{{ event.details }}</div>
        </div>
        <div class="event-time">{{ event.timestamp }}</div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.registry-panel {
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

.panel-actions {
  display: flex;
  gap: 8px;
}

.btn-refresh {
  padding: 4px 12px;
  background: #0f3460;
  border: 1px solid #1a3a5c;
  border-radius: 4px;
  color: #ccc;
  cursor: pointer;
  font-size: 12px;
}

.btn-refresh:hover:not(:disabled) {
  background: #1a4a80;
}

.btn-refresh:disabled {
  opacity: 0.5;
  cursor: not-allowed;
}

.event-list {
  max-height: 300px;
  overflow-y: auto;
}

.empty-state {
  padding: 40px;
  text-align: center;
  color: #666;
  font-size: 13px;
}

.event-item {
  display: flex;
  align-items: flex-start;
  gap: 12px;
  padding: 10px 16px;
  border-bottom: 1px solid #1a2744;
  font-size: 12px;
}

.event-item:last-child {
  border-bottom: none;
}

.type-badge {
  display: inline-block;
  padding: 2px 8px;
  border-radius: 4px;
  font-size: 11px;
  white-space: nowrap;
}

.type-badge.value_set {
  background: #0f3460;
  color: #4ade80;
}

.type-badge.value_deleted {
  background: #2d1a1a;
  color: #ff6b6b;
}

.type-badge.key_created {
  background: #1a1a40;
  color: #818cf8;
}

.type-badge.key_deleted {
  background: #2d1a1a;
  color: #ff6b6b;
}

.event-content {
  flex: 1;
  min-width: 0;
}

.event-path {
  color: #ccc;
  font-family: 'Consolas', monospace;
  font-size: 11px;
  overflow: hidden;
  text-overflow: ellipsis;
  white-space: nowrap;
}

.event-details {
  color: #888;
  font-size: 11px;
  margin-top: 2px;
}

.event-time {
  color: #666;
  font-size: 11px;
  white-space: nowrap;
}
</style>
