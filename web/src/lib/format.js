// Pure helpers (unit-tested in tests/format.test.js)

export const fmtBytes = (b) => {
  if (b == null || isNaN(b)) return '—'
  if (b < 1024) return `${b} B`
  if (b < 1048576) return `${(b / 1024).toFixed(1)} KB`
  return `${(b / 1048576).toFixed(2)} MB`
}

export const fmtDuration = (sec) => {
  if (sec == null || isNaN(sec)) return '—'
  sec = Math.max(0, Math.floor(sec))
  const d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600)
  const m = Math.floor((sec % 3600) / 60), s = sec % 60
  if (d) return `${d}d ${h}h ${m}m`
  if (h) return `${h}h ${m}m`
  if (m) return `${m}m ${s}s`
  return `${s}s`
}

/** "vor 12 s" style relative age from a ms timestamp */
export const fmtAgo = (ts, now = Date.now()) => {
  if (!ts) return '—'
  const s = Math.max(0, Math.round((now - ts) / 1000))
  if (s < 5) return 'jetzt'
  if (s < 60) return `vor ${s} s`
  if (s < 3600) return `vor ${Math.floor(s / 60)} min`
  if (s < 86400) return `vor ${Math.floor(s / 3600)} h`
  return `vor ${Math.floor(s / 86400)} d`
}

export const fmtTime = (ts) => ts ? new Date(ts).toLocaleTimeString('de-DE') : '—'
export const fmtMHz = (f, d = 2) => (f == null || isNaN(f) || f <= 0) ? '—' : `${Number(f).toFixed(d)} MHz`

/** Band from explicit band or frequency. Never guesses from a missing value. */
export const bandOf = (sig) => {
  if (sig.band != null && sig.band !== '') return String(sig.band)
  const f = Number(sig.frequency)
  if (Number.isFinite(f) && f > 0) return f > 600 ? '868' : '433'
  return null
}

export const rssiClass = (r) => r == null ? 'text-muted' : r > -70 ? 'text-ok' : r > -90 ? 'text-warn' : 'text-bad'

/** Fields of a decoded rtl_433 message that are metadata, not measurement */
export const META_KEYS = new Set(['model', 'id', 'channel', 'protocol', 'decoded', 'classified', 'status', 'unconfirmed',
  'frequency', 'band', 'rx_profile', 'modulation', 'rssi', 'persisted', 'time', 'mic', 'fingerprint',
  'duration', 'pulses', 'receivedAt', 'seq', 'raw', 'lqi', 'C', 'ci', 'access_no', 'version', 'device_type',
  'mode', 'driver', 'time_ms', 'data', 'data_raw', 'dst', 'flags', 'type', 'type_string', 'M'])

const LABELS = {
  temperature_C: ['Temp', '°C', 1], temperature_F: ['Temp', '°F', 1], humidity: ['Feuchte', '%', 0],
  battery_ok: ['Batterie', '', 0], pressure_hPa: ['Druck', 'hPa', 1], wind_avg_km_h: ['Wind', 'km/h', 1],
  wind_max_km_h: ['Böe', 'km/h', 1], wind_dir_deg: ['Windrichtung', '°', 0], rain_mm: ['Regen', 'mm', 1],
  uv: ['UV', '', 1], light_lux: ['Licht', 'lx', 0], moisture: ['Bodenfeuchte', '%', 0],
  pressure_kPa: ['Reifendruck', 'kPa', 0], power_W: ['Leistung', 'W', 0], energy_kWh: ['Energie', 'kWh', 2],
  newbattery: ['Neue Batterie', '', 0], rolling_code: ['Rolling Code', '', 0], control: ['Taste', '', 0], channel: ['Kanal', '', 0],
  sensor_id: ['Sensor-ID', '', 0], wind_dir: ['Windrichtung', '', 0],   hca_current: ['HKV aktuell', 'Einh.', 0], hca_previous: ['HKV Vorperiode', 'Einh.', 0],
  temp_room_C: ['Raum', '°C', 2], temp_radiator_C: ['Heizkörper', '°C', 2],
  date_current: ['Stand vom', '', 0], date_previous: ['Stichtag', '', 0],
  volume_period_m3: ['Volumen Periode', 'm³', 1], volume_target_m3: ['Volumen Stichtag', 'm³', 1],
  energy_period_kWh: ['Energie Periode', 'kWh', 0], energy_previous_kWh: ['Energie Stichtag', 'kWh', 0],
  smoke_status: ['Rauchmelder', '', 0], media: ['Gerät', '', 0],
  volume_m3: ['Volumen', 'm³', 3], energy_kWh: ['Energie', 'kWh', 1], energy_MJ: ['Energie', 'MJ', 1],
  hca_units: ['HKV-Einheiten', '', 0], flow_m3h: ['Durchfluss', 'm³/h', 3], flow_temp_C: ['Vorlauf', '°C', 1],
  return_temp_C: ['Rücklauf', '°C', 1], device_type_name: ['Gerätetyp', '', 0], manufacturer: ['Hersteller', '', 0],
  encryption: ['Verschlüsselung', '', 0], date: ['Datum', '', 0], datetime: ['Zeitpunkt', '', 0],
  button: ['Taste', '', 0], state: ['Zustand', '', 0], cmd: ['Befehl', '', 0], counter: ['Zähler', '', 0],
}

