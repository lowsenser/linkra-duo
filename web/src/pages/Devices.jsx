import { useMemo, useState } from 'preact/hooks'
import { Boxes, Save, Trash2, Send, Download, Search, X, ArrowUpDown } from 'lucide-preact'
import { Card, PageHead, Segmented, Empty, Chip, BandChip, Rssi, Spark, KV } from '../components/ui'
import { deviceList, actions, events } from '../lib/store'
import { fmtAgo, fmtTime, fmtMHz, fmtDuration, toCSV, download, deviceKey } from '../lib/format'
import { route, go } from '../lib/router'

const MAX_ROWS = 300   // longer lists: filter or search (review 07 M7)

// module level: an inline component would be re-mounted on every render (review 07 M4)
function Th({ k, sort, setSort, children, right }) {
  const active = sort[0] === k
  return (
    <th class={right ? 'text-right' : ''} aria-sort={active ? (sort[1] > 0 ? 'ascending' : 'descending') : undefined}>
      <button type="button" class="inline-flex items-center gap-1 uppercase tracking-[.12em] hover:text-fg" onClick={() => setSort([k, active ? -sort[1] : -1])}>
        {children}{active && <ArrowUpDown class="w-3 h-3" aria-hidden="true" />}
      </button>
    </th>
  )
}

const median = (a) => { if (!a?.length) return null; const s = [...a].sort((x, y) => x - y); return s[Math.floor(s.length / 2)] }

