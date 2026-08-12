<script setup>
import { computed, onMounted, ref } from 'vue'
import { $tp } from '../../../platform-i18n'
import PlatformLayout from '../../../PlatformLayout.vue'

const props = defineProps([
  'platform',
  'config'
])

const config = ref(props.config)
const secondMonitorStatus = ref(null)
const statusError = ref('')
const displayDevices = computed(() => secondMonitorStatus.value?.devices?.filter(device => device.virtual_device === true) || [])

function deviceTypeLabel(value) {
  if (value === true) return 'virtual'
  if (value === false) return 'physical'
  return 'unknown'
}

function updateSecondMonitorPolicy() {
  config.value.second_monitor_display_policy = config.value.second_monitor_device_id ? 'vdd_only' : 'disabled'
}

async function refreshSecondMonitorStatus() {
  try {
    const response = await fetch('/api/second-monitor/status', { credentials: 'same-origin' })
    if (!response.ok) {
      throw new Error(`HTTP ${response.status}`)
    }
    secondMonitorStatus.value = await response.json()
    if (!config.value.second_monitor_device_id && secondMonitorStatus.value.selected_device_id) {
      config.value.second_monitor_device_id = secondMonitorStatus.value.selected_device_id
    }
  } catch {
    statusError.value = 'Second-monitor status is unavailable.'
  }
}

onMounted(refreshSecondMonitorStatus)

let _outputNamePlaceholder = '0';
if(props.platform === 'windows') {
  _outputNamePlaceholder = '{de9bb7e2-186e-505b-9e93-f48793333810}';
} else if(props.platform === 'linux' || props.platform === 'freebsd') {
  _outputNamePlaceholder = 'DP-0';
}
const outputNamePlaceholder = _outputNamePlaceholder;  // NOSONAR(javascript:S1481,javascript:S1854): Constant used by vue.js binding for placeholder below
</script>

<template>
  <div class="mb-3">
    <div v-if="displayDevices.length" class="mb-3">
      <label for="second_monitor_device_id" class="form-label">Second-monitor VDD</label>
      <select id="second_monitor_device_id" class="form-select"
              v-model="config.second_monitor_device_id"
              @change="updateSecondMonitorPolicy">
        <option value="">Use regular Sunshine display selection</option>
        <option v-for="device in displayDevices"
                :key="device.device_id" :value="device.device_id">
          {{ device.friendly_name || device.display_name || 'Display device' }}
          ({{ device.device_id }})
          - {{ deviceTypeLabel(device.virtual_device) }}
        </option>
      </select>
    </div>

    <label for="output_name" class="form-label">{{ $t('config.output_name') }}</label>
    <select v-if="secondMonitorStatus?.selected_device_id"
            class="form-select" id="output_name" v-model="config.output_name">
      <option :value="secondMonitorStatus.selected_device_id">
        {{ secondMonitorStatus.friendly_name || secondMonitorStatus.mapped_output || 'Virtual display' }}
        ({{ secondMonitorStatus.selected_device_id }})
      </option>
    </select>
    <input v-else type="text" class="form-control" id="output_name" :placeholder="outputNamePlaceholder"
           v-model="config.output_name"/>
    <div v-if="secondMonitorStatus?.selected_device_id" class="mt-2 small">
      <div>Second-monitor device: {{ secondMonitorStatus.active ? 'active' : 'waiting for VDD' }}</div>
      <div>Stable ID: {{ secondMonitorStatus.selected_device_id || 'not selected' }}</div>
      <div>Current resolution: {{ secondMonitorStatus.current_resolution || 'unknown' }}</div>
      <div>Device type: {{ deviceTypeLabel(secondMonitorStatus.virtual_device) }}</div>
    </div>
    <div v-if="statusError" class="form-text">{{ statusError }}</div>
    <div class="form-text">
      {{ $tp('config.output_name_desc') }}<br>
      <PlatformLayout :platform="platform">
        <template #windows>
          <pre style="white-space: pre-line;">
            <b>&nbsp;&nbsp;{</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"device_id": "{de9bb7e2-186e-505b-9e93-f48793333810}"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"display_name": "\\\\.\\DISPLAY1"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;"friendly_name": "ROG PG279Q"</b>
            <b>&nbsp;&nbsp;&nbsp;&nbsp;...</b>
            <b>&nbsp;&nbsp;}</b>
          </pre>
        </template>
        <template #freebsd>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: HDMI-A-1 connected: true
            Info: Detected display: DP-1 connected: true
            Info: Detected display: DP-2 connected: false
            Info: Detected display: DVI-D-3 connected: false
          </pre>
        </template>
        <template #linux>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: HDMI-A-1 connected: true
            Info: Detected display: DP-1 connected: true
            Info: Detected display: DP-2 connected: false
            Info: Detected display: DVI-D-3 connected: false
          </pre>
        </template>
        <template #macos>
          <pre style="white-space: pre-line;">
            Info: Detecting displays
            Info: Detected display: Monitor-0 (id: 3) connected: true
            Info: Detected display: Monitor-1 (id: 2) connected: true
          </pre>
        </template>
      </PlatformLayout>
    </div>
  </div>
</template>
