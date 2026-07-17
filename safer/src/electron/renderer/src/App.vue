<script setup lang="ts">
import SystemDashboard from '@/components/dashboard/SystemDashboard.vue'
import DlpStatus from '@/components/dlp/DlpStatus.vue'
import AuditLog from '@/components/audit/FileAuditLog.vue'
import { useMonitorStore } from '@/stores/monitor'
import { useDlpStore } from '@/stores/dlp'
import { useAuditStore } from '@/stores/audit'
import { ref, onMounted, onUnmounted } from 'vue'

const activeTab = ref<'monitor' | 'dlp' | 'audit'>('monitor')

const monitorStore = useMonitorStore()
const dlpStore = useDlpStore()
const auditStore = useAuditStore()

let refreshTimer: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  // 启动监控数据自动刷新
  refreshTimer = setInterval(async () => {
    if (activeTab.value === 'monitor') {
      await monitorStore.refreshAll()
    }
  }, 2000)

  // 加载DLP状态
  dlpStore.loadStatus()
})

onUnmounted(() => {
  if (refreshTimer) {
    clearInterval(refreshTimer)
  }
})
</script>

<template>
  <div class="app-container">
    <!-- 顶部导航栏 -->
    <header class="app-header">
      <div class="header-left">
        <span class="app-icon">🛡️</span>
        <h1 class="app-title">PersonalSafer</h1>
        <span class="app-version">v1.0.0</span>
      </div>
      <nav class="header-tabs">
        <button
          class="tab-btn"
          :class="{ active: activeTab === 'monitor' }"
          @click="activeTab = 'monitor'"
        >
          <span class="tab-icon">📊</span>
          系统监控
        </button>
        <button
          class="tab-btn"
          :class="{ active: activeTab === 'dlp' }"
          @click="activeTab = 'dlp'"
        >
          <span class="tab-icon">🔒</span>
          DLP 防护
        </button>
        <button
          class="tab-btn"
          :class="{ active: activeTab === 'audit' }"
          @click="activeTab = 'audit'"
        >
          <span class="tab-icon">📋</span>
          审计日志
        </button>
      </nav>
      <div class="header-right">
        <span class="status-text">{{ dlpStore.statusText }}</span>
      </div>
    </header>

    <!-- 主内容区 -->
    <main class="app-main">
      <SystemDashboard v-if="activeTab === 'monitor'" />
      <DlpStatus v-else-if="activeTab === 'dlp'" />
      <AuditLog v-else-if="activeTab === 'audit'" />
    </main>
  </div>
</template>

<style scoped>
.app-container {
  display: flex;
  flex-direction: column;
  height: 100vh;
  background: #0f0f23;
  color: #e0e0e0;
}

.app-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 10px 20px;
  background: #1a1a2e;
  border-bottom: 1px solid #2a2a4a;
  flex-shrink: 0;
}

.header-left {
  display: flex;
  align-items: center;
  gap: 10px;
}

.app-icon {
  font-size: 24px;
}

.app-title {
  font-size: 18px;
  font-weight: 600;
  color: #00d4ff;
}

.app-version {
  font-size: 12px;
  color: #888;
  background: #2a2a4a;
  padding: 2px 8px;
  border-radius: 10px;
}

.header-tabs {
  display: flex;
  gap: 4px;
}

.tab-btn {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 8px 16px;
  border: none;
  border-radius: 6px;
  background: transparent;
  color: #aaa;
  cursor: pointer;
  font-size: 14px;
  transition: all 0.2s;
}

.tab-btn:hover {
  background: #2a2a4a;
  color: #ddd;
}

.tab-btn.active {
  background: #00d4ff;
  color: #1a1a2e;
  font-weight: 500;
}

.tab-icon {
  font-size: 16px;
}

.header-right {
  display: flex;
  align-items: center;
  gap: 10px;
}

.status-text {
  font-size: 13px;
  color: #888;
}

.app-main {
  flex: 1;
  overflow-y: auto;
  padding: 20px;
}
</style>