export function Devices() {
  const [filter, setFilter] = useState('decoded')
  const [band, setBand] = useState('all')
  const [q, setQ] = useState('')
  const [sort, setSort] = useState(['lastSeen', -1])
  const sel = route.value.page === 'devices' ? route.value.arg : null
  const list = deviceList.value

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase()
    const r = list.filter(d =>
      (filter === 'all' || (filter === 'decoded' ? d.decoded : filter === 'saved' ? d.persisted : filter === 'mqtt' ? d.mqttEnabled : !d.decoded)) &&
      (band === 'all' || d.band === band) &&
      (!needle || d.key.toLowerCase().includes(needle) || (d.model || '').toLowerCase().includes(needle)))
    const [k, dir] = sort
    return r.sort((a, b) => {
      const x = a[k] ?? (typeof b[k] === 'string' ? '' : -1e15), y = b[k] ?? (typeof a[k] === 'string' ? '' : -1e15)
      return (x > y ? 1 : x < y ? -1 : 0) * dir
    })
  }, [list, filter, band, q, sort])

  const counts = { decoded: list.filter(d => d.decoded).length, raw: list.filter(d => !d.decoded).length, saved: list.filter(d => d.persisted).length, mqtt: list.filter(d => d.mqttEnabled).length }
  const ts = { sort, setSort }
  const selected = sel ? list.find(d => d.key === sel) : null

  const exportCsv = () => download('linkra-geraete.csv', toCSV(rows, [
    ['key', d => d.key], ['modell', d => d.model], ['id', d => d.devId], ['kanal', d => d.channel], ['band', d => d.band],
    ['frequenz', d => d.freq], ['profil', d => d.profile], ['rssi', d => d.rssi], ['anzahl', d => d.count],
    ['zuletzt', d => d.lastSeen ? new Date(d.lastSeen).toISOString() : ''], ['gespeichert', d => d.persisted], ['mqtt', d => d.mqttEnabled],
    ['werte', d => Object.fromEntries((d.meas || []).map(m => [m.k, m.value]))]]), 'text/csv')

  return (
    <>
      <PageHead title="Geräte" sub="Alle erkannten Sender. Gespeicherte Geräte überleben Neustarts und können per MQTT an Home Assistant gesendet werden.">
        <button class="btn" onClick={exportCsv}><Download class="w-4 h-4" />CSV</button>
        <button class="btn-danger" onClick={() => confirm('Alle gespeicherten Geräte aus der Datenbank löschen? Die MQTT-Freigaben gehen verloren, die Geräte verschwinden aus Home Assistant.') && actions.clearDb()}><Trash2 class="w-4 h-4" />Datenbank leeren</button>
      </PageHead>

      <div class={`grid gap-3 ${selected ? 'xl:grid-cols-[1fr,420px]' : ''}`}>
        <Card body="p-0">
          <div class="flex flex-wrap items-center gap-2 px-3 py-2.5 border-b border-line">
            <Segmented label="Anzeigen" value={filter} onChange={setFilter} options={[['decoded', `Decodiert ${counts.decoded}`], ['saved', `Gespeichert ${counts.saved}`], ['mqtt', `MQTT ${counts.mqtt}`], ['raw', `Unbekannt ${counts.raw}`], ['all', 'Alle']]} />
            <Segmented label="Band" value={band} onChange={setBand} options={[['all', 'Alle'], ['433', '433'], ['868', '868']]} />
            <div class="relative ml-auto">
              <Search class="w-4 h-4 absolute left-2 top-2.5 text-muted" aria-hidden="true" />
              <input class="input pl-8 w-48" type="search" aria-label="Geräte durchsuchen" placeholder="Suche" value={q} onInput={e => setQ(e.target.value)} />
            </div>
          </div>
          <div class="overflow-auto max-h-[calc(100vh-230px)]">
            {rows.length === 0 ? <Empty icon={Boxes} title="Keine Geräte in dieser Ansicht" /> : (
              <table class="tbl">
                <thead><tr>
                  <Th k="model" {...ts}>Gerät</Th><Th k="band" {...ts}>Band</Th><th>Werte</th><Th k="rssi" right {...ts}>Pegel</Th>
                  <Th k="count" right {...ts}>Empfangen</Th><Th k="lastSeen" {...ts}>Zuletzt</Th><th class="text-right">Status</th>
                </tr></thead>
                <tbody>
                  {rows.slice(0, MAX_ROWS).map(d => (
                    <tr key={d.key} class={`cursor-pointer ${sel === d.key ? '[&>td]:!bg-accent/10' : ''} ${d.decoded ? '' : 'text-muted'}`}
                      onClick={(e) => { if (e.target.closest('a,button')) return; go('devices', d.key) }}>
                      <td>
                        <a href={`#devices/${encodeURIComponent(d.key)}`} aria-current={sel === d.key ? 'true' : undefined} class="font-medium text-fg hover:text-accent">{d.model}</a>
                        <div class="text-xs text-muted font-mono">{d.devId != null ? `ID ${d.devId}` : ''}{d.channel != null ? ` · CH ${d.channel}` : ''}</div>
                      </td>
                      <td><div class="flex flex-col items-start gap-0.5"><BandChip band={d.band} /><span class="text-[11px] text-muted font-mono">{d.profile || ''}</span></div></td>
                      <td class="text-xs">{(d.meas || []).slice(0, 4).map(m => <span key={m.k} class={`mr-2 ${m.warn ? 'text-bad font-semibold' : ''}`}>{m.value}</span>)}</td>
                      <td class="text-right"><Rssi v={d.rssi} /></td>
                      <td class="text-right num">{d.count}</td>
                      <td class="text-xs">{d.lastSeen ? fmtAgo(d.lastSeen) : <span class="text-muted">vor Neustart</span>}</td>
                      <td class="text-right">
                        <div class="inline-flex gap-1">
                          {d.persisted && <Chip tone="ok">DB</Chip>}
                          {d.mqttEnabled && <Chip tone="accent">MQTT</Chip>}
                          {!d.decoded && <Chip tone="warn">RAW</Chip>}
                        </div>
                      </td>
                    </tr>
                  ))}
                </tbody>
              </table>
            )}
            {rows.length > MAX_ROWS && <div class="px-3 py-2.5 text-[12px] text-muted border-t border-line">{MAX_ROWS} von {rows.length} angezeigt – Filter oder Suche eingrenzen.</div>}
          </div>
        </Card>
        {selected && <DeviceDetail d={selected} />}
      </div>
    </>
  )
}

