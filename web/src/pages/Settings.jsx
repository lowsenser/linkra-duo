import { useState } from 'preact/hooks'
import { Palette, Wifi, Share2, Cpu, Sun, Moon, Monitor, FlaskConical, Sparkles, Search, Lock, Power, RotateCcw, LayoutPanelTop, Terminal, UserRound } from 'lucide-preact'
import { Card, PageHead, Toggle, KV, Chip } from '../components/ui'
import { MqttPanel } from './Mqtt'
import { wifi, sys, api, act, toast } from '../lib/store'
import { themePref, expert, motion, variant } from '../lib/prefs'
import { route, go } from '../lib/router'
import { fmtDuration } from '../lib/format'
import { lazy } from '../components/shell'

// 2FA setup and the QR encoder load only when the tab is opened
const AccountPanel = lazy(() => import('./Account').then(m => m.AccountPanel))

const TABS = [['look', 'Darstellung', Palette], ['account', 'Konto', UserRound], ['wifi', 'WLAN', Wifi], ['mqtt', 'MQTT', Share2], ['device', 'Gerät', Cpu]]

function Row({ title, desc, children, id }) {
  return (
    <div class="flex items-center justify-between gap-6 py-3 border-b border-line/60 last:border-0">
      <div class="min-w-0">
        <div class="font-medium" id={id}>{title}</div>
        {desc && <div class="text-[12.5px] text-muted mt-0.5 max-w-xl">{desc}</div>}
      </div>
      <div class="shrink-0">{children}</div>
    </div>
  )
}

function ThemePicker() {
  const opts = [['system', 'System', Monitor], ['light', 'Hell', Sun], ['dark', 'Dunkel', Moon]]
  return (
    <div class="inline-flex p-0.5 rounded-xl bg-raised border border-line" role="group" aria-label="Farbschema">
      {opts.map(([v, l, Icon]) => (
        <button key={v} type="button" aria-pressed={themePref.value === v} onClick={() => { themePref.value = v }}
          class={`inline-flex items-center gap-1.5 px-3 h-8 rounded-lg text-[13px] font-medium transition-colors ${themePref.value === v ? 'bg-panel text-accent shadow-sm' : 'text-muted hover:text-fg'}`}>
          <Icon class="w-4 h-4" />{l}
        </button>
      ))}
    </div>
  )
}

function VariantPicker() {
  const opts = [
    ['studio', 'Studio', LayoutPanelTop, 'Große Pegelanzeigen, Kacheln für deine Sensoren, Wasserfall.'],
    ['console', 'Konsole', Terminal, 'Wie SDR-Software: Statuszeile, großer Wasserfall mit LED-Pegeln, Sensortabelle, Empfangsprotokoll.'],
  ]
  return (
    <div class="grid sm:grid-cols-2 gap-3 py-3">
      {opts.map(([v, l, Icon, d]) => (
        <button key={v} type="button" aria-pressed={variant.value === v} onClick={() => { variant.value = v }}
          class={`text-left p-4 rounded-xl border transition-colors ${variant.value === v ? 'border-accent bg-accent/10' : 'border-line hover:border-accent/50'}`}>
          <div class={`flex items-center gap-2 font-semibold ${variant.value === v ? 'text-accent' : ''}`}><Icon class="w-4 h-4" />{l}</div>
          <div class="text-[12.5px] text-muted mt-1">{d}</div>
        </button>
      ))}
    </div>
  )
}

function Look() {
  return (
    <Card title="Darstellung" icon={Palette} body="px-4 py-1">
      <div class="pt-3 font-medium">Design</div>
      <VariantPicker />
      <Row title="Farbschema" desc="„System“ folgt der Einstellung deines Geräts."><ThemePicker /></Row>
      <Row id="pref-expert" title={<span class="inline-flex items-center gap-2">Expertenmodus <FlaskConical class="w-4 h-4 text-accent" aria-hidden="true" /></span>}
        desc="Zeigt alle technischen Ansichten: Live-Telegramme, Empfangsprofile und Tuning, Systemdiagnose, Pegelverläufe und Rohdaten.">
        <Toggle labelledby="pref-expert" checked={expert.value} onChange={(v) => { expert.value = v }} />
      </Row>
      <Row id="pref-motion" title={<span class="inline-flex items-center gap-2">Animationen <Sparkles class="w-4 h-4 text-muted" aria-hidden="true" /></span>}
        desc="Übergänge und Leuchteffekte. Ausschalten spart Rechenleistung auf schwachen Geräten. Standard folgt der Systemeinstellung „Bewegung reduzieren“.">
        <Toggle labelledby="pref-motion" checked={motion.value} onChange={(v) => { motion.value = v }} />
      </Row>
      <div class="text-[11px] text-muted py-2">Diese Einstellungen gelten nur für diesen Browser.</div>
    </Card>
  )
}

