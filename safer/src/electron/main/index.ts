import { app, BrowserWindow } from 'electron'
import { execFileSync } from 'node:child_process'
import { existsSync } from 'node:fs'
import { join } from 'node:path'
import { registerHandlers } from './ipc/handler_registry'
import { flushEventPersistence, initializeEventPersistence, startEventPolling, stopEventPolling } from './modules/dlp_events'
import { startDeliveryWorker, stopDeliveryWorker } from './modules/delivery_worker'
import { parseDeliverySelfCheckArgs, runInstalledDeliverySelfCheck } from './modules/delivery_self_check'
import { initLogger } from './modules/logger'
import { startLocalProxy, stopLocalProxy, updateLocalProxyPolicy } from './modules/local_proxy'
import { applyPolicyToKernel, getPersistedPolicy, initializePolicyStore } from './modules/policy_store'
import { flushQuarantineCatalog, initializeQuarantineCatalog } from './modules/quarantine_catalog'
import { getAddon, isAddonLoaded } from './modules/native_loader'
import { parseSelfCheckArgs, runInstalledProxySelfCheck } from './modules/proxy_self_check'
import { parseProtectionMaintenanceArgs, runProtectionMaintenance } from './modules/protection_maintenance'
import { createTray, setMainWindow } from './tray'
import { createMainWindow } from './window'

const selfCheckArgs = parseSelfCheckArgs()
const deliverySelfCheckArgs = parseDeliverySelfCheckArgs()
const protectionMaintenanceArgs = parseProtectionMaintenanceArgs()

function installDriverCert(): void {
  try {
    const certPath = join(process.resourcesPath, 'driver', 'PersonalSafer-Test.cer')
    if (!existsSync(certPath)) {
      return
    }

    for (const store of ['Root', 'TrustedPublisher']) {
      try {
        execFileSync('certutil', ['-addstore', '-f', store, certPath], { stdio: 'ignore' })
      } catch {
        // Ignore duplicate or non-fatal import failures.
      }
    }

    console.log('[AutoLoad] driver test certificate installed')
  } catch (error: any) {
    console.warn('[AutoLoad] install driver certificate failed:', error?.message || error)
  }
}

async function autoLoadDriver(): Promise<void> {
  if (!app.isPackaged) {
    return
  }

  try {
    const addon = getAddon()
    if (!addon || !isAddonLoaded()) {
      return
    }

    installDriverCert()
    const sysPath = join(process.resourcesPath, 'driver', 'PersonalSafer.sys')
    const result = addon.dlp.driver_loader.load(sysPath)
    if (result?.success) {
      addon.dlp.kernel_comm?.connect?.()
      const policyResult = applyPolicyToKernel(addon, getPersistedPolicy())
      if (!policyResult.success) {
        console.error('[AutoLoad] persisted policy restore failed:', policyResult.error)
        addon.dlp.kernel_comm?.disconnect?.()
        addon.dlp.driver_loader?.unload?.()
        return
      }
      updateLocalProxyPolicy(policyResult.policy)
      startEventPolling()
      try {
        await startLocalProxy({ enabled: true, installSystemProxy: false, mitmEnabled: true })
      } catch (error: any) {
        console.warn('[AutoLoad] local proxy start failed:', error?.message || error)
      }
      console.log(`[AutoLoad] driver loaded${result.alreadyRunning ? ' (already running)' : ''}`)
    } else {
      console.warn('[AutoLoad] driver load failed:', result?.error)
    }
  } catch (error: any) {
    console.warn('[AutoLoad] driver autoload failed:', error?.message || error)
  }
}

const gotTheLock = (selfCheckArgs.enabled || deliverySelfCheckArgs.enabled || protectionMaintenanceArgs.enabled)
  ? true
  : app.requestSingleInstanceLock()
if (!gotTheLock) {
  app.quit()
}

process.on('uncaughtException', (error: any) => {
  try {
    console.error('[FATAL] uncaught exception:', error?.stack || error?.message || error)
  } catch {
    // Ignore secondary logging failures.
  }
})

process.on('unhandledRejection', (reason: any) => {
  try {
    console.error('[FATAL] unhandled rejection:', reason)
  } catch {
    // Ignore secondary logging failures.
  }
})

app.on('before-quit', () => {
  BrowserWindow.getAllWindows().forEach((window) => {
    ;(window as any)._isQuitting = true
  })
  try {
    stopEventPolling()
  } catch {
    // Ignore shutdown cleanup failures.
  }
  try {
    flushEventPersistence()
  } catch {
    // Ignore persistence flush failures on shutdown.
  }
  try {
    flushQuarantineCatalog()
  } catch {
    // Ignore catalog flush failures during shutdown.
  }
  void stopDeliveryWorker()
  void stopLocalProxy()
})

if (gotTheLock) {
  app.on('second-instance', () => {
    const windows = BrowserWindow.getAllWindows()
    if (windows.length === 0) {
      return
    }
    const mainWindow = windows[0]
    if (!mainWindow.isVisible()) {
      mainWindow.show()
    }
    if (mainWindow.isMinimized()) {
      mainWindow.restore()
    }
    mainWindow.moveTop()
    mainWindow.focus()
  })

  app.whenReady().then(() => {
    initLogger()
    initializePolicyStore()
    initializeQuarantineCatalog()
    initializeEventPersistence()

    if (selfCheckArgs.enabled) {
      void runInstalledProxySelfCheck(selfCheckArgs.outDir).then((exitCode) => {
        console.log(`[SelfCheck] output directory: ${selfCheckArgs.outDir}`)
        app.exit(exitCode)
      })
      return
    }

    if (deliverySelfCheckArgs.enabled) {
      void runInstalledDeliverySelfCheck(deliverySelfCheckArgs.outDir).then((exitCode) => {
        console.log(`[DeliverySelfCheck] output directory: ${deliverySelfCheckArgs.outDir}`)
        app.exit(exitCode)
      })
      return
    }

    if (protectionMaintenanceArgs.enabled) {
      installDriverCert()
      void runProtectionMaintenance(protectionMaintenanceArgs).then((exitCode) => {
        console.log(`[ProtectionMaintenance] output directory: ${protectionMaintenanceArgs.outDir}`)
        app.exit(exitCode)
      })
      return
    }

    startDeliveryWorker()

    const mainWindow = createMainWindow()
    registerHandlers()

    try {
      createTray(mainWindow)
      setMainWindow(mainWindow)
    } catch (error) {
      console.error('[Tray] create tray failed:', error)
    }

    void autoLoadDriver()

    app.on('activate', () => {
      if (BrowserWindow.getAllWindows().length === 0) {
        createMainWindow()
      }
    })
  })
}

app.on('window-all-closed', () => {
  if (process.platform !== 'darwin') {
    app.quit()
  }
})
