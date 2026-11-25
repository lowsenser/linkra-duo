import { signal, computed, batch, effect } from '@preact/signals'
import { normalizeEvent, deviceKey, dbTimeToEpoch, measurements, pruneDevices } from './format'
import { api } from './http'
import { createScheduler, backoffDelay } from './scheduler'
import { route } from './router'
import { expert } from './prefs'

export { api }

// ---------- Toasts ----------
export const toasts = signal([])
let toastSeq = 0
export function toast(msg, kind = 'info') {
  const id = ++toastSeq
  toasts.value = [...toasts.value.slice(-3), { id, msg, kind }]
  setTimeout(() => { toasts.value = toasts.value.filter(t => t.id !== id) }, kind === 'bad' ? 6000 : 3500)
}
export async function act(promise, okMsg) {
  try { const r = await promise; if (okMsg) toast(okMsg, 'ok'); return r }
  catch (e) { toast(e.message || String(e), 'bad'); throw e }
}

// ---------- State ----------
export const conn = signal('offline')          // SSE: live | connecting | offline | paused
export const online = signal(true)             // gateway answers polls
export const offlineSince = signal(0)
export const lastPoll = signal(0)
export const sys = signal(null)                // /api/system
export const wifi = signal(null)               // /api/wifi/status
export const rf = signal(null)                 // /api/rf/overview
export const mqtt = signal(null)               // /api/mqtt
export const update = signal(null)             // /api/update (GitHub release check)
export const events = signal([])               // newest first
export const devices = signal(new Map())
export const paused = signal(false)
export const counters = signal({ total: 0, decoded: 0, classified: 0 })
export const notice = signal(null)             // short status message (e.g. after a gateway reboot)

const HIST = 900                               // 15 min @1s
export const rssiHist = { t: [], r433: [], n433: [], th433: [], r868: [], n868: [], th868: [] }
export const histTick = signal(0)
export const timeline = signal({ '433': [], '868': [] })  // profile transitions

export const deviceList = computed(() => Array.from(devices.value.values()))

const MAX_EVENTS = 600
const MAX_PENDING = MAX_EVENTS                 // hard cap while the tab sleeps (review 07 H2)
let evSeq = 0
let pendingEvents = []
let flushQueued = false

// rAF does not run in background tabs: a timer is the fallback, whichever comes first wins
function scheduleFlush() {
  if (flushQueued) return
  flushQueued = true
  const t = setTimeout(flushEvents, 250)
  if (typeof document !== 'undefined' && !document.hidden) requestAnimationFrame(() => { clearTimeout(t); flushEvents() })
}

function flushEvents() {
  flushQueued = false
  if (!pendingEvents.length) return
  const batchEv = pendingEvents; pendingEvents = []
  batch(() => {
    if (!paused.value) events.value = [...batchEv.reverse(), ...events.value].slice(0, MAX_EVENTS)
    let map = new Map(devices.value)
    const c = { ...counters.value }
    for (const ev of batchEv) {
      c.total++; if (ev.decoded) c.decoded++; else c.classified++
      upsertDevice(map, ev)
    }
    if (map.size > 500) map = pruneDevices(map)
    devices.value = map
    counters.value = c
  })
}