function WifiSettings() {
  const w = wifi.value
  const [nets, setNets] = useState(null)
  const [scanning, setScanning] = useState(false)
  const [sel, setSel] = useState(null)
  const [pw, setPw] = useState('')
  const scan = async () => {
    setScanning(true)
    try {
      const r = await api('/api/wifi/scan', { timeoutMs: 20000 })
      const best = new Map()
      for (const n of r.networks || []) if (n.ssid && (!best.has(n.ssid) || best.get(n.ssid).rssi < n.rssi)) best.set(n.ssid, n)
      setNets([...best.values()].sort((a, b) => b.rssi - a.rssi))
    } catch (e) { toast(e.message, 'bad') }
    setScanning(false)
  }
  const connect = () => {
    if (!sel) return
    if (!confirm(`Mit „${sel.ssid}“ verbinden? Die Verbindung zu dieser Seite bricht dabei ab. Erreichbar ist das Gateway danach unter der neuen IP-Adresse oder http://linkra-duo.local`)) return
    act(api('/api/wifi/settings/connect', { json: { ssid: sel.ssid, password: pw } }), 'Verbinde …')
  }
  const reset = () => confirm('Gespeicherte WLAN-Zugangsdaten löschen und neu starten? Das Gerät startet danach im Einrichtungsmodus (eigener Access-Point).') &&
    act(api('/api/config/reset', { method: 'POST' }), 'WLAN-Konfiguration gelöscht')
  const bars = (r) => r > -55 ? 4 : r > -67 ? 3 : r > -78 ? 2 : 1

  return (
    <div class="grid lg:grid-cols-2 gap-3">
      <Card title="Verbindung" icon={Wifi}>
        <KV rows={[
          ['Status', w?.connected ? <Chip tone="ok">verbunden</Chip> : <Chip tone="bad">{w?.mode || '—'}</Chip>],
          ['Netzwerk', w?.ssid], ['IP-Adresse', w?.ip], ['Signal', w?.rssi != null && `${w.rssi} dBm · Kanal ${w.channel}`, w?.rssi < -80 ? 'text-warn' : ''],
          ['Gateway', w?.gateway], ['DNS', w?.dns], ['MAC', w?.mac], ['Hostname', w?.hostname],
        ]} />
        <button class="btn-danger mt-4" onClick={reset}><RotateCcw class="w-4 h-4" />Zugangsdaten zurücksetzen</button>
      </Card>
      <Card title="Netzwerk wechseln" icon={Search} right={<button class="btn btn-sm" disabled={scanning} onClick={scan}>{scanning ? 'Suche …' : 'Suchen'}</button>}>
        {!nets ? <div class="text-[13px] text-muted">Nach WLAN-Netzen in Reichweite suchen. Die Suche dauert einige Sekunden.</div> : (
          <ul class="divide-y divide-line/60 max-h-64 overflow-auto -mx-1">
            {nets.map(n => (
              <li key={n.ssid}>
                <button class={`w-full flex items-center gap-3 px-2 py-2 rounded-lg text-left ${sel?.ssid === n.ssid ? 'bg-accent/10 text-accent' : 'hover:bg-raised'}`} onClick={() => { setSel(n); setPw('') }}>
                  <span class="flex items-end gap-0.5 h-3.5">{[1, 2, 3, 4].map(b => <span key={b} class={`w-1 rounded-sm ${b <= bars(n.rssi) ? 'bg-current' : 'bg-line'}`} style={{ height: `${b * 25}%` }} />)}</span>
                  <span class="flex-1 truncate">{n.ssid}</span>
                  {n.secure && <Lock class="w-3.5 h-3.5 text-muted" />}
                  <span class="text-[11px] text-muted num">{n.rssi} dBm · K{n.channel}</span>
                </button>
              </li>
            ))}
          </ul>
        )}
        {sel && (
          <div class="mt-3 flex gap-2">
            {sel.secure && <input class="input flex-1" type="password" aria-label={`WLAN-Passwort für ${sel.ssid}`} placeholder="Passwort" autocomplete="off" value={pw} onInput={e => setPw(e.target.value)} />}
            <button class="btn-primary" onClick={connect}>Verbinden</button>
          </div>
        )}
      </Card>
    </div>
  )
}

function DeviceSettings() {
  const s = sys.value
  const reboot = () => confirm('Gateway jetzt neu starten?') && act(api('/api/reboot', { method: 'POST' }), 'Neustart ausgelöst')
  return (
    <Card title="Gerät" icon={Cpu}>
      <KV rows={[
        ['Firmware', s && `${s.version} · ${s.build}`], ['Hardware', s?.hardware], ['Chip', s && `${s.chip} · ${s.cpu_mhz} MHz`],
        ['Laufzeit', fmtDuration(s?.uptime)], ['Temperatur', s?.temp_c != null ? `${s.temp_c.toFixed(1)} °C` : null],
      ]} />
      <div class="flex flex-wrap gap-2 mt-4">
        <button class="btn" onClick={reboot}><Power class="w-4 h-4" />Neu starten</button>
        {expert.value && <button class="btn" onClick={() => go('system')}><Cpu class="w-4 h-4" />Systemdiagnose</button>}
      </div>
    </Card>
  )
}

export function SettingsPage() {
  const tab = TABS.some(t => t[0] === route.value.arg) ? route.value.arg : (route.value.page === 'mqtt' ? 'mqtt' : 'look')
  return (
    <div class="page-in">
      <PageHead sub="Darstellung, Konto, Netzwerk, Home Assistant (MQTT) und Gerät" />
      <nav class="flex gap-1 mb-4 overflow-x-auto overflow-y-hidden border-b border-line" aria-label="Einstellungsbereiche">
        {TABS.map(([id, label, Icon]) => (
          <a key={id} href={`#settings/${id}`} aria-current={tab === id ? 'page' : undefined}
            class={`inline-flex items-center gap-2 px-3 h-10 -mb-px border-b-2 text-[13px] font-medium whitespace-nowrap transition-colors ${tab === id ? 'border-accent text-accent' : 'border-transparent text-muted hover:text-fg'}`}>
            <Icon class="w-4 h-4" aria-hidden="true" />{label}
          </a>
        ))}
      </nav>
      {tab === 'look' && <Look />}
      {tab === 'account' && <AccountPanel />}
      {tab === 'wifi' && <WifiSettings />}
      {tab === 'mqtt' && <MqttPanel />}
      {tab === 'device' && <DeviceSettings />}
    </div>
  )
}