function DeviceDetail({ d }) {
  const tl = d.history || []
  const numericKeys = [...new Set(tl.flatMap(h => Object.keys(h.v || {})))].filter(k => k !== 'battery_ok')
  const iv = median(d.intervals)
  const evs = events.value.filter(e => deviceKey(e) === d.key).slice(0, 15)
  return (
    <Card title={<span class="truncate">{d.model}</span>} right={<button class="btn btn-sm" aria-label="Details schließen" onClick={() => go('devices')}><X class="w-4 h-4" /></button>}>
      <div class="flex flex-wrap gap-1.5 mb-3">
        <BandChip band={d.band} />
        {d.decoded ? <Chip tone="ok">decodiert</Chip> : <Chip tone="warn">nur klassifiziert</Chip>}
        {d.persisted && <Chip tone="ok">gespeichert</Chip>}
        {d.mqttEnabled && <Chip tone="accent">per MQTT gesendet</Chip>}
      </div>

      {d.meas?.length > 0 && (
        <div class="grid grid-cols-2 gap-2 mb-4">
          {d.meas.map(m => (
            <div key={m.k} class="rounded-lg bg-raised px-3 py-2">
              <div class="label">{m.label}</div>
              <div class={`num text-lg font-semibold ${m.warn ? 'text-bad' : ''}`}>{m.value}</div>
              {numericKeys.includes(m.k) && <div class="text-accent"><Spark values={tl.map(h => h.v?.[m.k])} w={160} /></div>}
            </div>
          ))}
        </div>
      )}

      <KV rows={[
        ['Schlüssel', <span class="font-mono text-xs">{d.key}</span>],
        ['Frequenz', fmtMHz(d.freq)],
        ['Profil (zuletzt)', d.profile],
        Object.keys(d.profiles || {}).length > 0 && ['Treffer je Profil', Object.entries(d.profiles).map(([k, v]) => `${k} ${v}`).join(', ')],
        ['Modulation', d.mod],
        ['RSSI aktuell', <Rssi v={d.rssi} />],
        d.rssiMin != null && ['RSSI min / max', `${d.rssiMin} / ${d.rssiMax} dBm`],
        ['Empfangen gesamt', d.count],
        ['in dieser Sitzung', d.sessionCount || 0],
        ['Sendeintervall (Median)', iv ? fmtDuration(iv / 1000) : '—'],
        ['Zuerst gesehen', d.firstSeen ? fmtTime(d.firstSeen) : '—'],
        ['Zuletzt gesehen', d.lastSeen ? `${fmtTime(d.lastSeen)} (${fmtAgo(d.lastSeen)})` : 'vor letztem Neustart'],
      ]} />

      {tl.length > 1 && (
        <div class="mt-4">
          <div class="label mb-1">RSSI-Verlauf</div>
          <div class="text-b868"><Spark values={tl.map(h => h.rssi)} w={380} h={40} /></div>
        </div>
      )}

      {d.decoded && (
        <div class="mt-4 flex flex-wrap gap-2">
          {d.persisted
            ? <button class="btn-danger" onClick={() => actions.unpersist(d)}><Trash2 class="w-4 h-4" />Aus DB entfernen</button>
            : <button class="btn-primary" onClick={() => actions.persist(d)}><Save class="w-4 h-4" />Speichern</button>}
          <button class="btn" disabled={!d.persisted} title={d.persisted ? '' : 'Erst speichern'} onClick={() => actions.toggleMqtt(d)}>
            <Send class="w-4 h-4" />{d.mqttEnabled ? 'Nicht mehr per MQTT senden' : 'Per MQTT senden'}
          </button>
        </div>
      )}

      {d.last && (
        <details class="mt-4">
          <summary class="label cursor-pointer">Letztes Telegramm (JSON)</summary>
          <pre class="mt-2 text-xs font-mono bg-raised rounded-lg p-2 overflow-auto max-h-60">{JSON.stringify(d.last, null, 2)}</pre>
        </details>
      )}
      {evs.length > 0 && (
        <div class="mt-4">
          <div class="label mb-1">Letzte Empfänge</div>
          <ul class="text-xs font-mono space-y-0.5">
            {evs.map(e => <li key={e.seq} class="flex justify-between"><span>{fmtTime(e.t)} · {e.profile}</span><Rssi v={e.rssi} /></li>)}
          </ul>
        </div>
      )}
    </Card>
  )
}
