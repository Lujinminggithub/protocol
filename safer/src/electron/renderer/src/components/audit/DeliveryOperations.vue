<script setup lang="ts">
import StatusBadge from '@/components/common/StatusBadge.vue'
import { computed, onMounted, ref, watch } from 'vue'

const loading = ref(false)
const busy = ref(false)
const dirty = ref(false)
const status = ref<any>(null)
const checkpoints = ref<any[]>([])
const archivedCheckpoints = ref<any[]>([])
const deadLetterQueue = ref<any[]>([])
const deadLetterRecords = ref<any[]>([])
const deliveredRecords = ref<any[]>([])
const pipelineHistory = ref<any[]>([])
const feedback = ref('')
const configText = ref('{}')

const pipelineSummary = computed(() => {
  const sinks = status.value?.pipeline?.sinks || []
  return sinks.map((sink: any) => `${sink.id}:${sink.kind}${sink.enabled ? '' : '(off)'}`).join(', ')
})

const backpressureEntries = computed(() => {
  const source = status.value?.sinkBackpressure || {}
  return Object.values(source) as any[]
})

const checkpointSummary = computed(() => {
  const items = checkpoints.value
  return {
    inflight: items.filter((item) => item.state === 'inflight').length,
    partial: items.filter((item) => item.state === 'partial').length,
    completed: items.filter((item) => item.state === 'completed').length,
    recovered: items.filter((item) => item.state === 'recovered').length,
  }
})

const latestReportHint = computed(() => {
  const info = status.value?.deliveredQueue || {}
  return info?.reportsDir || '-'
})

watch(
  () => status.value?.pipeline,
  (pipeline) => {
    if (!dirty.value && pipeline) {
      configText.value = JSON.stringify(pipeline, null, 2)
    }
  },
  { deep: true }
)

function checkpointTone(state: string): 'info' | 'warning' | 'success' | 'error' {
  if (state === 'completed') return 'success'
  if (state === 'recovered') return 'info'
  if (state === 'partial' || state === 'inflight') return 'warning'
  return 'error'
}

function formatTime(value?: string): string {
  if (!value) return '-'
  return new Date(value).toLocaleString('zh-CN')
}

function sinkStatusSummary(checkpoint: any): string {
  const values = Object.entries(checkpoint?.sinkStatuses || {})
  if (values.length === 0) return '-'
  return values
    .map(([sinkId, sinkStatus]: [string, any]) => `${sinkId}:${sinkStatus.state}[ack:${sinkStatus.deliveredEventIds?.length || 0}/retry:${sinkStatus.retryEventIds?.length || 0}/drop:${sinkStatus.droppedEventIds?.length || 0}]`)
    .join(' | ')
}

function sinkStateSummary(item: any): string {
  const values = Object.entries(item?.sinkStates || {})
  if (values.length === 0) return '-'
  return values
    .map(([sinkId, sinkState]: [string, any]) => `${sinkId}:${sinkState.state}`)
    .join(' | ')
}

async function refresh() {
  loading.value = true
  try {
    const [
      deliveryStatus,
      checkpointData,
      archivedData,
      deadQueue,
      deadRecords,
      delivered,
      history,
    ] = await Promise.all([
      window.saferAPI.audit.getDeliveryStatus(),
      window.saferAPI.audit.getDeliveryCheckpoints(50),
      window.saferAPI.audit.getArchivedCheckpoints(20),
      window.saferAPI.audit.getDeadLetterQueue(50),
      window.saferAPI.audit.getDeadLetterRecords(20),
      window.saferAPI.audit.getDeliveredRecords(20),
      window.saferAPI.audit.getDeliveryPipelineHistory(20),
    ])
    status.value = deliveryStatus
    checkpoints.value = checkpointData
    archivedCheckpoints.value = archivedData
    deadLetterQueue.value = deadQueue
    deadLetterRecords.value = deadRecords
    deliveredRecords.value = delivered
    pipelineHistory.value = history
  } finally {
    loading.value = false
  }
}

async function runAction(action: () => Promise<any>, successText: string, formatResult?: (result: any) => string) {
  busy.value = true
  feedback.value = ''
  try {
    const result = await action()
    feedback.value = formatResult ? formatResult(result) : successText
    await refresh()
  } catch (error: any) {
    feedback.value = error?.message || String(error)
  } finally {
    busy.value = false
  }
}

async function savePipelineConfig() {
  let parsed: any
  try {
    parsed = JSON.parse(configText.value)
  } catch (error: any) {
    feedback.value = `配置 JSON 解析失败: ${error?.message || error}`
    return
  }

  await runAction(
    () => window.saferAPI.audit.setDeliverySinkConfig({
      ...parsed,
      historyReason: 'ui-pipeline-save',
    }),
    'Pipeline 配置已保存'
  )
  dirty.value = false
}