function upsertDevice(map, ev) {
  const key = deviceKey(ev)
  const d = map.get(key)
  const meas = ev.meas
  const histPoint = { t: ev.t, rssi: ev.rssi, v: Object.fromEntries(meas.filter(m => typeof ev.raw[m.k] === 'number').map(m => [m.k, ev.raw[m.k]])) }
  if (d) {
    const history = [...(d.history || []), histPoint].slice(-120)
    const intervals = d.lastSeen ? [...(d.intervals || []), ev.t - d.lastSeen].slice(-10) : d.intervals
    map.set(key, {
      ...d, count: d.count + 1, sessionCount: (d.sessionCount || 0) + 1, lastSeen: ev.t,
      rssi: ev.rssi ?? d.rssi,
      rssiMin: ev.rssi != null ? Math.min(d.rssiMin ?? ev.rssi, ev.rssi) : d.rssiMin,
      rssiMax: ev.rssi != null ? Math.max(d.rssiMax ?? ev.rssi, ev.rssi) : d.rssiMax,
      band: ev.band || d.band, freq: ev.freq || d.freq, profile: ev.profile || d.profile,
      profiles: { ...(d.profiles || {}), [ev.profile || '?']: ((d.profiles || {})[ev.profile || '?'] || 0) + 1 },
      mod: ev.mod || d.mod, channel: ev.channel ?? d.channel,
      last: ev.decoded ? ev.raw : d.last, meas: ev.decoded ? meas : d.meas,
      decoded: d.decoded || ev.decoded, history, intervals,
      persisted: d.persisted || !!ev.raw.persisted,
    })
  } else {
    map.set(key, {
      key, model: ev.model, devId: ev.devId, channel: ev.channel, band: ev.band, freq: ev.freq,
      profile: ev.profile, profiles: { [ev.profile || '?']: 1 }, mod: ev.mod,
      decoded: ev.decoded, classified: ev.classified,
      count: 1, sessionCount: 1, firstSeen: ev.t, lastSeen: ev.t,
      rssi: ev.rssi, rssiMin: ev.rssi, rssiMax: ev.rssi,
      last: ev.decoded ? ev.raw : null, meas, history: [histPoint], intervals: [],
      persisted: !!ev.raw.persisted, mqttEnabled: false,
    })
  }
}

export function clearSession() {
  batch(() => {
    events.value = []
    counters.value = { total: 0, decoded: 0, classified: 0 }
    const map = new Map()
    for (const [k, d] of devices.value) if (d.persisted) map.set(k, { ...d, sessionCount: 0, history: [] })
    devices.value = map
  })
}

// ---------- Live level frames (5 Hz via SSE "rf_rssi") ----------
// Each frame: peak/mean RSSI of both receivers over 200 ms, noise floor,
// threshold, receive flags. Listeners get every frame synchronously (the
// waterfall must not skip columns); signals update once per animation frame.
const LIVE_MAX = 1800                                   // 6 min
export const live = { t: [], p433: [], m433: [], p868: [], m868: [], n433: [], n868: [], th433: [], th868: [], rx: [], wm: [], f433: [], f868: [] }
export const liveFrame = signal(null)
export const liveTick = signal(0)
const frameListeners = new Set(), signalListeners = new Set()
export const onLiveFrame = (cb) => { frameListeners.add(cb); return () => frameListeners.delete(cb) }
export const onSignal = (cb) => { signalListeners.add(cb); return () => signalListeners.delete(cb) }
let liveQueued = false, lastLive = null
function onRssiFrame(j) {
  if (j.p433 === undefined) return                     // old firmware frame format
  const mods = rf.value?.modules || []
  const freqOf = (b) => mods.find(m => m.band === b)?.freqMHz ?? null
  const f = {
    t: Date.now(), p433: j.p433 > -128 ? j.p433 : null, m433: j.m433 > -128 ? j.m433 : null,
    p868: j.p868 > -128 ? j.p868 : null, m868: j.m868 > -128 ? j.m868 : null,
    n433: j.avg433, n868: j.avg868, th433: j.th433, th868: j.th868, rx: j.rx, wm: j.wm,
    f433: freqOf('433'), f868: j.wm ? 868.95 : freqOf('868'),
  }
  for (const k in live) live[k].push(f[k])
  if (live.t.length > LIVE_MAX * 1.2) for (const k in live) live[k].splice(0, live[k].length - LIVE_MAX)
  lastLive = f
  for (const cb of frameListeners) { try { cb(f) } catch { } }
  if (!liveQueued) {
    liveQueued = true
    requestAnimationFrame(() => { liveQueued = false; batch(() => { liveFrame.value = lastLive; liveTick.value++ }) })
  }
}

