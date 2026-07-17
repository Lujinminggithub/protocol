import { Tray, Menu, nativeImage, app, BrowserWindow, dialog, shell } from 'electron'
import { join } from 'path'

let tray: Tray | null = null
let mainWindow: BrowserWindow | null = null

export function setMainWindow(win: BrowserWindow): void {
  mainWindow = win
}

export function createTray(window: BrowserWindow): void {
  // 安全加载托盘图标：nativeImage.createFromPath 对不存在的路径返回空 image
  // 而不会抛错；空则退化为占位图标，避免 new Tray(坏路径) 在 Windows 下崩溃。
  let trayIcon = nativeImage.createFromPath(join(__dirname, '..', 'public', 'icon.png'))
  if (trayIcon.isEmpty()) {
    trayIcon = nativeImage.createEmpty()
  }

  // 创建托盘图标
  tray = new Tray(trayIcon)
  tray.setToolTip('PersonalSafer - 个人PC安全管理')

  // 托盘菜单
  const contextMenu = Menu.buildFromTemplate([
    {
      label: '打开主窗口',
      click: () => {
        if (window) {
          window.show()
          window.focus()
        }
      }
    },
    { type: 'separator' },
    {
      label: '系统监控',
      click: () => {
        if (window) {
          window.show()
          window.focus()
          // 可以通过 IPC 通知渲染进程切换到监控页
        }
      }
    },
    {
      label: 'DLP 状态',
      click: () => {
        if (window) {
          window.show()
          window.focus()
        }
      }
    },
    {
      label: '审计日志',
      click: () => {
        if (window) {
          window.show()
          window.focus()
        }
      }
    },
    { type: 'separator' },
    {
      label: '关于 PersonalSafer',
      click: () => {
        dialog.showMessageBox({
          type: 'info',
          title: '关于',
          message: 'PersonalSafer',
          detail: '个人PC安全管理客户端 v1.0.0\n\n© 2026 PersonalSafer Team'
        })
      }
    },
    {
      label: '退出',
      click: () => {
        // 彻底退出应用：先标记退出，避免 close 被 preventDefault 拦截导致进程残留
        ;(window as any)._isQuitting = true
        tray?.destroy()
        tray = null
        app.quit()
      }
    }
  ])

  tray.setContextMenu(contextMenu)

  // 双击托盘图标显示窗口
  tray.on('double-click', () => {
    if (window) {
      window.show()
      window.focus()
    }
  })

  // 右键点击托盘图标显示菜单
  tray.on('right-click', () => {
    tray?.popUpContextMenu(contextMenu)
  })

  // 监听窗口关闭事件，最小化到托盘
  window.on('close', (event) => {
    // 检查是否通过"退出"菜单关闭
    if (!(window as any)._isQuitting) {
      event.preventDefault()
      window.hide()
    }
  })
}

// 标记窗口即将退出（通过托盘菜单的"退出"选项）
export function quitApp(): void {
  if (mainWindow) {
    mainWindow._isQuitting = true
    mainWindow.close()
  } else {
    app.quit()
  }
}

// 清理托盘
export function destroyTray(): void {
  if (tray) {
    tray.destroy()
    tray = null
  }
}
