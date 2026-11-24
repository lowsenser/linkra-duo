import { liveFrame, live, rf } from '../lib/store'

// Vertical LED-segment level meter (5 Hz): lit up to the current peak,
// peak-hold over the last 2 s, noise floor and receive threshold marked.
const MIN = -115, MAX = -35, SEG = 32
const frac = (v) => Math.max(0, Math.min(1, (v - MIN) / (MAX - MIN)))

export function VuMeter({ band, height = 300 }) {
  const f = liveFrame.value
  const m = rf.value?.modules.find(x => x.band === band)
  const color = `rgb(var(--b${band}))`
  const wm = band === '868' && f?.wm
  const peak = f && !wm ? f['p' + band] : null
  const P = live['p' + band]
  let hold = null
  for (let i = Math.max(0, P.length - 10); i < P.length; i++) if (P[i] != null && (hold == null || P[i] > hold)) hold = P[i]
  const noise = f ? f['n' + band] : m?.noiseFloor, thr = f ? f['th' + band] : m?.threshold
  const rx = f && (f.rx & (band === '433' ? 1 : 2))
  const lit = peak != null ? Math.round(frac(peak) * SEG) : 0
  const holdSeg = hold != null && !wm ? Math.min(SEG - 1, Math.round(frac(hold) * SEG) - 1) : -1
  const pos = (v) => `${frac(v) * 100}%`
  return (
    <div class="flex flex-col items-center gap-2 w-[64px]">
      <div class="mono text-[11px] font-bold tracking-widest" style={{ color }}>{band}</div>
      <div class="relative w-7" style={{ height }}>
        <div class="absolute inset-0 flex flex-col-reverse gap-[2px]">
          {Array.from({ length: SEG }, (_, i) => (
            <div key={i} class="flex-1 rounded-[1px] transition-opacity duration-100"
              style={{ background: color, opacity: i < lit ? 0.95 : i === holdSeg ? 0.8 : 0.09, boxShadow: i < lit ? `0 0 6px ${color}` : 'none' }} />
          ))}
        </div>
        {noise != null && <div class="absolute -left-1.5 -right-1.5 h-px bg-muted/80" style={{ bottom: pos(noise) }} title={`Rauschen ${noise} dBm`} />}
        {thr != null && <div class="absolute -left-2 w-1.5 h-0.5 bg-warn" style={{ bottom: pos(thr) }} title={`Schwelle ${thr} dBm`} />}
      </div>
      <div class="mono num text-[15px] font-semibold leading-none" style={{ color }}>{wm ? 'Paket' : peak ?? '—'}</div>
      <div class={`w-2 h-2 rounded-full ${rx ? 'rx-on' : ''}`} style={{ background: rx ? color : 'rgb(var(--line))', color }} title={rx ? 'empfängt' : 'wartet'} />
    </div>
  )
}