// ---------- SSE ----------
// One EventSource per tab. Reconnect with exponential backoff (3 s → 30 s),
// closed after 45 s in a background tab and reopened when the tab returns.
const HIDDEN_CLOSE_MS = 45000
let es = null
let sseWanted = false
let sseFails = 0
let lastSseMs = 0
let reconnectTimer = null, hiddenTimer = null, watchdog = null

function closeES() { if (es) { es.close(); es = null } }

function scheduleReconnect(ms) {
  closeES()
  clearTimeout(reconnectTimer)
  if (!sseWanted) return
  conn.value = 'offline'
  reconnectTimer = setTimeout(() => {
    if (document.hidden) { conn.value = 'paused'; return }   // reopened on visibilitychange
    connectSSE()
  }, ms)
}

function connectSSE() {
  if (!sseWanted) return
  closeES()
  clearTimeout(reconnectTimer)
  conn.value = 'connecting'
  lastSseMs = Date.now()
  es = new EventSource('/api/events')
  es.onopen = () => { conn.value = 'live'; sseFails = 0; lastSseMs = Date.now() }
  es.onerror = () => { sseFails++; scheduleReconnect(backoffDelay(sseFails, 3000, 30000)) }
  // rf_rssi arrives at 5 Hz: any event proves the stream is alive
  es.addEventListener('rf_rssi', (e) => {
    lastSseMs = Date.now()
    try { onRssiFrame(JSON.parse(e.data)) } catch { }
  })
  es.addEventListener('rf_signal', (e) => {
    lastSseMs = Date.now()
    let raw; try { raw = JSON.parse(e.data) } catch { return }
    const ev = normalizeEvent(raw, ++evSeq)
    ev.meas = ev.decoded ? measurements(raw) : []
    pendingEvents.push(ev)
    if (pendingEvents.length > MAX_PENDING) pendingEvents.splice(0, pendingEvents.length - MAX_PENDING)
    for (const cb of signalListeners) { try { cb(ev) } catch { } }
    scheduleFlush()
  })
}

function onVisibility() {
  if (!sseWanted) return
  if (document.hidden) {
    clearTimeout(hiddenTimer)
    hiddenTimer = setTimeout(() => {
      if (!document.hidden) return
      closeES(); clearTimeout(reconnectTimer); conn.value = 'paused'
    }, HIDDEN_CLOSE_MS)
    return
  }
  clearTimeout(hiddenTimer)
  if (!es || Date.now() - lastSseMs > 10000) connectSSE()
  sched.resumeHidden()
}

// ---------- Polling ----------
const sched = createScheduler()
let lastProfiles = { '433': null, '868': null }

async function pollRf() {
  const o = await api('/api/rf/overview')
  const now = Date.now()
  const m433 = o.modules.find(m => m.band === '433'), m868 = o.modules.find(m => m.band === '868')
  const h = rssiHist
  const nf = (v) => v != null && v > -128 ? v : null      // same rule for both bands (review 07 N4)
  h.t.push(now / 1000)
  h.r433.push(nf(m433?.rssi)); h.n433.push(nf(m433?.noiseFloor)); h.th433.push(m433?.threshold ?? null)
  h.r868.push(nf(m868?.rssi)); h.n868.push(nf(m868?.noiseFloor)); h.th868.push(m868?.threshold ?? null)
  if (h.t.length > HIST) for (const k in h) h[k].splice(0, h[k].length - HIST)
  const tl = timeline.value
  let tlChanged = false
  const next = { '433': tl['433'], '868': tl['868'] }
  for (const m of o.modules) {
    if (m.profile && m.profile !== lastProfiles[m.band]) {
      next[m.band] = [...next[m.band], { t: now - (m.profileElapsedMs || 0), id: m.profile }].slice(-200)
      lastProfiles[m.band] = m.profile; tlChanged = true
    }
  }
  batch(() => {
    rf.value = o
    lastPoll.value = now
    histTick.value++
    if (tlChanged) timeline.value = next
  })
}

