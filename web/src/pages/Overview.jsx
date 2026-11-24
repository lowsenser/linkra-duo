import { Activity, Radio, Boxes, Share2, Send, Wifi, Waves, Thermometer } from 'lucide-preact'
import { Card, Empty, Rssi, BandChip, Chip } from '../components/ui'
import { RadioCard } from '../components/RadioCard'
import { RssiChart } from '../components/RssiChart'
import { EventTable } from '../components/EventTable'
import { Waterfall, WaterfallLegend } from '../components/Waterfall'
import { BandLive } from '../components/BandLive'
import { wifi, rf, mqtt, events, deviceList } from '../lib/store'
import { fmtAgo, readingRank as rank, isWmbus } from '../lib/format'
import { expert, variant } from '../lib/prefs'
import { ConsoleOverview } from './OverviewConsole'
import { go } from '../lib/router'


export function Mini({ icon: Icon, label, value, sub, tone = '' }) {
  return (
    <div class="card px-4 py-3 min-w-0">
      <div class="flex items-center gap-1.5 label"><Icon class="w-3.5 h-3.5" />{label}</div>
      <div class={`mt-1 text-xl font-semibold num truncate ${tone}`}>{value}</div>
      {sub && <div class="text-[11px] text-muted truncate">{sub}</div>}
    </div>
  )
}

export function DeviceTile({ d }) {
  // most useful readings first: temperature, humidity, then other physical values
  const main = (d.meas || []).filter(m => rank(m.k) < 9).sort((a, b) => rank(a.k) - rank(b.k))
  const fresh = d.lastSeen && Date.now() - d.lastSeen < 300000
  return (
    <button class="card p-4 text-left hover:border-accent/50 transition-colors group" onClick={() => go('devices', d.key)}>
      <div class="flex items-center gap-2">
        <BandChip band={d.band} />
        <span class="font-medium truncate flex-1">{d.model}<span class="text-muted font-mono text-xs">{d.devId != null ? ` #${d.devId}` : ''}</span></span>
        {d.mqttEnabled && <Send class="w-4 h-4 text-accent" aria-label="wird per MQTT gesendet" />}
      </div>
      <div class="mt-3 flex items-baseline gap-4 min-h-[36px]">
        {main.slice(0, 2).map((m, i) => (
          <div key={m.k} class={i === 0 ? 'text-[26px] leading-none font-semibold num' : 'text-[15px] text-muted num'}>{m.value}</div>
        ))}
        {main.length === 0 && <div class="text-muted text-sm">keine Messwerte</div>}
      </div>
      <div class="mt-3 flex items-center justify-between text-[11px] text-muted">
        <span class="inline-flex items-center gap-1.5"><span class={`w-1.5 h-1.5 rounded-full ${fresh ? 'bg-ok' : 'bg-muted/50'}`} />{d.lastSeen ? fmtAgo(d.lastSeen) : 'vor Neustart'}</span>
        <Rssi v={d.rssi} />
      </div>
    </button>
  )
}

export function Overview() {
  return variant.value === 'console' ? <ConsoleOverview /> : <StudioOverview />
}

function StudioOverview() {
  const w = wifi.value, o = rf.value, mq = mqtt.value
  const now = Date.now()
  const all = deviceList.value.filter(d => d.decoded)
  const activeDevs = all.filter(d => d.lastSeen && now - d.lastSeen < 600000)
  const mine = all.filter(d => d.persisted && !isWmbus(d)).sort((a, b) => (b.lastSeen || 0) - (a.lastSeen || 0))
  const perMin = events.value.filter(e => e.decoded && now - e.t < 60000).length
  const meters = all.filter(d => isWmbus(d) && d.lastSeen && now - d.lastSeen < 3600000).length

  return (
    <div class="page-in space-y-3">
      <div class="grid md:grid-cols-2 xl:grid-cols-12 gap-3">
        <div class="xl:col-span-4"><BandLive band="433" /></div>
        <div class="xl:col-span-4"><BandLive band="868" /></div>
        <div class="md:col-span-2 xl:col-span-4 grid grid-cols-2 gap-3">
          <Mini icon={Boxes} label="Aktive Geräte" value={activeDevs.length} sub={`letzte 10 min · ${meters} Zähler`} />
          <Mini icon={Activity} label="Telegramme" value={perMin} sub="decodiert pro Minute" />
          <Mini icon={Share2} label="MQTT" value={!mq ? '—' : !mq.enabled ? 'aus' : mq.connected ? 'verbunden' : 'getrennt'}
            sub={mq?.enabled ? `${deviceList.value.filter(d => d.mqttEnabled).length} Sensor(en) freigegeben` : 'für Home Assistant'}
            tone={!mq?.enabled ? 'text-muted' : mq.connected ? 'text-ok' : 'text-warn'} />
          <Mini icon={Wifi} label="WLAN" value={w?.connected ? `${w.rssi} dBm` : 'getrennt'} sub={w?.ssid || ''} tone={w?.connected ? '' : 'text-bad'} />
        </div>
      </div>

      <Card title="Wasserfall" icon={Waves} right={<WaterfallLegend />} body="p-3">
        <Waterfall height={230} />
      </Card>

      <Card title="Meine Geräte" icon={Thermometer} body="p-3" right={<button class="btn btn-sm" onClick={() => go('devices')}>Alle Geräte</button>}>
        {mine.length === 0 ? <Empty icon={Radio} title="Noch keine gespeicherten Geräte">Sensoren werden nach dem zweiten Empfang automatisch gespeichert</Empty> : (
          <div class="grid sm:grid-cols-2 lg:grid-cols-3 xl:grid-cols-4 gap-3">
            {mine.map(d => <DeviceTile key={d.key} d={d} />)}
          </div>
        )}
      </Card>

      {expert.value && <>
        <div class="grid lg:grid-cols-2 gap-3">
          <RadioCard m={o?.modules.find(m => m.band === '433')} profiles={o?.profiles} pinned={o?.pinned} />
          <RadioCard m={o?.modules.find(m => m.band === '868')} profiles={o?.profiles} pinned={o?.pinned} />
        </div>
        <Card title="Pegelverlauf (5 min)" icon={Activity} right={<button class="btn btn-sm" onClick={() => go('radio')}>Details</button>}>
          <RssiChart height={200} />
        </Card>
        <Card title="Letzte Telegramme" icon={Radio} body="p-0" right={<button class="btn btn-sm" onClick={() => go('live')}>Live-Ansicht</button>}>
          {events.value.length === 0 ? <Empty icon={Radio} title="Warte auf Signale …" /> : <EventTable rows={events.value.slice(0, 10)} compact />}
        </Card>
      </>}
    </div>
  )
}
