import { useState } from 'preact/hooks'
import { ChevronRight } from 'lucide-preact'
import { BandChip, Chip, Rssi } from './ui'
import { fmtTime, fmtMHz, measurements } from '../lib/format'

/** Telegram table. Rows expand into the full decoder JSON. */
export function EventTable({ rows, compact, onDevice }) {
  const [open, setOpen] = useState(null)
  return (
    <div class="overflow-auto">
      <table class="tbl">
        <thead><tr>
          <th><span class="sr-only">Details</span></th><th>Zeit</th><th>Band</th>{!compact && <th>Frequenz</th>}<th>Profil</th><th>Gerät / Klasse</th>
          <th>Werte</th><th class="text-right">RSSI</th>
        </tr></thead>
        <tbody>
          {rows.map((e) => {
            const meas = e.meas ?? (e.decoded ? measurements(e.raw) : [])
            const isOpen = open === e.seq
            return [
              <tr key={e.seq} class={`cursor-pointer ${e.decoded ? '' : 'text-muted'}`} onClick={(ev) => { if (ev.target.closest('button')) return; setOpen(isOpen ? null : e.seq) }}>
                <td class="!pr-0 w-6">
                  <button type="button" class="icon-btn w-6 h-6" aria-expanded={isOpen} aria-controls={`ev-${e.seq}`}
                    aria-label={`Telegramm ${e.model} ${fmtTime(e.t)} ${isOpen ? 'zuklappen' : 'aufklappen'}`} onClick={() => setOpen(isOpen ? null : e.seq)}>
                    <ChevronRight class={`w-3.5 h-3.5 transition-transform ${isOpen ? 'rotate-90' : ''}`} />
                  </button>
                </td>
                <td class="font-mono text-xs num">{fmtTime(e.t)}</td>
                <td><BandChip band={e.band} /></td>
                {!compact && <td class="num text-xs">{fmtMHz(e.freq)}</td>}
                <td class="font-mono text-xs">{e.profile || '—'}{e.mod && <span class="text-muted"> · {e.mod}</span>}</td>
                <td>
                  <div class="flex items-center gap-1.5">
                    {e.decoded ? <Chip tone="ok" title="Decodiert">DEC</Chip>
                      : e.unconfirmed ? <Chip tone="warn" title="Neues Gerät, erst einmal empfangen. Es wird beim zweiten Empfang innerhalb von 15 min bestätigt – einmalige Treffer mit zufälliger ID sind meist Rauschen.">NEU?</Chip>
                      : <Chip tone="warn" title="Nicht decodiert – nur Pulsmuster-Schätzung">RAW</Chip>}
                    <span class={e.decoded ? 'font-medium text-fg' : 'italic'}>{e.model}</span>
                    {e.devId != null && <span class="text-xs text-muted font-mono">#{e.devId}</span>}
                    {e.channel != null && <span class="text-xs text-muted">CH{e.channel}</span>}
                  </div>
                </td>
                <td class="text-xs">
                  {e.decoded
                    ? meas.slice(0, compact ? 3 : 5).map(m => <span key={m.k} class={`mr-2 ${m.warn ? 'text-bad' : ''}`}><span class="text-muted">{m.label}</span> {m.value}</span>)
                    : <span class="font-mono">{e.raw.fingerprint || ''}{e.raw.duration ? ` · ${(e.raw.duration / 1000).toFixed(1)} ms` : ''}</span>}
                </td>
                <td class="text-right"><Rssi v={e.rssi} /></td>
              </tr>,
              isOpen && (
                <tr key={e.seq + 'x'} id={`ev-${e.seq}`}>
                  <td colSpan={compact ? 7 : 8} class="!bg-raised/50 !whitespace-normal">
                    <div class="flex items-start gap-3 py-1">
                      <pre class="flex-1 text-xs font-mono overflow-auto max-h-60">{JSON.stringify(e.raw, null, 2)}</pre>
                      <div class="flex flex-col gap-1.5">
                        <button class="btn btn-sm" onClick={() => navigator.clipboard?.writeText(JSON.stringify(e.raw))}>JSON kopieren</button>
                        {onDevice && e.decoded && <button class="btn btn-sm" onClick={() => onDevice(e)}>Zum Gerät</button>}
                      </div>
                    </div>
                  </td>
                </tr>
              ),
            ]
          })}
        </tbody>
      </table>
    </div>
  )
}