async function pollSys() {
  // one after the other, never in parallel (review 07 H1)
  const s = await api('/api/system')
  const w = await api('/api/wifi/status')
  const prev = sys.value
  batch(() => { sys.value = s; wifi.value = w })
  const pruned = pruneDevices(devices.value)
  if (pruned !== devices.value) devices.value = pruned
  // Gateway rebooted (uptime went backwards): reload state, restart the stream
  if (prev && s.uptime_ms < prev.uptime_ms) {
    notice.value = 'Gerät neu gestartet – Daten neu geladen'
    setTimeout(() => { notice.value = null }, 8000)
    loadDbDevices(); sched.kick('mqtt'); scheduleReconnect(500)
  }
}

async function pollMqttTask() { mqtt.value = await api('/api/mqtt') }
/** Refresh MQTT status now (never overlaps the scheduled poll) */
export const pollMqtt = () => sched.kick('mqtt')
export const refreshRf = () => sched.kick('rf')

// Intervals depend on what is on screen. Budget: SSE carries the live data;
// polls only for state that has no event yet.
const rfEvery = () => route.value.page === 'radio' ? 1000 : expert.value ? 2000 : 3000
const mqttEvery = () => (route.value.page === 'mqtt' || route.value.arg === 'mqtt') ? 5000 : 30000
const updBusy = () => ['checking', 'downloading', 'installing'].includes(update.value?.state)
const updateEvery = () => updBusy() ? 2000 : (route.value.page === 'settings' && route.value.arg === 'device') ? 15000 : 600000

async function pollUpdateTask() { update.value = await api('/api/update') }
/** Refresh update status now */
export const pollUpdate = () => sched.kick('update')

function markOnline(ok) {
  if (ok) {
    if (!online.value) batch(() => { online.value = true; offlineSince.value = 0 })
  } else if (online.value && sched.failures('rf') >= 2) {
    batch(() => { online.value = false; offlineSince.value = lastPoll.value || Date.now() })
  }
}

sched.add('rf', pollRf, { every: rfEvery, offset: 0, onResult: markOnline })
sched.add('sys', async () => { await pollSys(); if (!dbLoaded) await loadDbDevices() }, { every: 15000, offset: 600 })
sched.add('mqtt', pollMqttTask, { every: mqttEvery, offset: 1800 })
sched.add('update', pollUpdateTask, { every: updateEvery, offset: 2400 })

let dbLoaded = false
export async function loadDbDevices() {
  try {
    const list = await api('/api/rf/devices')
    const s = sys.value || await api('/api/system')
    if (!Array.isArray(list)) return
    dbLoaded = true
    const map = new Map(devices.value)
    for (const d of list) {
      const key = d.device_key
      const lastSeen = dbTimeToEpoch(d.last_seen, s.uptime_ms)
      const firstSeen = dbTimeToEpoch(d.first_seen, s.uptime_ms)
      const ex = map.get(key)
      const fromDb = {
        persisted: true, count: Math.max(d.signal_count || 0, ex?.count || 0),
        mqttEnabled: d.mqtt_enabled === 1 || d.mqtt_enabled === true,
      }
      if (ex) { map.set(key, { ...ex, ...fromDb }); continue }
      const raw = { model: d.model, id: d.device_id }
      if (d.temperature != null) raw.temperature_C = d.temperature
      if (d.humidity != null) raw.humidity = d.humidity
      if (d.battery_ok >= 0) raw.battery_ok = d.battery_ok
      map.set(key, {
        key, model: d.model, devId: d.device_id >= 0 ? d.device_id : null, channel: d.channel >= 0 ? d.channel : null,
        freq: d.frequency || null, band: d.frequency ? (d.frequency > 600 ? '868' : '433') : null,
        decoded: true, classified: false, sessionCount: 0, firstSeen, lastSeen, staleBoot: lastSeen == null,
        rssi: d.rssi, last: raw, meas: measurements(raw), history: [], intervals: [], profiles: {}, ...fromDb,
      })
    }
    devices.value = map
  } catch { }
}

