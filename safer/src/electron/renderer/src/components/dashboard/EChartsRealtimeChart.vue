/*
 * EChartsRealtimeChart.vue - ECharts 实时图表组件
 * 支持 CPU/内存/网络/磁盘的历史趋势折线图
 */
<script setup lang="ts">
import { ref, onMounted, onUnmounted, watch, nextTick } from 'vue'
import * as echarts from 'echarts/core'
import { BarChart, LineChart } from 'echarts/charts'
import { CanvasRenderer } from 'echarts/renderers'
import { GridComponent, TooltipComponent, LegendComponent } from 'echarts/components'

echarts.use([BarChart, LineChart, CanvasRenderer, GridComponent, TooltipComponent, LegendComponent])

const props = withDefaults(defineProps<{
  title: string
  seriesName: string[]
  data: number[][]
  maxDataPoints?: number
  unit?: string
  colorPalette?: string[]
  yAxisMin?: number
  yAxisMax?: number
}>(), {
  maxDataPoints: 60
})

const chartRef = ref<HTMLDivElement | null>(null)
let chart: echarts.ECharts | null = null
let refreshTimer: ReturnType<typeof setInterval> | null = null

const defaultPalette = ['#00d4ff', '#4ade80', '#f59e0b', '#ef4444', '#8b5cf6', '#ec4899']

function initChart() {
  if (!chartRef.value) return
  chart = echarts.init(chartRef.value, undefined, { renderer: 'canvas' })

  const series = props.seriesName.map((name, i) => ({
    name,
    type: 'line',
    data: props.data[i] || [],
    smooth: true,
    symbol: 'none',
    lineStyle: { width: 2 },
    areaStyle: {
      color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
        { offset: 0, color: props.colorPalette?.[i] || defaultPalette[i] + '66' },
        { offset: 1, color: (props.colorPalette?.[i] || defaultPalette[i]) + '08' }
      ])
    },
    emphasis: { focus: 'series' }
  }))

  chart.setOption({
    tooltip: {
      trigger: 'axis',
      backgroundColor: 'rgba(22, 33, 62, 0.95)',
      borderColor: '#1a3a5c',
      textStyle: { color: '#ccc', fontSize: 12 },
      formatter: (params: any) => {
        let tip = `<div style="font-size:12px;color:#888;margin-bottom:4px">${params[0].axisValue}</div>`
        for (const p of params) {
          tip += `<div style="display:flex;align-items:center;gap:4px">
            <span style="display:inline-block;width:8px;height:8px;border-radius:50%;background:${p.color}"></span>
            <span>${p.seriesName}: ${p.value}${props.unit || ''}</span>
          </div>`
        }
        return tip
      }
    },
    legend: {
      data: props.seriesName,
      top: 0,
      right: 10,
      textStyle: { color: '#888', fontSize: 11 },
      itemWidth: 15,
      itemHeight: 8
    },
    grid: { left: 50, right: 16, top: 30, bottom: 24 },
    xAxis: {
      type: 'category',
      data: props.data[0]?.map((_, i) => i.toString()) || [],
      boundaryGap: false,
      axisLine: { lineStyle: { color: '#2a2a4a' } },
      axisLabel: { color: '#666', fontSize: 10 },
      splitLine: { show: false }
    },
    yAxis: {
      type: 'value',
      min: props.yAxisMin ?? undefined,
      max: props.yAxisMax ?? undefined,
      axisLine: { show: false },
      axisLabel: {
        color: '#666',
        fontSize: 10,
        formatter: (v: number) => `${v}${props.unit || ''}`
      },
      splitLine: { lineStyle: { color: '#1a2744' } }
    },
    series,
    animation: true,
    animationDuration: 300,
    animationEasing: 'cubicOut'
  })
}

function updateChartData() {
  if (!chart || props.data.length === 0) return

  const labels = props.data[0]?.map((_, i) => i.toString()) || []
  const series = props.seriesName.map((name, i) => ({
    name,
    type: 'line',
    data: props.data[i] || [],
    smooth: true,
    symbol: 'none',
    lineStyle: { width: 2 },
    areaStyle: {
      color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
        { offset: 0, color: (props.colorPalette?.[i] || defaultPalette[i]) + '66' },
        { offset: 1, color: (props.colorPalette?.[i] || defaultPalette[i]) + '08' }
      ])
    }
  }))

  chart.setOption({
    xAxis: { data: labels.slice(-props.maxDataPoints || 60) },
    series
  })
}

function startAutoRefresh(intervalMs: number = 2000) {
  stopAutoRefresh()
  refreshTimer = setInterval(() => {
    nextTick(() => {
      if (chart) chart.resize()
    })
  }, intervalMs)
}

function stopAutoRefresh() {
  if (refreshTimer) {
    clearInterval(refreshTimer)
    refreshTimer = null
  }
}

onMounted(() => {
  initChart()
  startAutoRefresh()
  window.addEventListener('resize', () => {
    if (chart) chart.resize()
  })
})

onUnmounted(() => {
  stopAutoRefresh()
  if (chart) {
    chart.dispose()
    chart = null
  }
})

watch(() => props.data, () => {
  nextTick(() => {
    if (chart) updateChartData()
  })
}, { deep: true })
</script>

<template>
  <div class="realtime-chart">
    <div class="chart-title">{{ title }}</div>
    <div ref="chartRef" class="chart-container"></div>
  </div>
</template>

<style scoped>
.realtime-chart {
  background: #16213e;
  border: 1px solid #1a3a5c;
  border-radius: 12px;
  overflow: hidden;
}

.chart-title {
  padding: 12px 16px 4px;
  font-size: 14px;
  font-weight: 500;
  color: #ccc;
}

.chart-container {
  width: 100%;
  height: 200px;
}
</style>
