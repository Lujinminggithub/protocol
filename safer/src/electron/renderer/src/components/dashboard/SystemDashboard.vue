<script setup lang="ts">
import { useMonitorStore } from '@/stores/monitor'
import { useMonitor } from '@/composables/useMonitor'
import { ref, onMounted, onUnmounted, computed } from 'vue'
import EChartsRealtimeChart from './EChartsRealtimeChart.vue'
import RegistryPanel from './RegistryPanel.vue'

const store = useMonitorStore()
const { startMonitoring } = useMonitor()

const activeSubTab = ref<'charts' | 'bars'>('charts')
let refreshTimer: ReturnType<typeof setInterval> | null = null

onMounted(() => {
  startMonitoring(2000)
  // 额外刷新网络详情（TCP状态需要单独获取）
  refreshTimer = setInterval(async () => {
    try {
      await store.refreshAll()
    } catch {
      // ignore
    }
  }, 2000)
})

onUnmounted(() => {
  if (refreshTimer) clearInterval(refreshTimer)
})

// 为 ECharts 准备数据
const cpuSeriesData = computed(() => {
  const cores = store.cpuData.perCore.length
  const data: number[][] = []
  for (let i = 0; i < cores; i++) {
    data.push(store.cpuHistory.map(h => store.cpuData.perCore[i] || 0))
  }
  return data.slice(0, 8) // 最多显示8核
})

const cpuSeriesNames = computed(() =>
  Array.from({ length: Math.min(store.cpuData.perCore.length, 8) }, (_, i) => `核${i}`)
)

const memorySeriesData = computed(() => [store.memoryHistory.map(h => h.value)])
const memorySeriesNames = ['内存使用率']

const networkRxSeriesData = computed(() => {
  const net = store.networkData.interfaces[0]
  return [[...store.networkRxHistory.map(h => net?.inBytesPerSec || 0)]]
})
const networkTxSeriesData = computed(() => {
  const net = store.networkData.interfaces[0]
  return [[...store.networkTxHistory.map(h => net?.outBytesPerSec || 0)]]
})

const diskReadSeriesData = computed(() => {
  const drives = store.diskData.drives
  return drives.map(d => [
    ...store.diskReadHistory.map((_, i) => drives[i]?.readBytesPerSec || 0)
  ])
})

const diskWriteSeriesData = computed(() => {
  const drives = store.diskData.drives
  return drives.map(d => [
    ...store.diskWriteHistory.map((_, i) => drives[i]?.writeBytesPerSec || 0)
  ])
})

function formatSpeed(bytesPerSec: number): string {
  if (bytesPerSec < 1024) return `${bytesPerSec} B/s`
  if (bytesPerSec < 1024 * 1024) return `${(bytesPerSec / 1024).toFixed(1)} KB/s`
  if (bytesPerSec < 1024 * 1024 * 1024) return `${(bytesPerSec / (1024 * 1024)).toFixed(1)} MB/s`
  return `${(bytesPerSec / (1024 * 1024 * 1024)).toFixed(2)} GB/s`
}

function formatBytes(bytes: number): string {
  if (bytes === 0) return '0 B'
  const k = 1024
  const sizes = ['B', 'KB', 'MB', 'GB', 'TB']
  const i = Math.floor(Math.log(bytes) / Math.log(k))
  return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i]
}
</script>