// Route or mode change: apply the new intervals at once
let intervalsKey = ''
effect(() => {
  const k = `${rfEvery()}|${mqttEvery()}|${updateEvery()}`
  if (k === intervalsKey) return
  intervalsKey = k
  sched.reschedule('rf'); sched.reschedule('mqtt'); sched.reschedule('update')
})

/** Start polls and the event stream (only after sign-in) */
export function startLive() {
  if (sseWanted) return
  sseWanted = true
  sseFails = 0
  online.value = true; offlineSince.value = 0
  connectSSE()
  sched.start()
  document.addEventListener('visibilitychange', onVisibility)
  // Stream watchdog: no event for 10 s → rebuild the EventSource
  watchdog = setInterval(() => {
    if (!document.hidden && es && Date.now() - lastSseMs > 10000) scheduleReconnect(0)
  }, 3000)
}

/** Stop everything that talks to the gateway (sign-out, expired session) */
export function stopLive() {
  if (!sseWanted) return
  sseWanted = false
  sched.stop()
  closeES()
  clearTimeout(reconnectTimer); clearTimeout(hiddenTimer); clearInterval(watchdog)
  document.removeEventListener('visibilitychange', onVisibility)
  conn.value = 'offline'
}

/** Rebuild the event stream now (the gateway closes all streams on a password change) */
export function reconnectLive() { if (sseWanted) { sseFails = 0; scheduleReconnect(300) } }

/** Retry now (banner button) */
export function retryNow() {
  sched.kick('rf')
  if (sseWanted && conn.value !== 'live') connectSSE()
}

// ---------- Actions ----------
export const actions = {
  pinProfile: (id, durationMs) => act(api('/api/rf/profile', { json: { profile: id, durationMs } }), `Profil ${id} fixiert`).then(refreshRf),
  setRotation: (on) => act(api('/api/rf/profile', { json: { rotation: on } }), on ? 'Rotation aktiv' : 'Rotation gestoppt (Heimprofile)').then(refreshRf),
  setSensitivity: (band, level) => act(api('/api/rf/sensitivity', { json: { module: band, level } }), `Empfindlichkeit ${band} MHz gesetzt`).then(refreshRf),
  persist: async (d) => {
    await act(api('/api/rf/devices/persist', { json: {
      device_key: d.key, model: d.model, device_id: d.devId ?? -1, channel: d.channel ?? -1,
      temperature: d.last?.temperature_C ?? null, humidity: d.last?.humidity ?? null,
      battery_ok: d.last?.battery_ok ?? -1, rssi: d.rssi ?? -128, frequency: d.freq ?? 0 } }), `${d.key} gespeichert`)
    patchDevice(d.key, { persisted: true })
  },
  unpersist: async (d) => {
    await act(api('/api/rf/devices/delete', { json: { device_key: d.key } }), `${d.key} entfernt`)
    patchDevice(d.key, { persisted: false, mqttEnabled: false })
  },
  toggleMqtt: async (d) => {
    const on = !d.mqttEnabled
    await act(api('/api/mqtt/device', { json: { device_key: d.key, enabled: on } }),
      on ? `${d.model || d.key} wird per MQTT gesendet` : `${d.model || d.key} wird nicht mehr per MQTT gesendet`)
    patchDevice(d.key, { mqttEnabled: on })
  },
  saveMqtt: (cfg) => act(api('/api/mqtt', { json: cfg, timeoutMs: 10000 }), 'MQTT-Einstellungen gespeichert').then(r => { mqtt.value = r; return r }),
  clearDb: () => act(api('/api/rf/devices', { method: 'DELETE' }), 'Gerätedatenbank geleert').then(() => {
    const map = new Map()
    for (const [k, d] of devices.value) map.set(k, { ...d, persisted: false, mqttEnabled: false })
    devices.value = map
  }),
}
function patchDevice(key, patch) {
  const map = new Map(devices.value)
  const d = map.get(key)
  if (d) { map.set(key, { ...d, ...patch }); devices.value = map }
}
