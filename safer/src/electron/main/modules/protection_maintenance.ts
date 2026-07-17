import { app } from 'electron'
import { mkdirSync, writeFileSync } from 'node:fs'
import { join, resolve } from 'node:path'
import { getAddon, isAddonLoaded } from './native_loader'

export interface ProtectionMaintenanceArgs {
  enabled: boolean
  action: 'status' | 'unlock' | 'relock'
  outDir: string
}

function values(name: string): string[] {
  const prefix = `--${name}=`
  return process.argv.filter((arg) => arg.startsWith(prefix)).map((arg) => arg.slice(prefix.length))
}

function value(name: string, fallback = ''): string {
  return values(name)[0] || fallback
}

export function parseProtectionMaintenanceArgs(): ProtectionMaintenanceArgs {
  const rawAction = value('ps-maintenance-action')
  const allowed = ['status', 'unlock', 'relock']
  const action = (allowed.includes(rawAction) ? rawAction : 'status') as ProtectionMaintenanceArgs['action']
  const outDir = value(
    'ps-maintenance-out',
    join(app.getPath('temp'), `PersonalSafer-maintenance-${Date.now()}`),
  )

  return {
    enabled: process.argv.some((arg) => arg.startsWith('--ps-maintenance-action=')),
    action,
    outDir: resolve(outDir),
  }
}

function writeResult(outDir: string, name: string, value: unknown): void {
  mkdirSync(outDir, { recursive: true })
  writeFileSync(join(outDir, name), JSON.stringify(value, null, 2), 'utf8')
}

export async function runProtectionMaintenance(args: ProtectionMaintenanceArgs): Promise<number> {
  const summary: Record<string, unknown> = {
    action: args.action,
    timestamp: new Date().toISOString(),
  }

  try {
    const addon = getAddon()
    if (!addon || !isAddonLoaded()) throw new Error('native addon not loaded')
    if (!addon.dlp.driver_loader.isLoaded()) {
      const sysPath = app.isPackaged
        ? join(process.resourcesPath, 'driver', 'PersonalSafer.sys')
        : undefined
      const loadResult = addon.dlp.driver_loader.load(sysPath)
      summary.loadResult = loadResult
      if (!loadResult?.success) throw new Error(loadResult?.error || 'driver load failed')
    }
    if (!addon.dlp.kernel_comm.connect()) throw new Error('kernel device connection failed')

    const kernel = addon.dlp.kernel_comm
    try {
      if (args.action !== 'status' &&
          !kernel.setProtectionMaintenanceMode(args.action === 'unlock')) {
        throw new Error(`${args.action} protection control failed`)
      }
      summary.success = true
      summary.state = kernel.getProtectionState()
    } finally {
      kernel.disconnect()
    }
  } catch (error: any) {
    summary.success = false
    summary.error = error?.stack || error?.message || String(error)
  }

  writeResult(args.outDir, 'protection-maintenance-summary.json', summary)
  return summary.success ? 0 : 1
}
