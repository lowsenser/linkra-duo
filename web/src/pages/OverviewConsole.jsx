import { Waves, Thermometer, Terminal } from 'lucide-preact'
import { Card, Empty, Spark } from '../components/ui'
import { RadioCard } from '../components/RadioCard'
import { Waterfall, WaterfallLegend } from '../components/Waterfall'
import { VuMeter } from '../components/VuMeter'
import { wifi, rf, mqtt, events, deviceList, liveFrame } from '../lib/store'
import { fmtAgo, fmtMHz, rssiClass, readingRank as rank, isWmbus } from '../lib/format'
import { expert } from '../lib/prefs'
import { go } from '../lib/router'

// Design variant "Konsole": built like SDR software – one status line, a large
// waterfall with LED level meters, a dense sensor table and a telegram log.
const readings = (d) => (d.meas || []).filter(m => rank(m.k) < 9).sort((a, b) => rank(a.k) - rank(b.k))
const time = (t) => new Date(t).toLocaleTimeString('de-DE')

// module level: an inline component would be re-mounted at 5 Hz (review 07 M3)
const Item = ({ k, v, tone = '' }) => <span class="inline-flex items-baseline gap-1.5 whitespace-nowrap"><span class="text-muted">{k}</span><span class={`text-fg ${tone}`}>{v}</span></span>

function StatusLine() {
  const o = rf.value, w = wifi.value, mq = mqtt.value, f = liveFrame.value
  const now = Date.now()
  const active = deviceList.value.filter(d => d.decoded && d.lastSeen && now - d.lastSeen < 600000 && !isWmbus(d)).length
  const perMin = events.value.filter(e => e.decoded && now - e.t < 60000).length
  return (
    <div class="card px-4 py-2.5 mono text-[12px] flex flex-wrap gap-x-6 gap-y-1.5 items-center">
      {['433', '868'].map(b => {
        const m = o?.modules.find(x => x.band === b)
        const wm = b === '868' && f?.wm
        return <span key={b} class="inline-flex items-baseline gap-2 whitespace-nowrap">
          <span class="font-bold" style={{ color: `rgb(var(--b${b}))` }}>{b}</span>
          <span class="text-fg">{m?.profile || '—'}</span>
          <span class="text-muted">{m ? fmtMHz(m.freqMHz) : ''}</span>
          <span class="text-fg num live-val">{wm ? 'Paket' : f?.['p' + b] ?? '—'}{!wm && <span class="text-muted"> dBm</span>}</span>
        </span>
      })}
      <Item k="aktiv" v={active} />
      <Item k="Telegramme/min" v={perMin} />
      <Item k="mqtt" v={!mq ? '—' : !mq.enabled ? 'aus' : mq.connected ? 'verbunden' : 'getrennt'} tone={!mq?.enabled ? 'text-muted' : mq.connected ? 'text-ok' : 'text-warn'} />
      <Item k="wlan" v={w?.connected ? `${w.rssi} dBm` : 'aus'} tone={w?.connected ? '' : 'text-bad'} />
    </div>
  )
}

