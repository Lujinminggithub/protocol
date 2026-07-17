<script setup lang="ts">
import { computed, onUnmounted, reactive, ref } from 'vue'
import { useDlpStore, type DlpPolicy } from '@/stores/dlp'
import { useDlp } from '@/composables/useDlp'

type RuleKey =
  | 'fileExtensions'
  | 'blockedDomains'
  | 'blockedUrls'
  | 'blockedFtpCommands'
  | 'blockedFtpPaths'
  | 'blockedFtpContentPatterns'
  | 'blockedHttpHeaders'
  | 'blockedHttpTrailers'
  | 'blockedHttpBodyPatterns'
  | 'blockedJsonKeys'
  | 'blockedJsonPaths'
  | 'blockedJsonValues'

const store = useDlpStore()
const { init } = useDlp()
const saving = ref(false)
const saveMessage = ref('')
const portDraft = ref('')
const quarantineEntries = ref<any[]>([])
const quarantineSearch = ref('')
const quarantineBusyId = ref<number | null>(null)
let quarantineTimer: ReturnType<typeof setInterval> | null = null
const drafts = reactive<Record<RuleKey, string>>({
  fileExtensions: '',
  blockedDomains: '',
  blockedUrls: '',
  blockedFtpCommands: '',
  blockedFtpPaths: '',
  blockedFtpContentPatterns: '',
  blockedHttpHeaders: '',
  blockedHttpTrailers: '',
  blockedHttpBodyPatterns: '',
  blockedJsonKeys: '',
  blockedJsonPaths: '',
  blockedJsonValues: ''
})

const ruleGroups: ReadonlyArray<{ key: RuleKey; label: string; placeholder: string }> = [
  { key: 'fileExtensions', label: '受控文件扩展名', placeholder: '.docx' },
  { key: 'blockedDomains', label: '阻断域名', placeholder: 'example.com' },
  { key: 'blockedUrls', label: '阻断 URL', placeholder: '/private' },
  { key: 'blockedFtpCommands', label: '阻断 FTP 命令', placeholder: 'RETR' },
  { key: 'blockedFtpPaths', label: '阻断 FTP 路径', placeholder: '/confidential' },
  { key: 'blockedFtpContentPatterns', label: 'FTP 内容字符串', placeholder: 'secret' },
  { key: 'blockedHttpHeaders', label: '阻断 HTTP Header', placeholder: 'authorization' },
  { key: 'blockedHttpTrailers', label: '阻断 HTTP Trailer', placeholder: 'digest' },
  { key: 'blockedHttpBodyPatterns', label: 'HTTP Body 内容', placeholder: 'secret' },
  { key: 'blockedJsonKeys', label: 'JSON Key', placeholder: 'password' },
  { key: 'blockedJsonPaths', label: 'JSON Path', placeholder: 'users[*].password' },
  { key: 'blockedJsonValues', label: 'JSON Value', placeholder: 'secret' }
]

const filteredQuarantineEntries = computed(() => {
  const search = quarantineSearch.value.trim().toLowerCase()
  if (!search) return quarantineEntries.value
  return quarantineEntries.value.filter((entry) =>
    String(entry.originalPath || '').toLowerCase().includes(search) ||
    String(entry.quarantinePath || '').toLowerCase().includes(search) ||
    String(entry.processName || '').toLowerCase().includes(search)
  )
})

async function loadQuarantineCatalog() {
  quarantineEntries.value = await window.saferAPI.dlp.getQuarantineCatalog({ limit: 500 })
}

async function runQuarantineAction(entry: any, action: 'restore' | 'purge') {
  const label = action === 'restore' ? '恢复到原始路径' : '永久删除隔离文件'
  if (!window.confirm(`${label}？\n${entry.originalPath}`)) return
  quarantineBusyId.value = entry.id
  saveMessage.value = ''
  try {
    const result = action === 'restore'
      ? await window.saferAPI.dlp.restoreQuarantineEntry(entry.id)
      : await window.saferAPI.dlp.purgeQuarantineEntry(entry.id)
    saveMessage.value = result?.success ? `${label}成功` : `${label}失败：${result?.error || '未知错误'}`
    await loadQuarantineCatalog()
  } finally {
    quarantineBusyId.value = null
  }
}

async function initializeView() {
  await init()
  await loadQuarantineCatalog()
  quarantineTimer = setInterval(() => { void loadQuarantineCatalog() }, 5000)
}

function formatFileSize(value: number | null): string {
  if (typeof value !== 'number' || !Number.isFinite(value)) return '-'
  if (value < 1024) return `${value} B`
  if (value < 1024 * 1024) return `${(value / 1024).toFixed(1)} KB`
  return `${(value / (1024 * 1024)).toFixed(1)} MB`
}

