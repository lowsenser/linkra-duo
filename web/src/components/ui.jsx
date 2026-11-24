import { rssiClass } from '../lib/format'

export const Card = ({ title, icon: Icon, right, children, class: cls = '', body = 'p-4' }) => (
  <section class={`card ${cls}`}>
    {(title || right) && (
      <header class="card-h">
        <h2 class="card-t">{Icon && <Icon class="w-4 h-4 text-muted" />}{title}</h2>
        <div class="flex items-center gap-2">{right}</div>
      </header>
    )}
    <div class={body}>{children}</div>
  </section>
)

export const KV = ({ rows }) => (
  <dl class="grid grid-cols-[auto,1fr] gap-x-4 gap-y-1.5 text-[13px]">
    {rows.filter(Boolean).map(([k, v, cls]) => [
      <dt key={k + 'k'} class="text-muted">{k}</dt>,
      <dd key={k + 'v'} class={`num text-right break-words min-w-0 ${cls || ''}`}>{v ?? '—'}</dd>,
    ])}
  </dl>
)

const TONES = {
  ok: 'bg-ok/15 text-ok', warn: 'bg-warn/15 text-warn', bad: 'bg-bad/15 text-bad',
  accent: 'bg-accent/15 text-accent', muted: 'bg-raised text-muted', '433': 'bg-b433/15 text-b433', '868': 'bg-b868/15 text-b868',
}
export const Chip = ({ tone = 'muted', children, title }) => <span title={title} class={`chip ${TONES[tone] || TONES.muted}`}>{children}</span>

export const BandChip = ({ band }) => band ? <Chip tone={band}>{band}</Chip> : <Chip>?</Chip>

export const Dot = ({ tone = 'ok', live }) => (
  <span aria-hidden="true" class={`relative inline-block w-2 h-2 rounded-full ${live ? 'live-dot' : ''} ${{ ok: 'bg-ok', warn: 'bg-warn', bad: 'bg-bad', muted: 'bg-muted' }[tone]}`} />
)

export const Rssi = ({ v }) => <span class={`num font-mono text-xs ${rssiClass(v)}`}>{v != null ? `${v} dBm` : '—'}</span>

/** Switch; label (or labelledby) gives it an accessible name (review 07 N7) */
export const Toggle = ({ checked, onChange, disabled, label, labelledby }) => (
  <button type="button" role="switch" aria-checked={checked} aria-label={label} aria-labelledby={labelledby} disabled={disabled} onClick={() => onChange(!checked)}
    class={`relative w-9 h-5 rounded-full transition-colors ${checked ? 'bg-accent' : 'bg-line'} disabled:opacity-40`}>
    <span class={`absolute top-0.5 left-0.5 w-4 h-4 bg-white rounded-full shadow transition-transform ${checked ? 'translate-x-4' : ''}`} />
  </button>
)

export const Segmented = ({ value, options, onChange, label }) => (
  <div class="inline-flex p-0.5 rounded-lg bg-raised border border-line" role="group" aria-label={label}>
    {options.map(([v, l]) => (
      <button key={v} type="button" aria-pressed={value === v} onClick={() => onChange(v)}
        class={`px-2.5 h-7 rounded-md text-xs font-medium transition-colors ${value === v ? 'bg-panel text-fg shadow-sm' : 'text-muted hover:text-fg'}`}>{l}</button>
    ))}
  </div>
)

/** Horizontal level meter for dBm: noise floor, threshold and live value on one scale */
export const LevelMeter = ({ rssi, floor, threshold, min = -120, max = -30, color = 'bg-accent' }) => {
  const pct = (v) => `${Math.max(0, Math.min(100, ((v - min) / (max - min)) * 100))}%`
  const live = rssi != null && rssi > -128
  return (
    <div class="relative h-2.5 rounded-full bg-raised overflow-hidden" title={`RSSI ${rssi} / Rauschen ${floor} / Schwelle ${threshold} dBm`}>
      {floor != null && floor > -128 && <div class="absolute inset-y-0 left-0 bg-muted/25" style={{ width: pct(floor) }} />}
      {live && <div class={`absolute inset-y-0 left-0 ${color} opacity-80 transition-[width] duration-300`} style={{ width: pct(rssi) }} />}
      {threshold != null && <div class="absolute inset-y-[-2px] w-0.5 bg-warn" style={{ left: pct(threshold) }} />}
    </div>
  )
}

export const Progress = ({ value, max, tone = 'bg-accent' }) => (
  <div class="h-1.5 rounded-full bg-raised overflow-hidden">
    <div class={`h-full ${tone} transition-[width] duration-500`} style={{ width: `${max ? Math.min(100, (value / max) * 100) : 0}%` }} />
  </div>
)

export const Empty = ({ icon: Icon, title, children }) => (
  <div class="py-10 text-center text-muted">
    {Icon && <Icon class="w-8 h-8 mx-auto mb-2 opacity-50" />}
    <div class="font-medium text-fg/80">{title}</div>
    {children && <div class="text-xs mt-1">{children}</div>}
  </div>
)

// The page title is shown in the top bar; PageHead carries the description and page actions.
export const PageHead = ({ sub, children }) => (sub || children) ? (
  <div class="flex flex-wrap items-center justify-between gap-3 mb-4">
    <p class="text-[13px] text-muted max-w-3xl">{sub}</p>
    <div class="flex flex-wrap items-center gap-2">{children}</div>
  </div>
) : null

/** Tiny inline SVG sparkline (no library) */
export const Spark = ({ values, w = 90, h = 22, stroke = 'currentColor' }) => {
  const v = values.filter(x => x != null && !isNaN(x))
  if (v.length < 2) return <svg width={w} height={h} />
  const lo = Math.min(...v), hi = Math.max(...v), r = hi - lo || 1
  const pts = v.map((x, i) => `${(i / (v.length - 1)) * w},${h - 2 - ((x - lo) / r) * (h - 4)}`).join(' ')
  return <svg width={w} height={h} class="overflow-visible"><polyline points={pts} fill="none" stroke={stroke} stroke-width="1.5" stroke-linejoin="round" /></svg>
}