async function exportReport() {
  await runAction(
    () => window.saferAPI.audit.exportDeliveryReport('ui-export'),
    '报告已导出',
    (result) => `报告已导出: ${result?.path || ''}`
  )
}

const startWorker = () => runAction(
  () => window.saferAPI.audit.startDeliveryWorker(),
  'delivery worker 已启动'
)
const stopWorker = () => runAction(
  () => window.saferAPI.audit.stopDeliveryWorker(),
  'delivery worker 已停止'
)
const requeueDeadLetters = () => runAction(
  () => window.saferAPI.audit.requeueAbandonedEvents(100),
  '已重放 abandoned 事件'
)
const clearBackpressure = () => runAction(
  () => window.saferAPI.audit.clearDeliveryBackpressure(),
  '已清理 backpressure'
)
const clearCheckpoints = () => runAction(
  () => window.saferAPI.audit.clearCompletedCheckpoints(),
  '已清理已完成 checkpoint'
)

function resetConfigEditor() {
  configText.value = JSON.stringify(status.value?.pipeline || {}, null, 2)
  dirty.value = false
  feedback.value = '配置编辑器已重置为当前生效配置'
}

onMounted(() => {
  void refresh()
})
</script>

<template>
  <section class="ops-panel">
    <div class="ops-header">
      <div>
        <h3 class="ops-title">投递运维</h3>
        <p class="ops-subtitle">Worker、checkpoint、dead letter、pipeline 和导出操作台</p>
      </div>
      <div class="ops-actions">
        <button class="ops-btn secondary" :disabled="loading || busy" @click="refresh()">刷新</button>
        <button class="ops-btn" :disabled="loading || busy" @click="startWorker">启动 Worker</button>
        <button class="ops-btn danger" :disabled="loading || busy" @click="stopWorker">停止 Worker</button>
      </div>
    </div>

    <div v-if="feedback" class="feedback">{{ feedback }}</div>

    <div class="card-grid">
      <div class="stat-card">
        <div class="card-label">Worker</div>
        <StatusBadge :status="status?.running ? 'success' : 'warning'" :text="status?.running ? '运行中' : '已停止'" />
        <div class="card-meta">interval {{ status?.intervalMs || 0 }} ms / batch {{ status?.batchSize || 0 }}</div>
      </div>
      <div class="stat-card">
        <div class="card-label">队列</div>
        <div class="card-value">{{ status?.queueDepth || 0 }}</div>
        <div class="card-meta">pending {{ status?.pendingCount || 0 }} / sending {{ status?.sendingCount || 0 }} / failed {{ status?.failedCount || 0 }}</div>
      </div>
      <div class="stat-card">
        <div class="card-label">结果</div>
        <div class="card-value">{{ status?.sentCount || 0 }}</div>
        <div class="card-meta">sent {{ status?.sentCount || 0 }} / abandoned {{ status?.abandonedCount || 0 }}</div>
      </div>
      <div class="stat-card">
        <div class="card-label">Checkpoint</div>
        <div class="card-value">{{ checkpoints.length }}</div>
        <div class="card-meta">inflight {{ checkpointSummary.inflight }} / partial {{ checkpointSummary.partial }}</div>
      </div>
    </div>

    <div class="ops-toolbar">
      <div class="toolbar-block">
        <span class="toolbar-label">Pipeline</span>
        <span class="toolbar-text">{{ pipelineSummary || '-' }}</span>
      </div>
      <div class="toolbar-actions">
        <button class="ops-btn secondary" :disabled="busy" @click="requeueDeadLetters">重放 Dead Letter</button>
        <button class="ops-btn secondary" :disabled="busy" @click="clearBackpressure">清理 Backpressure</button>
        <button class="ops-btn secondary" :disabled="busy" @click="clearCheckpoints">清理已完成 Checkpoint</button>
        <button class="ops-btn secondary" :disabled="busy" @click="exportReport()">导出报告</button>
      </div>
    </div>

    <div class="ops-layout">
      <div class="ops-column wide">
        <div class="panel-card">
          <div class="panel-card-header">
            <h4>Pipeline 配置编辑</h4>
            <span class="small-text">reports: {{ latestReportHint }}</span>
          </div>
          <textarea
            v-model="configText"
            class="config-editor"
            spellcheck="false"
            @input="dirty = true"
          />
          <div class="panel-card-footer">
            <span class="small-text">{{ dirty ? '有未保存修改' : '配置与当前生效状态同步' }}</span>
            <div class="toolbar-actions">
              <button class="ops-btn secondary" :disabled="busy" @click="resetConfigEditor()">重置</button>
              <button class="ops-btn" :disabled="busy" @click="savePipelineConfig()">保存 Pipeline</button>
            </div>
          </div>
        </div>

        <div class="panel-card">
          <div class="panel-card-header">
            <h4>批次 Checkpoint</h4>
            <span class="small-text">{{ checkpoints.length }} 条</span>
          </div>
          <div class="table-wrap">
            <table class="ops-table">
              <thead>
                <tr>
                  <th>Batch</th>
                  <th>状态</th>
                  <th>事件</th>
                  <th>Sinks</th>
                  <th>更新时间</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="checkpoint in checkpoints" :key="checkpoint.batchId">
                  <td class="mono">{{ checkpoint.batchId }}</td>
                  <td><StatusBadge :status="checkpointTone(checkpoint.state)" :text="checkpoint.state" /></td>
                  <td>{{ checkpoint.eventIds?.length || 0 }}</td>
                  <td class="details">{{ sinkStatusSummary(checkpoint) }}</td>
                  <td>{{ formatTime(checkpoint.updatedAt) }}</td>
                </tr>
                <tr v-if="checkpoints.length === 0">
                  <td colspan="5" class="empty">暂无 checkpoint</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>

        <div class="panel-card">
          <div class="panel-card-header">
            <h4>Dead Letter 队列</h4>
            <span class="small-text">{{ deadLetterQueue.length }} 条</span>
          </div>
          <div class="table-wrap">
            <table class="ops-table">
              <thead>
                <tr>
                  <th>ID</th>
                  <th>类型</th>
                  <th>进程</th>
                  <th>错误</th>
                  <th>Sink 状态</th>
                </tr>
              </thead>
              <tbody>
                <tr v-for="event in deadLetterQueue" :key="event.id">
                  <td class="mono">{{ event.id }}</td>
                  <td>{{ event.type }}</td>
                  <td>{{ event.processName }}</td>
                  <td class="details">{{ event.error || '-' }}</td>
                  <td class="details">{{ sinkStateSummary(event) }}</td>
                </tr>
                <tr v-if="deadLetterQueue.length === 0">
                  <td colspan="5" class="empty">暂无 dead letter</td>
                </tr>
              </tbody>
            </table>
          </div>
        </div>
      </div>

      <div class="ops-column">
        <div class="panel-card">
          <div class="panel-card-header">
            <h4>Backpressure</h4>
            <span class="small-text">{{ backpressureEntries.length }} 项</span>
          </div>
          <div class="list-wrap">
            <div v-for="entry in backpressureEntries" :key="entry.sinkId" class="list-item">
              <div class="list-title">{{ entry.sinkId }}</div>
              <div class="list-sub">暂停到 {{ formatTime(entry.pausedUntil) }}</div>
              <div class="list-sub">max batch {{ entry.maxBatchSize || '-' }}</div>
              <div class="list-sub">{{ entry.reason || '-' }}</div>
            </div>
            <div v-if="backpressureEntries.length === 0" class="empty">暂无 backpressure</div>
          </div>
        </div>

        <div class="panel-card">
          <div class="panel-card-header">
            <h4>配置历史</h4>
            <span class="small-text">{{ pipelineHistory.length }} 条</span>
          </div>
          <div class="list-wrap">
            <div v-for="item in pipelineHistory" :key="`${item.changedAt}-${item.reason}`" class="list-item">
              <div class="list-title">{{ item.reason }}</div>
              <div class="list-sub">{{ formatTime(item.changedAt) }}</div>
              <div class="list-sub mono">{{ (item.pipeline?.sinks || []).map((sink: any) => sink.id).join(', ') || '-' }}</div>
            </div>
            <div v-if="pipelineHistory.length === 0" class="empty">暂无配置历史</div>
          </div>
        </div>

        <div class="panel-card">
          <div class="panel-card-header">
            <h4>归档 Checkpoint</h4>
            <span class="small-text">{{ archivedCheckpoints.length }} 条</span>
          </div>
          <div class="list-wrap">
            <div v-for="item in archivedCheckpoints" :key="`${item.archivedAt}-${item.checkpoint?.batchId}`" class="list-item">
              <div class="list-title">{{ item.checkpoint?.batchId }}</div>
              <div class="list-sub">{{ formatTime(item.archivedAt) }}</div>
              <div class="list-sub">{{ item.reason }}</div>
            </div>
            <div v-if="archivedCheckpoints.length === 0" class="empty">暂无归档 checkpoint</div>
          </div>
        </div>

        <div class="panel-card">
          <div class="panel-card-header">
            <h4>最近 Delivered</h4>
            <span class="small-text">{{ deliveredRecords.length }} 条</span>
          </div>
          <div class="list-wrap">
            <div v-for="record in deliveredRecords" :key="`${record.deliveredAt}-${record.event?.id}`" class="list-item">
              <div class="list-title">#{{ record.event?.id }} {{ record.event?.type }}</div>
              <div class="list-sub">{{ formatTime(record.deliveredAt) }}</div>
              <div class="list-sub mono">{{ record.event?.processName }}</div>
            </div>
            <div v-if="deliveredRecords.length === 0" class="empty">暂无 delivered 记录</div>
          </div>
        </div>

        <div class="panel-card">
          <div class="panel-card-header">
            <h4>最近 Dead Letter 记录</h4>
            <span class="small-text">{{ deadLetterRecords.length }} 条</span>
          </div>
          <div class="list-wrap">
            <div v-for="record in deadLetterRecords" :key="`${record.abandonedAt}-${record.event?.id}`" class="list-item">
              <div class="list-title">#{{ record.event?.id }} {{ record.decision }}</div>
              <div class="list-sub">{{ formatTime(record.abandonedAt) }}</div>
              <div class="list-sub details">{{ record.error }}</div>
            </div>
            <div v-if="deadLetterRecords.length === 0" class="empty">暂无 dead-letter 记录</div>
          </div>
        </div>
      </div>
    </div>
  </section>
