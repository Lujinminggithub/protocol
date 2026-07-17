<script setup lang="ts">
interface TimelineEvent {
  time: string
  title: string
  description: string
  type: 'info' | 'warning' | 'error' | 'success'
}

defineProps<{
  events: TimelineEvent[]
}>()
</script>

<template>
  <div class="event-timeline">
    <div
      v-for="(event, idx) in events"
      :key="idx"
      class="timeline-item"
    >
      <div class="timeline-dot" :class="event.type"></div>
      <div class="timeline-content">
        <div class="timeline-header">
          <span class="timeline-title">{{ event.title }}</span>
          <span class="timeline-time">{{ event.time }}</span>
        </div>
        <div class="timeline-desc">{{ event.description }}</div>
      </div>
    </div>
  </div>
</template>

<style scoped>
.event-timeline {
  padding: 8px 0;
}

.timeline-item {
  display: flex;
  gap: 12px;
  padding: 8px 0;
}

.timeline-dot {
  width: 10px;
  height: 10px;
  border-radius: 50%;
  flex-shrink: 0;
  margin-top: 4px;
}

.timeline-dot.info { background: #00d4ff; }
.timeline-dot.warning { background: #f59e0b; }
.timeline-dot.error { background: #ef4444; }
.timeline-dot.success { background: #4ade80; }

.timeline-header {
  display: flex;
  justify-content: space-between;
  align-items: center;
}

.timeline-title {
  font-size: 13px;
  font-weight: 500;
  color: #ccc;
}

.timeline-time {
  font-size: 11px;
  color: #666;
}

.timeline-desc {
  font-size: 12px;
  color: #888;
  margin-top: 2px;
}
</style>