void initializeView()

onUnmounted(() => {
  if (quarantineTimer) clearInterval(quarantineTimer)
})

async function toggleLoadDriver() {
  saveMessage.value = ''
  if (store.status.driverLoaded) {
    await store.unloadDriver()
  } else {
    const result = await store.loadDriver()
    if (!result?.success) {
      saveMessage.value = result?.error || '驱动加载失败'
    } else if (result?.warning) {
      saveMessage.value = `驱动已加载，但代理启动失败：${result.warning}`
    }
  }
}

function addRule(key: RuleKey) {
  const value = drafts[key].trim()
  if (!value || store.policy[key].includes(value)) return
  store.policy[key] = [...store.policy[key], value]
  drafts[key] = ''
}

function removeRule(key: RuleKey, index: number) {
  store.policy[key] = store.policy[key].filter((_item, itemIndex) => itemIndex !== index)
}

function addPort() {
  const port = Number(portDraft.value)
  if (!Number.isInteger(port) || port < 1 || port > 65535 || store.policy.blockedPorts.includes(port)) return
  store.policy.blockedPorts = [...store.policy.blockedPorts, port]
  portDraft.value = ''
}

function removePort(index: number) {
  store.policy.blockedPorts = store.policy.blockedPorts.filter((_item, itemIndex) => itemIndex !== index)
}

async function savePolicy() {
  saving.value = true
  saveMessage.value = ''
  try {
    const policy: DlpPolicy = {
      ...store.policy,
      fileActions: { ...store.policy.fileActions },
      networkActions: { ...store.policy.networkActions }
    }
    const result = await store.savePolicy(policy)
    saveMessage.value = result?.success
      ? (result?.warning || '策略已保存并通过内核回读验证')
      : (result?.error || '策略保存失败')
  } catch (error: any) {
    saveMessage.value = error?.message || '策略保存失败'
  } finally {
    saving.value = false
  }
}
</script>

