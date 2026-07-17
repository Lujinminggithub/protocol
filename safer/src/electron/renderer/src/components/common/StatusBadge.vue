<script setup lang="ts">
defineProps<{
  status: 'success' | 'warning' | 'error' | 'info'
  text: string
}>()
</script>

<template>
  <span class="status-badge" :class="status">
    <span class="badge-dot"></span>
    <slot>{{ text }}</slot>
  </span>
</template>

<style scoped>
.status-badge {
  display: inline-flex;
  align-items: center;
  gap: 4px;
  padding: 3px 10px;
  border-radius: 12px;
  font-size: 12px;
  font-weight: 500;
}

.badge-dot {
  width: 6px;
  height: 6px;
  border-radius: 50%;
}

.status-badge.success {
  background: rgba(74, 222, 128, 0.15);
  color: #4ade80;
}

.status-badge.success .badge-dot {
  background: #4ade80;
}

.status-badge.warning {
  background: rgba(245, 158, 11, 0.15);
  color: #f59e0b;
}

.status-badge.warning .badge-dot {
  background: #f59e0b;
}

.status-badge.error {
  background: rgba(239, 68, 68, 0.15);
  color: #ef4444;
}

.status-badge.error .badge-dot {
  background: #ef4444;
}

.status-badge.info {
  background: rgba(0, 212, 255, 0.15);
  color: #00d4ff;
}

.status-badge.info .badge-dot {
  background: #00d4ff;
}
</style>
