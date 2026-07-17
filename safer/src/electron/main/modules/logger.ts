/**
 * logger.ts - 文件日志
 *
 * 将主进程的 console.log/warn/error 同时写入日志文件。
 * 日志目录：
 *   - 打包应用：安装目录下的 log/（exe 所在目录，requireAdministrator 可写）
 *   - 开发模式：userData/log/（避免写入项目目录/系统目录）
 */
import { app } from 'electron'
import { join, dirname } from 'path'
import { mkdirSync, createWriteStream, openSync, WriteStream } from 'fs'

let stream: WriteStream | null = null
let activeLogDir = ''

/** 候选日志目录（按优先级）：安装目录/log → userData/log → temp/log */
function candidateDirs(): string[] {
  const dirs: string[] = []
  try {
    if (app.isPackaged) dirs.push(join(dirname(app.getPath('exe')), 'log'))
  } catch { /* ignore */ }
  try { dirs.push(join(app.getPath('userData'), 'log')) } catch { /* ignore */ }
  try { dirs.push(join(app.getPath('temp'), 'PersonalSafer', 'log')) } catch { /* ignore */ }
  return dirs
}

/** 实际生效的日志目录（初始化后才有值） */
export function getLogDir(): string {
  return activeLogDir
}

function pad(n: number): string {
  return n < 10 ? '0' + n : String(n)
}

function timestamp(): string {
  const d = new Date()
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
         `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`
}

function dateStamp(): string {
  const d = new Date()
  return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())}`
}

function writeLine(level: string, args: any[]): void {
  if (!stream) return
  const text = args
    .map((a) => (typeof a === 'string' ? a : (() => { try { return JSON.stringify(a) } catch { return String(a) } })()))
    .join(' ')
  try {
    stream.write(`[${timestamp()}] [${level}] ${text}\n`)
  } catch {
    /* 忽略写日志失败，不影响主功能 */
  }
}

/**
 * 初始化文件日志，并把 console.log/warn/error 接到日志文件（同时保留控制台输出）。
 * 依次尝试多个候选目录，选第一个能创建/可写的；全部失败则仅保留控制台输出。
 */
export function initLogger(): void {
  const origLog = console.log.bind(console)
  const origWarn = console.warn.bind(console)
  const origError = console.error.bind(console)

  let lastErr: any = null
  for (const dir of candidateDirs()) {
    try {
      mkdirSync(dir, { recursive: true })
      const file = join(dir, `personalsafer-${dateStamp()}.log`)
      // 关键：用 openSync 同步探测可写性——EPERM/EACCES 会在此被 try/catch 捕获，
      // 从而回退到下一个候选目录。若直接用 createWriteStream，它是异步打开，
      // 失败只会触发 'error' 事件，try/catch 抓不到，会变成未捕获异常导致崩溃。
      const fd = openSync(file, 'a')
      stream = createWriteStream(file, { fd })
      // 兜底：即便打开成功，运行期写入错误也不得让应用崩溃
      stream.on('error', (err: any) => {
        origWarn('[Logger] 写日志出错（已忽略）:', err?.message || err)
      })
      activeLogDir = dir
      break
    } catch (e) {
      lastErr = e
      stream = null
    }
  }

  console.log = (...a: any[]) => { writeLine('INFO', a); origLog(...a) }
  console.warn = (...a: any[]) => { writeLine('WARN', a); origWarn(...a) }
  console.error = (...a: any[]) => { writeLine('ERROR', a); origError(...a) }

  if (stream) {
    console.log(`[Logger] 日志目录: ${activeLogDir}`)
  } else {
    // 所有候选目录都写不了：只保留控制台，并把原因打出来（便于排查）
    origWarn('[Logger] 无法创建日志文件（所有候选目录均不可写）:', lastErr && (lastErr.message || lastErr))
  }
}