<template>
  <div class="dlp-panel">
    <section class="status-section">
      <div class="section-heading">
        <h2>DLP 防护状态</h2>
        <button class="driver-button" :class="{ danger: store.status.driverLoaded }" :disabled="store.loading" @click="toggleLoadDriver">
          {{ store.status.driverLoaded ? '卸载驱动' : '加载驱动' }}
        </button>
      </div>

      <div class="status-grid">
        <div class="status-card">
          <span class="status-dot" :class="{ active: store.status.driverLoaded }" />
          <div>
            <div class="status-label">内核驱动</div>
            <div class="status-value">{{ store.status.driverLoaded ? '已加载' : '未加载' }}</div>
          </div>
        </div>
        <div class="status-card">
          <span class="status-dot" :class="{ active: store.status.fileFilterActive }" />
          <div>
            <div class="status-label">文件过滤</div>
            <div class="status-value">{{ store.status.fileFilterActive ? '监控中' : '未激活' }}</div>
          </div>
        </div>
        <div class="status-card">
          <span class="status-dot" :class="{ active: store.status.networkFilterActive }" />
          <div>
            <div class="status-label">网络过滤</div>
            <div class="status-value">{{ store.status.networkFilterActive ? '监控中' : '未激活' }}</div>
          </div>
        </div>
      </div>
    </section>

    <section class="policy-section">
      <div class="section-heading">
        <h2>防护策略</h2>
        <button class="save-button" :disabled="saving" @click="savePolicy">
          {{ saving ? '保存中' : '保存策略' }}
        </button>
      </div>

      <div class="switch-grid">
        <label class="switch-row">
          <input v-model="store.policy.fileFilterEnabled" type="checkbox">
          <span>文件过滤防护</span>
        </label>
        <label class="switch-row">
          <input v-model="store.policy.networkFilterEnabled" type="checkbox">
          <span>网络过滤防护</span>
        </label>
        <label class="switch-row compact">
          <input v-model="store.policy.auditEnabled" type="checkbox">
          <span>审计日志</span>
        </label>
      </div>

      <div class="rules">
        <div class="rule-row">
          <div class="rule-label">阻断端口</div>
          <div class="rule-editor">
            <div class="rule-tags">
              <span v-for="(port, index) in store.policy.blockedPorts" :key="`port-${port}-${index}`" class="rule-tag">
                {{ port }}
                <button :aria-label="`删除端口 ${port}`" title="删除" @click="removePort(index)">×</button>
              </span>
            </div>
            <div class="rule-input">
              <input v-model="portDraft" type="number" min="1" max="65535" placeholder="443" @keyup.enter="addPort">
              <button :disabled="!portDraft" @click="addPort">添加</button>
            </div>
          </div>
        </div>
        <div v-for="group in ruleGroups" :key="group.key" class="rule-row">
          <div class="rule-label">{{ group.label }}</div>
          <div class="rule-editor">
            <div class="rule-tags">
              <span v-for="(item, index) in store.policy[group.key]" :key="`${group.key}-${item}-${index}`" class="rule-tag">
                {{ item }}
                <button :aria-label="`删除 ${item}`" title="删除" @click="removeRule(group.key, index)">×</button>
              </span>
            </div>
            <div class="rule-input">
              <input v-model="drafts[group.key]" :placeholder="group.placeholder" @keyup.enter="addRule(group.key)">
              <button :disabled="!drafts[group.key].trim()" @click="addRule(group.key)">添加</button>
            </div>
          </div>
        </div>
      </div>
    </section>

    <section class="quarantine-section">
      <div class="section-heading quarantine-heading">
        <h2>隔离目录</h2>
        <div class="catalog-actions">
          <input v-model="quarantineSearch" type="search" placeholder="搜索路径或进程">
          <button class="refresh-button" @click="loadQuarantineCatalog">刷新</button>
        </div>
      </div>
      <div class="catalog-table-wrapper">
        <table class="catalog-table">
          <thead>
            <tr>
              <th>原始路径</th>
              <th>隔离路径</th>
              <th>进程</th>
              <th>大小</th>
              <th>状态</th>
              <th>最近更新</th>
              <th>操作</th>
            </tr>
          </thead>
          <tbody>
            <tr v-for="entry in filteredQuarantineEntries" :key="entry.id">
              <td class="path-cell" :title="entry.originalPath">{{ entry.originalPath }}</td>
              <td class="path-cell" :title="entry.quarantinePath">{{ entry.quarantinePath }}</td>
              <td>{{ entry.processName }} ({{ entry.pid }})</td>
              <td>{{ formatFileSize(entry.size) }}</td>
              <td><span class="catalog-status" :class="entry.status">{{ entry.status === 'present' ? '存在' : '缺失' }}</span></td>
              <td>{{ new Date(entry.lastSeenAt).toLocaleString('zh-CN') }}</td>
              <td class="row-actions">
                <button title="恢复到原始路径" :disabled="entry.status !== 'present' || quarantineBusyId !== null" @click="runQuarantineAction(entry, 'restore')">恢复</button>
                <button class="danger" title="永久删除隔离文件" :disabled="entry.status !== 'present' || quarantineBusyId !== null" @click="runQuarantineAction(entry, 'purge')">删除</button>
              </td>
            </tr>
            <tr v-if="filteredQuarantineEntries.length === 0">
              <td colspan="7" class="catalog-empty">暂无隔离记录</td>
            </tr>
          </tbody>
        </table>
      </div>
    </section>

    <div v-if="store.status.lastError" class="message error">驱动错误：{{ store.status.lastError }}</div>
    <div v-else-if="saveMessage" class="message" :class="{ error: saveMessage.includes('失败') }">{{ saveMessage }}</div>
  </div>
</template>