<template>
  <div class="dashboard">
    <div v-if="store.sourceError" class="source-error" role="status">
      {{ store.sourceError }}
    </div>
    <!-- 顶部统计卡片 -->
    <div class="stats-row">
      <div class="stat-card cpu-card">
        <div class="stat-icon">🔲</div>
        <div class="stat-info">
          <div class="stat-label">CPU 使用率</div>
          <div class="stat-value">{{ store.cpuData.total.toFixed(1) }}%</div>
          <div class="stat-sub">共 {{ store.cpuData.perCore.length }} 核</div>
        </div>
      </div>
      <div class="stat-card memory-card">
        <div class="stat-icon">💾</div>
        <div class="stat-info">
          <div class="stat-label">内存使用</div>
          <div class="stat-value">{{ store.memoryData.physPercent }}%</div>
          <div class="stat-sub">{{ formatBytes(store.memoryData.physUsed) }} / {{ formatBytes(store.memoryData.physTotal) }}</div>
        </div>
      </div>
      <div class="stat-card network-card">
        <div class="stat-icon">🌐</div>
        <div class="stat-info">
          <div class="stat-label">网络流量</div>
          <div class="stat-down">↓ {{ formatSpeed(store.networkData.interfaces[0]?.inBytesPerSec || 0) }}</div>
          <div class="stat-up">↑ {{ formatSpeed(store.networkData.interfaces[0]?.outBytesPerSec || 0) }}</div>
        </div>
      </div>
      <div class="stat-card disk-card">
        <div class="stat-icon">💿</div>
        <div class="stat-info">
          <div class="stat-label">磁盘活动</div>
          <div class="stat-read">读 {{ formatSpeed(store.diskData.drives[0]?.readBytesPerSec || 0) }}</div>
          <div class="stat-write">写 {{ formatSpeed(store.diskData.drives[0]?.writeBytesPerSec || 0) }}</div>
        </div>
      </div>
    </div>

    <!-- 子标签切换 -->
    <div class="sub-tabs">
      <button
        class="sub-tab"
        :class="{ active: activeSubTab === 'charts' }"
        @click="activeSubTab = 'charts'"
      >
        📈 趋势图表
      </button>
      <button
        class="sub-tab"
        :class="{ active: activeSubTab === 'bars' }"
        @click="activeSubTab = 'bars'"
      >
        📊 实时详情
      </button>
    </div>

    <!-- 趋势图表视图 -->
    <div v-if="activeSubTab === 'charts'" class="charts-row">
      <div class="chart-panel">
        <EChartsRealtimeChart
          title="CPU 使用率趋势"
          :series-name="cpuSeriesNames"
          :data="cpuSeriesData"
          :max-data-points="60"
          :unit="'%'"
          :y-axis-max="100"
        />
      </div>

      <div class="chart-panel">
        <EChartsRealtimeChart
          title="内存使用率趋势"
          :series-name="memorySeriesNames"
          :data="memorySeriesData"
          :max-data-points="60"
          :unit="'%'"
          :y-axis-min="0"
          :y-axis-max="100"
        />
      </div>

      <div class="chart-panel">
        <EChartsRealtimeChart
          title="网络下载速率"
          :series-name="['下载速率']"
          :data="networkRxSeriesData"
          :max-data-points="60"
          :unit="' B/s'"
        />
      </div>

      <div class="chart-panel">
        <EChartsRealtimeChart
          title="网络上传速率"
          :series-name="['上传速率']"
          :data="networkTxSeriesData"
          :max-data-points="60"
          :unit="' B/s'"
        />
      </div>

      <div class="chart-panel">
        <EChartsRealtimeChart
          title="磁盘读取速率"
          :series-name="store.diskData.drives.map(d => d.displayName)"
          :data="diskReadSeriesData"
          :max-data-points="60"
          :unit="' B/s'"
        />
      </div>

      <div class="chart-panel">
        <EChartsRealtimeChart
          title="磁盘写入速率"
          :series-name="store.diskData.drives.map(d => d.displayName)"
          :data="diskWriteSeriesData"
          :max-data-points="60"
          :unit="' B/s'"
        />
      </div>
    </div>

    <!-- 实时详情视图 -->
    <div v-else class="charts-row">
      <!-- CPU 详情 -->
      <div class="chart-panel">
        <div class="panel-header">
          <span>CPU 使用率</span>
          <span class="panel-value">{{ store.cpuData.total.toFixed(1) }}%</span>
        </div>
        <div class="panel-body">
          <div class="cpu-bars">
            <div
              v-for="(usage, idx) in store.cpuData.perCore"
              :key="idx"
              class="cpu-bar-item"
            >
              <div class="cpu-bar-label">核{{ idx }}</div>
              <div class="cpu-bar-track">
                <div class="cpu-bar-fill" :style="{ width: usage + '%' }"></div>
              </div>
              <div class="cpu-bar-value">{{ usage.toFixed(1) }}%</div>
            </div>
          </div>
        </div>
      </div>

      <!-- 内存详情 -->
      <div class="chart-panel">
        <div class="panel-header">
          <span>内存使用率</span>
          <span class="panel-value">{{ store.memoryData.physPercent }}%</span>
        </div>
        <div class="panel-body">
          <div class="memory-visual">
            <div class="memory-bar-track">
              <div class="memory-bar-fill" :style="{ width: store.memoryData.physPercent + '%' }"></div>
            </div>
            <div class="memory-details">
              <div class="mem-detail">
                <span class="dot dot-used"></span>
                <span>已用 {{ formatBytes(store.memoryData.physUsed) }}</span>
              </div>
              <div class="mem-detail">
                <span class="dot dot-free"></span>
                <span>可用 {{ formatBytes(store.memoryData.physTotal - store.memoryData.physUsed) }}</span>
              </div>
              <div class="mem-detail">
                <span class="dot dot-page"></span>
                <span>页文件 {{ formatBytes(store.memoryData.pageUsed) }} / {{ formatBytes(store.memoryData.pageTotal) }}</span>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- 网络详情 -->
      <div class="chart-panel">
        <div class="panel-header">
          <span>网络流量</span>
        </div>
        <div class="panel-body">
          <div class="network-visual">
            <div class="net-stat">
              <div class="net-stat-label">下载</div>
              <div class="net-stat-value down">
                ↓ {{ formatSpeed(store.networkData.interfaces[0]?.inBytesPerSec || 0) }}
              </div>
            </div>
            <div class="net-stat">
              <div class="net-stat-label">上传</div>
              <div class="net-stat-value up">
                ↑ {{ formatSpeed(store.networkData.interfaces[0]?.outBytesPerSec || 0) }}
              </div>
            </div>
            <div class="net-connections">
              <div class="conn-item">
                <span>已建立</span>
                <strong>{{ store.networkData.interfaces[0]?.tcpConnections?.established || 0 }}</strong>
              </div>
              <div class="conn-item">
                <span>等待中</span>
                <strong>{{ store.networkData.interfaces[0]?.tcpConnections?.timeWait || 0 }}</strong>
              </div>
              <div class="conn-item">
                <span>半关闭</span>
                <strong>{{ store.networkData.interfaces[0]?.tcpConnections?.closeWait || 0 }}</strong>
              </div>
            </div>
          </div>
        </div>
      </div>

      <!-- 磁盘详情 -->
      <div class="chart-panel">
        <div class="panel-header">
          <span>磁盘 IO</span>
        </div>
        <div class="panel-body">
          <div class="disk-visual">
            <div
              v-for="drive in store.diskData.drives"
              :key="drive.name"
              class="drive-item"
            >
              <div class="drive-name">{{ drive.displayName }}</div>
              <div class="drive-stats">
                <div class="drive-stat">
                  <span class="dot dot-read"></span>
                  <span>读 {{ formatSpeed(drive.readBytesPerSec) }}</span>
                </div>
                <div class="drive-stat">
                  <span class="dot dot-write"></span>
                  <span>写 {{ formatSpeed(drive.writeBytesPerSec) }}</span>
                </div>
                <div class="drive-stat">
                  <span class="dot dot-queue"></span>
                  <span>IOPS: {{ drive.readOpsPerSec }}/{{ drive.writeOpsPerSec }}</span>
                </div>
                <div class="drive-stat">
                  <span class="dot dot-queue"></span>
                  <span>队列: {{ drive.queueDepth }}</span>
                </div>
              </div>
            </div>
          </div>
        </div>
      </div>
    </div>

    <!-- 注册表监控 -->
    <div class="registry-section">
      <RegistryPanel />
    </div>

    <!-- 最后更新时间 -->
    <div class="last-update">
      最后更新: {{ store.lastUpdate ? store.lastUpdate.toLocaleTimeString() : '--' }}
      <span v-if="store.loading" class="loading-indicator">⏳ 加载中...</span>
    </div>
  </div>