/** Human readable measurement list of a decoded message */
export function measurements(msg) {
  const out = []
  for (const [k, v] of Object.entries(msg || {})) {
    if (META_KEYS.has(k) || v == null || typeof v === 'object') continue
    const l = LABELS[k]
    if (k === 'battery_ok') { out.push({ k, label: 'Batterie', value: v ? 'OK' : 'LOW', warn: !v }); continue }
    if (l) out.push({ k, label: l[0], value: typeof v === 'number' ? `${v.toFixed(l[2])}${l[1] ? ' ' + l[1] : ''}` : `${v}` })
    else out.push({ k, label: k, value: String(v) })
  }
  return out
}

/** Normalize an rf_signal SSE payload into one event record */
export function normalizeEvent(raw, seq, now = Date.now()) {
  const classified = raw.status === 'classified' || raw.classified === true
  const decoded = !classified && (raw.decoded ?? Boolean(raw.model && !String(raw.model).startsWith('undecoded')))
  const f = Number(raw.frequency)
  return {
    seq, t: now, raw,
    decoded, classified,
    unconfirmed: raw.unconfirmed === true,
    model: raw.model || raw.protocol || 'unbekannt',
    devId: raw.id ?? null,
    channel: raw.channel ?? null,
    band: bandOf(raw),
    freq: Number.isFinite(f) && f > 0 ? f : null,
    profile: raw.rx_profile || null,
    mod: raw.modulation || null,
    rssi: typeof raw.rssi === 'number' ? raw.rssi : null,
  }
}

/** Device key: identical rule as firmware (model_id) so DB and live merge */
export function deviceKey(ev) {
  if (ev.decoded) return ev.devId != null && ev.devId !== '' ? `${ev.model}_${ev.devId}` : ev.model
  if (ev.unconfirmed) return `?${ev.band || 'x'}:unbestätigt`
  return `?${ev.band || 'x'}:${ev.model}`
}

/** Convert DB boot-relative millis to wall clock; null if from a previous boot */
export function dbTimeToEpoch(dbMs, uptimeMs, now = Date.now()) {
  if (dbMs > 1e12) return dbMs                       // firmware stored wall-clock epoch ms (NTP synced)
  if (dbMs == null || uptimeMs == null || dbMs < 0 || dbMs > uptimeMs) return null
  return now - (uptimeMs - dbMs)
}

export function toCSV(rows, cols) {
  const esc = (v) => {
    if (v == null) return ''
    const s = typeof v === 'object' ? JSON.stringify(v) : String(v)
    return /[",\n;]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s
  }
  return [cols.map(c => c[0]).join(';'), ...rows.map(r => cols.map(c => esc(c[1](r))).join(';'))].join('\n')
}

export function download(name, text, type = 'text/plain') {
  const a = document.createElement('a')
  a.href = URL.createObjectURL(new Blob([text], { type }))
  a.download = name
  a.click()
  setTimeout(() => URL.revokeObjectURL(a.href), 1000)
}

/** Readings shown first: temperature, humidity, then other physical values (9 = not a main reading) */
export const readingRank = (k) => /^temperature/.test(k) ? 0 : /^humidity/.test(k) ? 1 : /(pressure|wind|rain|power|energy|volume|total|current|voltage)/.test(k) ? 2 : 9
export const isWmbus = (d) => String(d.model).startsWith('wMBus')

/**
 * Keep the live device map bounded (review 07 M7): saved devices always stay;
 * others are dropped after maxAgeMs without reception, and beyond max entries
 * the least recently seen go first. Returns the same map if nothing changed.
 */
export function pruneDevices(map, now = Date.now(), { max = 500, maxAgeMs = 24 * 3600e3 } = {}) {
  let drop = []
  for (const [k, d] of map) if (!d.persisted && (d.lastSeen ?? 0) < now - maxAgeMs) drop.push(k)
  const left = map.size - drop.length
  if (left > max) {
    const gone = new Set(drop)
    const rest = [...map.values()].filter(d => !d.persisted && !gone.has(d.key))
      .sort((a, b) => (a.lastSeen ?? 0) - (b.lastSeen ?? 0))
    drop = drop.concat(rest.slice(0, left - max).map(d => d.key))
  }
  if (!drop.length) return map
  const out = new Map(map)
  for (const k of drop) out.delete(k)
  return out
}
