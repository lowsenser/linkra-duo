import { Radio } from 'lucide-preact'
import { Card, Chip, Dot, LevelMeter, Progress, KV } from './ui'
import { fmtMHz } from '../lib/format'

const SENS = ['Ultra', 'Sensitiv', 'Normal', 'Strikt']

/** Live state of one CC1101: active profile, countdown to next switch, levels and counters */
export function RadioCard({ m, profiles, pinned, compact }) {
  if (!m) return <Card title="—" body="p-4"><div class="h-24 animate-pulse bg-raised rounded-lg" /></Card>
  const p = profiles?.find(x => x.id === m.profile)
  const left = m.profileDwellMs != null ? Math.max(0, m.profileDwellMs - m.profileElapsedMs) : null
  const isPinned = pinned && pinned === m.profile
  const tone = m.band === '433' ? 'bg-b433' : 'bg-b868'
  return (
    <Card
      title={<span class="flex items-center gap-2"><span class={m.band === '433' ? 'text-b433' : 'text-b868'}>CC1101 · {m.band} MHz</span></span>}
      icon={Radio}
      right={<>
        {m.receiving && <Chip tone="accent"><Dot tone="ok" live /> empfängt</Chip>}
        <Chip tone={m.ok ? 'ok' : 'bad'}>{m.ok ? 'OK' : 'FEHLER'}</Chip>
      </>}
    >
      <div class="flex items-start justify-between gap-3">
        <div class="min-w-0">
          <div class="label">Aktives Profil</div>
          <div class="mt-0.5 flex items-center gap-2 flex-wrap">
            <span class="font-mono text-[15px] font-semibold">{m.profile || '—'}</span>
            {m.modulation && <Chip tone={m.modulation === 'OOK' ? '433' : m.modulation === 'wM-Bus' ? 'accent' : '868'}>{m.modulation}</Chip>}
            {p?.home && <Chip>Home</Chip>}
            {isPinned && <Chip tone="warn">fixiert</Chip>}
          </div>
          <div class="text-xs text-muted mt-1 num">
            {fmtMHz(m.freqMHz, 2)} · {m.bitrateKbps?.toFixed(2)} {m.modulation === 'wM-Bus' ? 'kchip/s · Paketmodus' : 'kbit/s'} · BW {m.bwKHz} kHz{m.modulation === 'FSK' && m.devKHz ? ` · Hub ±${m.devKHz} kHz` : ''}
          </div>
        </div>
        <div class="text-right shrink-0">
          <div class="label">Wechsel in</div>
          <div class="num text-[15px] font-semibold">{left != null ? `${(left / 1000).toFixed(0)} s` : '—'}</div>
        </div>
      </div>
      {left != null && <div class="mt-2"><Progress value={m.profileElapsedMs} max={m.profileDwellMs} tone={tone} /></div>}

      <div class="mt-4">
        <div class="flex justify-between text-xs mb-1">
          <span class="text-muted">Pegel</span>
          <span class="num"><b>{m.rssi > -128 ? m.rssi : '—'}</b> dBm · Rauschen {m.noiseFloor > -128 ? m.noiseFloor : '—'} · Schwelle {m.threshold}</span>
        </div>
        <LevelMeter rssi={m.rssi} floor={m.noiseFloor} threshold={m.threshold} color={tone} />
      </div>

      {!compact && (
        <div class="mt-4 grid grid-cols-4 gap-2 text-center">
          {[['Roh', m.rawSignals], ['Verworfen', m.ignored], ['Decodiert', m.decoded, 'text-ok'], ['Quote', m.rawSignals ? `${((m.decoded / m.rawSignals) * 100).toFixed(1)}%` : '—']].map(([l, v, c]) => (
            <div key={l} class="rounded-lg bg-raised py-2">
              <div class={`num font-semibold ${c || ''}`}>{v ?? '—'}</div>
              <div class="text-[10px] uppercase tracking-wider text-muted">{l}</div>
            </div>
          ))}
        </div>
      )}
      {!compact && (
        <div class="mt-3">
          <KV rows={[
            ['Empfindlichkeit', SENS[m.sensitivity] ?? m.sensitivity],
            m.regErrors != null && ['Register fremd verändert', m.regErrors ? `${m.regErrors}× – zuletzt ${m.regErrorLast}` : 'nie', m.regErrors ? 'text-bad' : ''],
            m.forcedEnds != null && ['Empfang zwangsbeendet (>800 ms)', m.forcedEnds.toLocaleString('de-DE')],
            m.queueDrops != null && ['Decoder-Überlauf (verworfen)', m.queueDrops.toLocaleString('de-DE'), m.queueDrops ? 'text-warn' : ''],
            m.isrEdges != null && ['GDO0-Flanken', m.isrEdges.toLocaleString('de-DE')],
            m.wmbus && ['wM-Bus Sync / OK / Fehler', `${m.wmbus.syncs} / ${m.wmbus.ok} / ${m.wmbus.bad}${m.wmbus.overflow ? ` (${m.wmbus.overflow} Überlauf)` : ''}`],
            m.wmbus?.lastError && ['wM-Bus letzter Fehler', m.wmbus.lastError],
          ]} />
        </div>
      )}
    </Card>
  )
}
