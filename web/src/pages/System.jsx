import { useEffect, useState } from 'preact/hooks'
import { Cpu, Wifi, HardDrive, Power, Upload, FolderUp, AlertTriangle, MemoryStick } from 'lucide-preact'
import { Card, PageHead, KV, Progress, Chip } from '../components/ui'
import { sys, wifi, api, act, toast } from '../lib/store'
import { ApiError } from '../lib/http'
import { fmtBytes, fmtDuration } from '../lib/format'

const RESET = { poweron: 'Einschalten', sw: 'Software-Neustart', panic: 'Absturz (Panic)', int_wdt: 'Interrupt-Watchdog', task_wdt: 'Task-Watchdog', wdt: 'Watchdog', brownout: 'Unterspannung', ext: 'Externer Reset', deepsleep: 'Deep Sleep', other: 'USB/JTAG-Reset', unknown: 'unbekannt' }

export function SystemPage() {
  const s = sys.value, w = wifi.value
  const [files, setFiles] = useState(null)
  const [busy, setBusy] = useState(null)
  const loadFiles = () => api('/api/fs/list').then(setFiles).catch(() => { })
  useEffect(() => { loadFiles() }, [])

  const reboot = () => confirm('Gateway jetzt neu starten?') && act(api('/api/reboot', { method: 'POST' }), 'Neustart ausgelöst')
  const wifiReset = () => confirm('Gespeicherte WLAN-Zugangsdaten löschen und neu starten? Das Gerät startet danach im Einrichtungsmodus.') && act(api('/api/config/reset', { method: 'POST' }), 'WLAN-Konfiguration gelöscht')

  const uploadWeb = async (fileList) => {
    const relOf = (f) => (f.webkitRelativePath || f.name).split('/').slice(1).join('/') || f.name
    // assets first, index.html last: an aborted upload never points at missing files (review 07 H4)
    const list = [...fileList].filter(f => /(^|\/)(index\.html|assets\/.+)$/.test(f.webkitRelativePath || f.name))
      .sort((a, b) => (relOf(a) === 'index.html') - (relOf(b) === 'index.html'))
    if (!list.some(f => relOf(f) === 'index.html')) return toast('Ordner „data“ mit index.html und assets/ wählen', 'bad')
    const total = list.reduce((sum, f) => sum + f.size, 0)
    if (s && s.fs_total && total > s.fs_total - s.fs_used + 200000) return toast(`Zu wenig Platz auf dem Gerät (${fmtBytes(total)} nötig)`, 'bad')
    const keep = new Set()
    try {
      for (let i = 0; i < list.length; i++) {
        const f = list[i], rel = relOf(f), path = '/' + rel
        keep.add(path)
        setBusy(`${i + 1}/${list.length}: ${rel}`)
        const fd = new FormData(); fd.append('file', f, rel)
        await api(`/api/fs/upload?path=${encodeURIComponent(path)}`, { method: 'POST', body: fd, timeoutMs: 60000 })
      }
      // remove assets of the previous build
      const cur = await api('/api/fs/list')
      for (const f of cur.files) if (f.path.startsWith('/assets/') && !keep.has(f.path)) await api(`/api/fs/delete?path=${encodeURIComponent(f.path)}`, { method: 'POST' })
    } catch (e) {
      setBusy(null)
      return toast(`Upload fehlgeschlagen: ${e instanceof ApiError ? e.message : e}`, 'bad')
    }
    setBusy(null); toast('Weboberfläche aktualisiert – lade neu …', 'ok')
    setTimeout(() => location.reload(), 1200)
  }

  const uploadFw = async (file) => {
    if (!file) return
    if (!confirm(`Firmware „${file.name}“ (${fmtBytes(file.size)}) flashen? Das Gerät startet danach neu.`)) return
    const fd = new FormData(); fd.append('firmware', file, file.name)
    setBusy('Firmware wird übertragen …')
    // large upload over WLAN: long timeout (review 07 H3)
    try { await api('/api/ota', { method: 'POST', body: fd, timeoutMs: 180000 }); toast('Firmware übertragen, Neustart …', 'ok') }
    catch (e) { toast(e.message, 'bad') }
    setBusy(null)
  }

  return (
    <>
      <PageHead title="System" sub="Hardware, Speicher, Netzwerk und Wartung">
        <button class="btn" onClick={reboot}><Power class="w-4 h-4" />Neustart</button>
      </PageHead>

      <div class="grid lg:grid-cols-3 gap-3 mb-3">
        <Card title="Firmware & Chip" icon={Cpu}>
          <KV rows={[
            ['Firmware', s?.version], ['Build', s?.build], ['ESP-IDF', s?.idf],
            ['Chip', s && `${s.chip} rev ${s.chip_rev} · ${s.cores} Kerne · ${s.cpu_mhz} MHz`],
            ['Chiptemperatur', s?.temp_c != null ? `${s.temp_c.toFixed(1)} °C` : null],
            ['Laufzeit', fmtDuration(s?.uptime)],
            ['Letzter Reset', s && (RESET[s.reset_reason] || s.reset_reason), s?.reset_reason === 'panic' || s?.reset_reason?.includes('wdt') ? 'text-bad' : ''],
          ]} />
        </Card>
        <Card title="Speicher" icon={MemoryStick}>
          {s && <>
            <MemRow label="Interner Heap" used={s.heap_total - s.free_heap} total={s.heap_total} />
            <div class="text-xs text-muted mb-3 num">Minimum seit Start {fmtBytes(s.min_heap)} · größter Block {fmtBytes(s.max_alloc)}</div>
            <MemRow label="PSRAM" used={s.psram_total - s.psram_free} total={s.psram_total} />
            <MemRow label={`Flash (App-Partition ${s.partition})`} used={s.sketch_size} total={s.partition_size || s.flash_size} />
            <MemRow label="LittleFS" used={s.fs_used} total={s.fs_total} />
            <KV rows={[['Flash gesamt', fmtBytes(s.flash_size)], ['Laufende Partition', s.partition]]} />
          </>}
        </Card>
        <Card title="Netzwerk" icon={Wifi}>
          <KV rows={[
            ['Status', w?.connected ? <Chip tone="ok">verbunden</Chip> : <Chip tone="bad">{w?.mode || '—'}</Chip>],
            ['SSID', w?.ssid], ['IP-Adresse', w?.ip], ['Gateway', w?.gateway], ['DNS', w?.dns], ['Subnetz', w?.subnet],
            ['Signal', w?.rssi != null && `${w.rssi} dBm · Kanal ${w.channel}`, w?.rssi < -80 ? 'text-warn' : ''],
            ['BSSID', w?.bssid], ['MAC', w?.mac], ['Hostname', w?.hostname],
          ]} />
          <button class="btn-danger mt-3" onClick={wifiReset}>WLAN-Konfiguration zurücksetzen</button>
        </Card>
      </div>

      <div class="grid lg:grid-cols-2 gap-3">
        <Card title="Firmware-Update (OTA)" icon={Upload}>
          {s && !s.ota_possible ? (
            <div class="flex gap-3 text-[13px]">
              <AlertTriangle class="w-5 h-5 text-warn shrink-0" />
              <div>
                <div class="font-medium">OTA mit der aktuellen Partitionstabelle nicht möglich.</div>
                <div class="text-muted mt-1">Die 16-MB-Tabelle hat nur eine <span class="font-mono">factory</span>-Partition und keine OTA-Slots. Firmware per USB flashen oder eine Tabelle mit <span class="font-mono">ota_0/ota_1</span> einspielen (einmalig per USB).</div>
              </div>
            </div>
          ) : (
            <label class="block border-2 border-dashed border-line rounded-xl p-6 text-center cursor-pointer hover:border-accent">
              <input type="file" accept=".bin" class="sr-only" disabled={!!busy} onChange={e => uploadFw(e.target.files[0])} />
              <Upload class="w-6 h-6 mx-auto mb-2 text-accent" /><div class="font-medium">firmware.bin wählen</div>
            </label>
          )}
        </Card>
        <Card title="Weboberfläche" icon={HardDrive}>
          <label class="block border-2 border-dashed border-line rounded-xl p-5 text-center cursor-pointer hover:border-accent mb-3">
            <input type="file" class="sr-only" webkitdirectory directory multiple disabled={!!busy} onChange={e => uploadWeb(e.target.files)} />
            <FolderUp class="w-6 h-6 mx-auto mb-2 text-accent" />
            <div class="font-medium">Build-Ordner „data“ hochladen</div>
            <div class="text-xs text-muted mt-1">Ersetzt nur index.html und assets/ – Gerätedatenbank und Einstellungen bleiben erhalten.</div>
          </label>
          {busy && <div class="text-xs text-accent mb-2">{busy}</div>}
          <div class="max-h-48 overflow-auto">
            <table class="tbl"><tbody>
              {(files?.files || []).map(f => <tr key={f.path}><td class="font-mono text-xs">{f.path}</td><td class="text-right num text-xs">{fmtBytes(f.size)}</td></tr>)}
            </tbody></table>
          </div>
        </Card>
      </div>
    </>
  )
}

const MemRow = ({ label, used, total }) => (
  <div class="mb-3">
    <div class="flex justify-between text-[13px] mb-1"><span class="text-muted">{label}</span><span class="num">{fmtBytes(used)} / {fmtBytes(total)}</span></div>
    <Progress value={used} max={total} tone={used / total > 0.85 ? 'bg-warn' : 'bg-accent'} />
  </div>
)