</template>

<style scoped>
.ops-panel {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.ops-header,
.ops-toolbar,
.panel-card-header,
.panel-card-footer {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 12px;
}

.ops-title,
.panel-card-header h4 {
  margin: 0;
  color: #d8e6ff;
}

.ops-subtitle,
.small-text,
.toolbar-label,
.toolbar-text,
.card-label,
.card-meta,
.list-sub,
.empty {
  color: #8ba2c7;
}

.ops-subtitle {
  margin: 4px 0 0;
  font-size: 13px;
}

.ops-actions,
.toolbar-actions {
  display: flex;
  gap: 10px;
  flex-wrap: wrap;
}

.ops-btn {
  border: 1px solid #2a8a9d;
  background: #123a44;
  color: #bff6ff;
  border-radius: 8px;
  padding: 8px 14px;
  cursor: pointer;
}

.ops-btn.secondary {
  border-color: #284c73;
  background: #16213e;
  color: #d4e7ff;
}

.ops-btn.danger {
  border-color: #7d2f38;
  background: #33161a;
  color: #ffb8c0;
}

.ops-btn:disabled {
  opacity: 0.5;
  cursor: default;
}

.feedback {
  padding: 10px 14px;
  border-radius: 10px;
  background: #16213e;
  border: 1px solid #1a3a5c;
  color: #d8e6ff;
}

.card-grid {
  display: grid;
  grid-template-columns: repeat(4, minmax(0, 1fr));
  gap: 12px;
}

.stat-card,
.panel-card {
  background: #16213e;
  border: 1px solid #1a3a5c;
  border-radius: 12px;
  padding: 14px;
}

.card-value {
  margin-top: 8px;
  font-size: 26px;
  color: #6fe6ff;
  font-weight: 700;
}

.card-meta {
  margin-top: 6px;
  font-size: 12px;
}

.ops-toolbar {
  background: #121d35;
  border: 1px solid #1a3a5c;
  border-radius: 12px;
  padding: 12px 14px;
}

.toolbar-block {
  display: flex;
  align-items: center;
  gap: 10px;
  min-width: 0;
}

.toolbar-label {
  font-weight: 600;
}

.toolbar-text {
  word-break: break-all;
}

.ops-layout {
  display: grid;
  grid-template-columns: minmax(0, 2fr) minmax(320px, 1fr);
  gap: 16px;
}

.ops-column {
  display: flex;
  flex-direction: column;
  gap: 16px;
}

.config-editor {
  width: 100%;
  min-height: 240px;
  resize: vertical;
  margin-top: 12px;
  border: 1px solid #1a3a5c;
  border-radius: 10px;
  background: #101a31;
  color: #d8e6ff;
  padding: 12px;
  font-family: Consolas, Monaco, monospace;
  font-size: 12px;
  line-height: 1.5;
}

.table-wrap {
  overflow: auto;
}

.ops-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 12px;
}

.ops-table th,
.ops-table td {
  padding: 10px 8px;
  border-top: 1px solid #1a3a5c;
  text-align: left;
  vertical-align: top;
}

.ops-table th {
  color: #8ba2c7;
  font-weight: 600;
}

.mono {
  font-family: Consolas, Monaco, monospace;
  color: #bce7ff;
}

.details {
  color: #c5d2ec;
  word-break: break-word;
}

.list-wrap {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.list-item {
  padding: 10px 12px;
  border-radius: 10px;
  background: #101a31;
  border: 1px solid #1a3252;
}

.list-title {
  color: #d8e6ff;
  font-weight: 600;
}

.empty {
  padding: 18px 6px;
  text-align: center;
}

@media (max-width: 1100px) {
  .card-grid,
  .ops-layout {
    grid-template-columns: 1fr;
  }

  .ops-header,
  .ops-toolbar,
  .panel-card-footer {
    flex-direction: column;
    align-items: stretch;
  }
}
</style>
