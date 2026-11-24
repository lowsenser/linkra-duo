import { useId } from 'preact/hooks'
import { liveFrame, live, liveTick, rf } from '../lib/store'
import { fmtMHz } from '../lib/format'

// Live level of one receiver at 5 Hz: big peak value, noise floor, SNR bar,
// active profile and a 60 s trace of the peaks.
const MIN = -115, MAX = -35
const pct = (v) => Math.max(0, Math.min(100, ((v - MIN) / (MAX - MIN)) * 100))

function Trace({ band, color }) {
  liveTick.value                                      // re-render with the frame stream
  const gid = `g${band}-${useId()}`.replace(/[^a-zA-Z0-9-]/g, '')
  const P = live['p' + band], N = live['n' + band]
  const n = Math.min(P.length, 300), off = P.length - n
  const W = 300, H = 44
  const y = (v) => H - (pct(v) / 100) * H
  let path = '', started = false
  for (let i = 0; i < n; i++) {
    const v = P[off + i]
    if (v == null) { started = false; continue }
    // newest value at the right edge even with fewer than 300 samples (review 07 N15)
    path += `${started ? 'L' : 'M'}${(W - (n - 1 - i) * (W / 299)).toFixed(1)},${y(v).toFixed(1)}`
    started = true
  }
  const noise = N.length ? N[N.length - 1] : null
  return (
    <svg viewBox={`0 0 ${W} ${H}`} preserveAspectRatio="none" class="w-full h-11" aria-hidden="true">
      <defs>
        <linearGradient id={gid} x1="0" x2="0" y1="0" y2="1">
          <stop offset="0" stop-color={color} stop-opacity=".35" /><stop offset="1" stop-color={color} stop-opacity="0" />
        </linearGradient>
      </defs>
      {noise != null && <line x1="0" x2={W} y1={y(noise)} y2={y(noise)} stroke="rgb(var(--muted))" stroke-opacity=".5" stroke-dasharray="3 3" vector-effect="non-scaling-stroke" />}
      {path && <path d={`${path}L${W},${H}L${(W - (n - 1) * (W / 299)).toFixed(1)},${H}Z`} fill={`url(#${gid})`} />}
      {path && <path d={path} fill="none" stroke={color} stroke-width="1.5" vector-effect="non-scaling-stroke" />}
    </svg>
  )
}

export function BandLive({ band }) {
  const f = liveFrame.value
  const m = rf.value?.modules.find(x => x.band === band)
  const color = band === '433' ? 'rgb(var(--b433))' : 'rgb(var(--b868))'
  const wm = band === '868' && f?.wm
  const peak = f ? f['p' + band] : null, noise = f ? f['n' + band] : m?.noiseFloor
  const thr = f ? f['th' + band] : m?.threshold
  const rx = f ? !!(f.rx & (band === '433' ? 1 : 2)) : m?.receiving
  const snr = peak != null && noise != null ? peak - noise : null
  return (
    <div class="card p-4 overflow-hidden">
      <div class="flex items-center justify-between gap-2">
        <div class="flex items-center gap-2">
          <span class="text-[13px] font-bold tracking-[.12em] whitespace-nowrap" style={{ color }}>{band}<span class="text-[10px] font-medium text-muted ml-1">MHz</span></span>
          <span class={`inline-block w-2 h-2 rounded-full ${rx ? 'rx-on' : ''}`} style={{ background: rx ? color : 'rgb(var(--line))', color }} title={rx ? 'empfängt gerade' : 'wartet'} />
        </div>
        <div class="text-right min-w-0">
          <div class="font-mono text-[12px] truncate">{m?.profile || '—'}</div>
          <div class="text-[11px] text-muted num">{m ? `${fmtMHz(m.freqMHz)} · ${m.modulation}` : '…'}</div>
        </div>
      </div>
      <div class="mt-2 flex items-end gap-3">
        <div class={`font-mono num leading-none font-semibold tracking-tight glow-text ${wm ? 'text-[26px] py-1.5' : 'text-[40px]'}`} style={{ color }}>
          {wm ? 'wM-Bus' : peak != null ? peak : '—'}
        </div>
        <div class="pb-1 text-[12px] text-muted num leading-tight">
          {wm ? <>Paketmodus<br />Zähler-Telegramme</> : <>dBm Spitze<br />Rauschen {noise ?? '—'} · SNR {snr ?? '—'} dB</>}
        </div>
      </div>
      <div class="relative mt-3 h-1.5 rounded-full bg-raised overflow-hidden">
        {noise != null && <div class="absolute inset-y-0 left-0 bg-muted/30" style={{ width: `${pct(noise)}%` }} />}
        {peak != null && !wm && <div class="absolute inset-y-0 left-0 transition-[width] duration-150" style={{ width: `${pct(peak)}%`, background: color, boxShadow: `0 0 12px ${color}` }} />}
        {thr != null && <div class="absolute -inset-y-0.5 w-0.5 bg-warn" style={{ left: `${pct(thr)}%` }} title={`Schwelle ${thr} dBm`} />}
      </div>
      <div class="mt-2 -mx-1"><Trace band={band} color={color} /></div>
      <div class="flex justify-between text-[11px] text-muted num">
        <span>{m ? `${m.decoded} decodiert` : ''}</span><span>letzte 60 s</span>
      </div>
    </div>
  )
}
