import { defineStore } from 'pinia'
import { ref } from 'vue'

export const useSettingsStore = defineStore('settings', () => {
  const theme = ref<'dark' | 'light'>('dark')
  const refreshInterval = ref(2000)
  const autoStart = ref(false)
  const minimizeToTray = ref(true)
  const showNotifications = ref(true)
  const logRetentionDays = ref(30)

  return {
    theme, refreshInterval, autoStart,
    minimizeToTray, showNotifications, logRetentionDays
  }
})