</template>

<style scoped>
.dashboard {
  display: flex;
  flex-direction: column;
  gap: 20px;
}

.source-error {
  margin-bottom: 10px;
  padding: 9px 12px;
  border: 1px solid #9b3d3d;
  background: #2b171b;
  color: #ffb4b4;
  font-size: 13px;
}

.stats-row {
  display: grid;
  grid-template-columns: repeat(4, 1fr);
  gap: 16px;
}

.stat-card {
  display: flex;
  align-items: center;
  gap: 12px;
  padding: 16px;
  border-radius: 12px;
  background: #16213e;
  border: 1px solid #1a3a5c;
}

.stat-icon {
  font-size: 28px;
}

.stat-info {
  display: flex;
  flex-direction: column;
}

.stat-label {
  font-size: 12px;
  color: #888;
  margin-bottom: 4px;
}

.stat-value {
  font-size: 22px;
  font-weight: 600;
  color: #00d4ff;
}

.stat-sub {
  font-size: 11px;
  color: #666;
  margin-top: 2px;
}

.stat-down, .stat-up {
  font-size: 14px;
  color: #aaa;
}

.stat-down { color: #4ade80; }
.stat-up { color: #f59e0b; }

.stat-read, .stat-write {
  font-size: 13px;
  color: #aaa;
}

/* 子标签 */
.sub-tabs {
  display: flex;
  gap: 8px;
}

.sub-tab {
  padding: 6px 16px;
  border: 1px solid #1a3a5c;
  border-radius: 6px;
  background: transparent;
  color: #aaa;
  cursor: pointer;
  font-size: 13px;
  transition: all 0.2s;
}

.sub-tab:hover {
  background: #1a3a5c;
  color: #ddd;
}

.sub-tab.active {
  background: #00d4ff;
  border-color: #00d4ff;
  color: #1a1a2e;
  font-weight: 500;
}

.charts-row {
  display: grid;
  grid-template-columns: repeat(2, 1fr);
  gap: 16px;
}

.chart-panel {
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
  font-size: 14px;
  font-weight: 500;
}

.panel-value {
  color: #00d4ff;
  font-weight: 600;
}

.panel-body {
  padding: 16px;
}

/* CPU bars */
.cpu-bars {
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.cpu-bar-item {
  display: flex;
  align-items: center;
  gap: 8px;
}

.cpu-bar-label {
  font-size: 11px;
  color: #888;
  width: 30px;
  text-align: right;
}

.cpu-bar-track {
  flex: 1;
  height: 8px;
  background: #0f3460;
  border-radius: 4px;
  overflow: hidden;
}

.cpu-bar-fill {
  height: 100%;
  background: linear-gradient(90deg, #00d4ff, #0066ff);
  border-radius: 4px;
  transition: width 0.3s ease;
}

.cpu-bar-value {
  font-size: 11px;
  color: #00d4ff;
  width: 40px;
}

/* Memory */
.memory-visual {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.memory-bar-track {
  height: 20px;
  background: #0f3460;
  border-radius: 10px;
  overflow: hidden;
}

.memory-bar-fill {
  height: 100%;
  background: linear-gradient(90deg, #00d4ff, #0066ff);
  border-radius: 10px;
  transition: width 0.5s ease;
}

.memory-details {
  display: flex;
  flex-direction: column;
  gap: 6px;
}

.mem-detail {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 12px;
  color: #aaa;
}

.dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  display: inline-block;
}

.dot-used { background: #00d4ff; }
.dot-free { background: #4ade80; }
.dot-page { background: #f59e0b; }
.dot-read { background: #4ade80; }
.dot-write { background: #f59e0b; }
.dot-queue { background: #8b5cf6; }

/* Network */
.network-visual {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.net-stat {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.net-stat-label {
  font-size: 12px;
  color: #888;
}

.net-stat-value {
  font-size: 16px;
  font-weight: 600;
}

.net-stat-value.down { color: #4ade80; }
.net-stat-value.up { color: #f59e0b; }

.net-connections {
  display: flex;
  gap: 16px;
  padding-top: 8px;
  border-top: 1px solid #1a3a5c;
}

.conn-item {
  display: flex;
  flex-direction: column;
  align-items: center;
  gap: 4px;
  font-size: 11px;
  color: #888;
}

.conn-item strong {
  font-size: 18px;
  color: #00d4ff;
}

/* Disk */
.disk-visual {
  display: flex;
  flex-direction: column;
  gap: 12px;
}

.drive-item {
  padding: 10px;
  background: #0f3460;
  border-radius: 8px;
}

.drive-name {
  font-size: 13px;
  font-weight: 500;
  margin-bottom: 8px;
  color: #ccc;
}

.drive-stats {
  display: flex;
  gap: 16px;
  flex-wrap: wrap;
}

.drive-stat {
  display: flex;
  align-items: center;
  gap: 4px;
  font-size: 12px;
  color: #aaa;
}

.registry-section {
  margin-top: 4px;
}

.last-update {
  font-size: 12px;
  color: #666;
  text-align: right;
  display: flex;
  justify-content: flex-end;
  align-items: center;
  gap: 8px;
}

.loading-indicator {
  color: #f59e0b;
}
</style>
