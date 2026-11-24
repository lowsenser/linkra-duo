import { useMemo, useState } from 'preact/hooks'
import { Pause, Play, Trash2, Download, Search, Radio } from 'lucide-preact'
import { Card, PageHead, Segmented, Empty, Chip } from '../components/ui'
import { EventTable } from '../components/EventTable'
import { events, paused, counters, clearSession, rf } from '../lib/store'
import { toCSV, download, deviceKey } from '../lib/format'
import { go } from '../lib/router'

export function Live() {
  const [band, setBand] = useState('all')
  const [kind, setKind] = useState('all')
  const [profile, setProfile] = useState('all')
  const [q, setQ] = useState('')
  const all = events.value

  const rows = useMemo(() => {
    const needle = q.trim().toLowerCase()
    return all.filter(e =>
      (band === 'all' || e.band === band) &&
      (kind === 'all' || (kind === 'dec' ? e.decoded : !e.decoded)) &&
      (profile === 'all' || e.profile === profile) &&
      (!needle || JSON.stringify(e.raw).toLowerCase().includes(needle)))
  }, [all, band, kind, profile, q])

  // Telegram rate over the last 60 s
  const now = Date.now()
  const last60 = all.filter(e => now - e.t < 60000)
  const rate = { all: last60.length, dec: last60.filter(e => e.decoded).length }
  const c = counters.value

  const exportRows = (fmt) => {
    const stamp = new Date().toISOString().replace(/[:.]/g, '-')
    if (fmt === 'json') return download(`linkra-telegramme-${stamp}.json`, JSON.stringify(rows.map(e => ({ t: new Date(e.t).toISOString(), ...e.raw })), null, 2), 'application/json')
    download(`linkra-telegramme-${stamp}.csv`, toCSV(rows, [
      ['zeit', e => new Date(e.t).toISOString()], ['band', e => e.band], ['frequenz_mhz', e => e.freq], ['profil', e => e.profile],
      ['modulation', e => e.mod], ['decodiert', e => e.decoded], ['modell', e => e.model], ['id', e => e.devId], ['kanal', e => e.channel],
      ['rssi', e => e.rssi], ['json', e => e.raw]]), 'text/csv')
  }

  return (
    <>
      <PageHead title="Live-Telegramme" sub="Jedes empfangene Funktelegramm, decodiert (DEC) oder nur als Pulsmuster klassifiziert (RAW)">
        <button class={paused.value ? 'btn-primary' : 'btn'} onClick={() => { paused.value = !paused.value }}>
          {paused.value ? <><Play class="w-4 h-4" />Fortsetzen</> : <><Pause class="w-4 h-4" />Pause</>}
        </button>
        <button class="btn" onClick={() => exportRows('csv')}><Download class="w-4 h-4" />CSV</button>
        <button class="btn" onClick={() => exportRows('json')}><Download class="w-4 h-4" />JSON</button>
        <button class="btn-danger" onClick={() => confirm('Sitzungsdaten (Telegramme, nicht gespeicherte Geräte) im Browser leeren?') && clearSession()}><Trash2 class="w-4 h-4" />Leeren</button>
      </PageHead>

      <div class="grid grid-cols-2 md:grid-cols-4 gap-3 mb-3">
        {[['Telegramme / min', rate.all], ['Decodiert / min', rate.dec, 'text-ok'], ['Sitzung decodiert', c.decoded, 'text-ok'], ['Sitzung roh', c.classified, 'text-warn']].map(([l, v, cls]) => (
          <div key={l} class="card px-4 py-3"><div class="label">{l}</div><div class={`num text-xl font-semibold ${cls || ''}`}>{v}</div></div>
        ))}
      </div>

      <Card body="p-0">
        <div class="flex flex-wrap items-center gap-2 px-3 py-2.5 border-b border-line">
          <Segmented label="Band" value={band} onChange={setBand} options={[['all', 'Alle Bänder'], ['433', '433'], ['868', '868']]} />
          <Segmented label="Art" value={kind} onChange={setKind} options={[['all', 'Alle'], ['dec', 'Decodiert'], ['raw', 'Roh']]} />
          <select class="input" aria-label="Profil" value={profile} onChange={e => setProfile(e.target.value)}>
            <option value="all">Alle Profile</option>
            {(rf.value?.profiles || []).map(p => <option key={p.id} value={p.id}>{p.id}</option>)}
          </select>
          <div class="relative ml-auto">
            <Search class="w-4 h-4 absolute left-2 top-2.5 text-muted" aria-hidden="true" />
            <input class="input pl-8 w-56" type="search" aria-label="Telegramme durchsuchen" placeholder="Suche (Modell, ID, Wert …)" value={q} onInput={e => setQ(e.target.value)} />
          </div>
          {paused.value && <Chip tone="warn">pausiert</Chip>}
          <span class="text-xs text-muted num">{rows.length} / {all.length}</span>
        </div>
        <div class="max-h-[calc(100vh-300px)] overflow-auto">
          {rows.length === 0
            ? <Empty icon={Radio} title="Keine Telegramme">Filter anpassen oder auf Empfang warten</Empty>
            : <EventTable rows={rows.slice(0, 400)} onDevice={(e) => go('devices', deviceKey(e))} />}
        </div>
      </Card>
    </>
  )
}