<style scoped>
.dlp-panel { display: flex; flex-direction: column; gap: 20px; max-width: 1120px; margin: 0 auto; color: #e6edf5; }
.status-section, .policy-section, .quarantine-section { border: 1px solid #29354a; background: #151d2b; }
.section-heading { min-height: 52px; display: flex; align-items: center; justify-content: space-between; padding: 0 18px; border-bottom: 1px solid #29354a; }
.section-heading h2 { margin: 0; font-size: 15px; font-weight: 650; letter-spacing: 0; }
button, input, select { font: inherit; }
.driver-button, .save-button { height: 32px; padding: 0 14px; border: 1px solid #397a91; background: #16475a; color: #dff8ff; cursor: pointer; }
.driver-button.danger { border-color: #81434b; background: #4f252c; color: #ffdfe2; }
button:disabled { cursor: not-allowed; opacity: .48; }
.row-actions { display: flex; gap: 6px; white-space: nowrap; }
.row-actions button { height: 28px; padding: 0 9px; border: 1px solid #397a91; background: #16475a; color: #dff8ff; cursor: pointer; }
.row-actions button.danger { border-color: #81434b; background: #4f252c; color: #ffdfe2; }
.status-grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 1px; background: #29354a; }
.status-card { min-height: 82px; display: flex; align-items: center; gap: 12px; padding: 0 18px; background: #151d2b; }
.status-dot { width: 10px; height: 10px; flex: 0 0 auto; border-radius: 50%; background: #697386; box-shadow: 0 0 0 4px rgba(105,115,134,.12); }
.status-dot.active { background: #40c786; box-shadow: 0 0 0 4px rgba(64,199,134,.13); }
.status-label { margin-bottom: 4px; color: #8f9caf; font-size: 12px; }
.status-value { color: #eef4fb; font-size: 15px; font-weight: 600; }
.switch-grid { display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); border-bottom: 1px solid #29354a; }
.switch-row { min-height: 58px; display: grid; grid-template-columns: auto 1fr; align-items: center; gap: 9px; padding: 0 16px; border-right: 1px solid #29354a; font-size: 13px; }
.switch-row:last-child { border-right: 0; }
.switch-row.compact { grid-template-columns: auto 1fr; }
.switch-row input { accent-color: #37a9c8; }
.switch-row select { height: 30px; border: 1px solid #34445d; background: #101725; color: #dbe5ef; padding: 0 8px; }
.rules { display: flex; flex-direction: column; }
.rule-row { display: grid; grid-template-columns: 170px minmax(0, 1fr); gap: 16px; padding: 14px 18px; border-bottom: 1px solid #222e40; }
.rule-row:last-child { border-bottom: 0; }
.rule-label { padding-top: 7px; color: #a8b5c7; font-size: 13px; }
.rule-editor { min-width: 0; display: flex; flex-direction: column; gap: 8px; }
.rule-tags { min-height: 25px; display: flex; flex-wrap: wrap; gap: 6px; }
.rule-tag { max-width: 100%; display: inline-flex; align-items: center; gap: 6px; padding: 4px 7px 4px 9px; border: 1px solid #34566b; background: #142d3a; color: #bdeafa; font-size: 12px; overflow-wrap: anywhere; }
.rule-tag button { width: 18px; height: 18px; padding: 0; border: 0; background: transparent; color: #91a7b3; cursor: pointer; font-size: 16px; line-height: 16px; }
.rule-tag button:hover { color: #ff8e97; }
.rule-input { display: grid; grid-template-columns: minmax(0, 1fr) 62px; max-width: 520px; }
.rule-input input { min-width: 0; height: 31px; box-sizing: border-box; border: 1px solid #34445d; border-right: 0; background: #101725; color: #e6edf5; padding: 0 10px; outline: none; }
.rule-input input:focus { border-color: #3c9bb5; }
.rule-input button { border: 1px solid #397a91; background: #16475a; color: #dff8ff; cursor: pointer; }
.message { padding: 11px 14px; border: 1px solid #315b4b; background: #163126; color: #9ce0bd; font-size: 13px; }
.message.error { border-color: #713d45; background: #3b2026; color: #ffb5bc; }
.catalog-actions { display: flex; align-items: center; gap: 8px; }
.catalog-actions input { width: 220px; height: 31px; border: 1px solid #34445d; background: #101725; color: #e6edf5; padding: 0 9px; outline: none; }
.catalog-actions input:focus { border-color: #3c9bb5; }
.refresh-button { height: 31px; padding: 0 12px; border: 1px solid #397a91; background: #16475a; color: #dff8ff; cursor: pointer; }
.catalog-table-wrapper { overflow-x: auto; }
.catalog-table { width: 100%; border-collapse: collapse; table-layout: fixed; font-size: 12px; }
.catalog-table th, .catalog-table td { padding: 9px 11px; border-bottom: 1px solid #222e40; text-align: left; }
.catalog-table th { color: #8f9caf; background: #111927; font-weight: 500; }
.catalog-table td { color: #d4deea; }
.catalog-table th:nth-child(1), .catalog-table th:nth-child(2) { width: 27%; }
.catalog-table th:nth-child(3) { width: 15%; }
.path-cell { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; font-family: Consolas, monospace; color: #a9c9d8 !important; }
.catalog-status { display: inline-block; min-width: 38px; padding: 2px 6px; border: 1px solid #425168; color: #aeb9c8; text-align: center; }
.catalog-status.present { border-color: #317158; color: #79d6a7; }
.catalog-status.missing { border-color: #78434a; color: #ef9da5; }
.catalog-empty { padding: 28px !important; text-align: center !important; color: #718096 !important; }
@media (max-width: 760px) {
  .status-grid, .switch-grid { grid-template-columns: 1fr; }
  .status-card, .switch-row { border-right: 0; border-bottom: 1px solid #29354a; }
  .status-card:last-child, .switch-row:last-child { border-bottom: 0; }
  .rule-row { grid-template-columns: 1fr; gap: 6px; }
  .rule-label { padding-top: 0; }
  .quarantine-heading { align-items: flex-start; flex-direction: column; padding-top: 10px; padding-bottom: 10px; gap: 8px; }
  .catalog-actions, .catalog-actions input { width: 100%; }
}
</style>