function SensorTable() {
  const list = deviceList.value.filter(d => d.decoded && d.persisted && !isWmbus(d)).sort((a, b) => (b.lastSeen || 0) - (a.lastSeen || 0))
  if (!list.length) return <Empty title="Noch keine gespeicherten Sensoren" />
  return (
    <div class="overflow-x-auto">
      <table class="tbl mono">
        <thead><tr><th>Band</th><th>Gerät</th><th>Werte</th><th class="hidden md:table-cell">Verlauf</th><th class="text-right">Pegel</th><th class="text-right">Zuletzt</th></tr></thead>
        <tbody>
          {list.map(d => {
            const r = readings(d)
            const tk = r[0]?.k
            const hist = tk ? (d.history || []).map(h => h.v?.[tk]).filter(v => v != null) : []
            const fresh = d.lastSeen && Date.now() - d.lastSeen < 300000
            return (
              <tr key={d.key} class="cursor-pointer" onClick={(e) => { if (e.target.closest('a')) return; go('devices', d.key) }}>
                <td><span class="font-bold" style={{ color: `rgb(var(--b${d.band || '433'}))` }}>{d.band || '?'}</span></td>
                <td class="font-sans"><a href={`#devices/${encodeURIComponent(d.key)}`} class="font-medium hover:text-accent">{d.model}{d.devId != null && <span class="text-muted"> #{d.devId}</span>}</a>{d.mqttEnabled && <span class="ml-2 chip bg-accent/15 text-accent">MQTT</span>}</td>
                <td class="num">{r.length ? r.slice(0, 3).map((m, i) => <span key={m.k} class={i === 0 ? 'text-[15px] font-semibold mr-3' : 'text-muted mr-3'}>{m.value}</span>) : <span class="text-muted">—</span>}</td>
                <td class="hidden md:table-cell text-accent">{hist.length > 1 ? <Spark values={hist} w={110} h={20} /> : <span class="text-muted text-xs">—</span>}</td>
                <td class={`text-right num ${rssiClass(d.rssi)}`}>{d.rssi ?? '—'}</td>
                <td class="text-right text-xs"><span class={`inline-block w-1.5 h-1.5 rounded-full mr-1.5 ${fresh ? 'bg-ok' : 'bg-muted/40'}`} /><span class="text-muted">{d.lastSeen ? fmtAgo(d.lastSeen) : 'vor Neustart'}</span></td>
              </tr>
            )
          })}
        </tbody>
      </table>
    </div>
  )
}

function TelegramLog() {
  const rows = events.value.filter(e => e.decoded).slice(0, expert.value ? 14 : 8)
  if (!rows.length) return <div class="mono text-[12px] text-muted px-4 py-3">warte auf Telegramme …<span class="animate-pulse">▍</span></div>
  return (
    <div class="mono text-[12px] leading-[1.7] px-4 py-2 overflow-x-auto">
      {rows.map(e => {
        const r = (e.raw && Object.keys(e.raw).filter(k => rank(k) < 9).sort((a, b) => rank(a) - rank(b)).slice(0, 2)) || []
        return (
          <div key={e.seq} class="whitespace-nowrap row-new">
            <span class="text-muted">{time(e.t)}</span>
            <span class="ml-3 font-bold" style={{ color: `rgb(var(--b${e.band || '433'}))` }}>{e.band}</span>
            <span class="ml-3 text-muted">{(e.profile || '').padEnd(11, ' ')}</span>
            <span class="ml-3 text-fg">{e.model}{e.devId != null ? ` #${e.devId}` : ''}</span>
            <span class="ml-3 text-accent">{r.map(k => `${k.replace(/_C$/, '').replace('temperature', 'T').replace('humidity', 'H')}=${typeof e.raw[k] === 'number' ? +e.raw[k].toFixed(1) : e.raw[k]}`).join(' ')}</span>
            <span class={`ml-3 ${rssiClass(e.rssi)}`}>{e.rssi} dBm</span>
          </div>
        )
      })}
    </div>
  )
}

export function ConsoleOverview() {
  const o = rf.value
  return (
    <div class="page-in space-y-3">
      <StatusLine />
      <Card title="Wasserfall · 433 / 868 MHz" icon={Waves} right={<WaterfallLegend palette="phosphor" />} body="p-3">
        <div class="flex gap-3 items-stretch">
          <div class="flex-1 min-w-0"><Waterfall height={300} palette="phosphor" /></div>
          <div class="hidden sm:flex gap-1 pt-1"><VuMeter band="433" height={232} /><VuMeter band="868" height={232} /></div>
        </div>
      </Card>
      <div class="grid xl:grid-cols-5 gap-3">
        <Card title="Sensoren" icon={Thermometer} body="p-0" class="xl:col-span-3" right={<button class="btn btn-sm" onClick={() => go('devices')}>Alle</button>}>
          <SensorTable />
        </Card>
        <Card title="Empfangsprotokoll" icon={Terminal} body="p-0" class="xl:col-span-2">
          <TelegramLog />
        </Card>
      </div>
      {expert.value && (
        <div class="grid lg:grid-cols-2 gap-3">
          <RadioCard m={o?.modules.find(m => m.band === '433')} profiles={o?.profiles} pinned={o?.pinned} />
          <RadioCard m={o?.modules.find(m => m.band === '868')} profiles={o?.profiles} pinned={o?.pinned} />
        </div>
      )}
    </div>
  )
}
