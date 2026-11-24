import { useEffect, useRef } from 'preact/hooks'
import { live, onLiveFrame, onSignal, rf } from '../lib/store'

// Waterfall of the receive channels: one row per profile frequency, time runs
// right to left, colour = peak level above the noise floor in each 200 ms frame.
// A CC1101 listens on one frequency at a time, so only the row of the active
// profile gets data; the rotation shows up as the signal hopping between rows.
// Decoded telegrams are marked with a dot and the device name.
const PALETTES = {
  ice: [[0, [5, 11, 20]], [0.12, [11, 31, 58]], [0.3, [30, 58, 138]], [0.5, [8, 145, 178]], [0.7, [34, 211, 238]], [0.86, [253, 224, 71]], [1, [255, 255, 255]]],
  phosphor: [[0, [3, 8, 5]], [0.14, [6, 32, 18]], [0.34, [10, 92, 48]], [0.55, [34, 170, 80]], [0.72, [120, 230, 110]], [0.88, [240, 230, 90]], [1, [255, 255, 240]]],
}
const makeLut = (stops) => {
  const out = []
  for (let i = 0; i < 256; i++) {
    const v = i / 255
    let k = 0; while (k < stops.length - 2 && v > stops[k + 1][0]) k++
    const [a, ca] = stops[k], [b, cb] = stops[k + 1]
    const f = (v - a) / (b - a)
    out.push(`rgb(${ca.map((c, j) => Math.round(c + (cb[j] - c) * f)).join(',')})`)
  }
  return out
}
const LUTS = { ice: makeLut(PALETTES.ice), phosphor: makeLut(PALETTES.phosphor) }
const SPAN_DB = 40          // noise floor .. +40 dB maps onto the colour scale
const BAND_COL = { '433': '#fbb040', '868': '#c084fc' }

export function Waterfall({ height = 230, seconds = 120, palette = 'ice' }) {
  const LUT = LUTS[palette] || LUTS.ice
  const wrap = useRef(null), cv = useRef(null)

  useEffect(() => {
    const canvas = cv.current, ctx = canvas.getContext('2d')
    let w = 0, dpr = 1, raf = 0, dirty = true
    const markers = []
    const resize = () => {
      dpr = Math.min(2, window.devicePixelRatio || 1)
      w = wrap.current.clientWidth
      canvas.width = w * dpr; canvas.height = height * dpr
      canvas.style.width = w + 'px'; canvas.style.height = height + 'px'
      dirty = true
    }
    resize()
    const ro = new ResizeObserver(resize); ro.observe(wrap.current)

    const lanesOf = () => {
      const seen = new Map()
      for (const p of rf.value?.profiles || []) {
        if (/^(xtest|tune)/.test(p.id) || p.id === 'ook433-wide') continue
        const key = `${p.band}:${p.freqMHz.toFixed(2)}`
        if (!seen.has(key)) seen.set(key, { band: p.band, f: p.freqMHz, ids: [] })
        seen.get(key).ids.push(p.id)
      }
      return [...seen.values()].sort((a, b) => a.f - b.f)
    }
    const laneIdx = (lanes, band, f) => f == null ? -1 : lanes.findIndex(l => l.band === band && Math.abs(l.f - f) < 0.03)

    const offFrame = onLiveFrame(() => { dirty = true })
    const offSig = onSignal((ev) => {
      if (!ev.decoded) return
      const prof = (rf.value?.profiles || []).find(p => p.id === ev.profile)
      markers.push({ t: ev.t, band: ev.band, f: prof?.freqMHz ?? ev.freq, label: `${ev.model}${ev.devId != null ? ' #' + ev.devId : ''}`, wm: String(ev.model).startsWith('wMBus') })
      if (markers.length > 400) markers.splice(0, markers.length - 400)
      dirty = true
    })

    const draw = () => {
      raf = requestAnimationFrame(draw)
      if (!dirty || document.hidden) return
      dirty = false
      const lanes = lanesOf()
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0)
      ctx.fillStyle = palette === 'phosphor' ? '#020503' : '#03070b'; ctx.fillRect(0, 0, w, height)
      if (!lanes.length) return
      const gutL = 64, gutB = 18, gutT = 6
      const plotW = w - gutL - 8, plotH = height - gutB - gutT
      const laneH = plotH / lanes.length
      const cols = seconds * 5, colW = plotW / cols
      const nowMs = Date.now(), t0 = nowMs - seconds * 1000

      // lane backgrounds (no data = channel not being listened to)
      lanes.forEach((l, i) => {
        ctx.fillStyle = palette === 'phosphor' ? (i % 2 ? '#050a07' : '#040805') : (i % 2 ? '#060c13' : '#050a10')
        ctx.fillRect(gutL, gutT + i * laneH, plotW, laneH - 1)
      })

      // cells
      const T = live.t
      let i = T.length - 1
      while (i > 0 && T[i - 1] >= t0) i--
      for (; i < T.length; i++) {
        const x = gutL + ((T[i] - t0) / 1000) * 5 * colW
        if (x < gutL) continue
        for (const band of ['433', '868']) {
          const li = laneIdx(lanes, band, live['f' + band][i])
          if (li < 0) continue
          const y = gutT + li * laneH
          if (band === '868' && live.wm[i]) {
            ctx.fillStyle = 'rgba(45,212,191,0.28)'          // wM-Bus packet mode: active, no RSSI stream
          } else {
            const p = live['p' + band][i], n = live['n' + band][i]
            if (p == null) continue
            const v = Math.max(0, Math.min(1, (p - n) / SPAN_DB))
            ctx.fillStyle = LUT[Math.round(0.06 * 255 + v * 0.94 * 255)]
          }
          ctx.fillRect(x, y, Math.ceil(colW) + 0.5, laneH - 1)
        }
      }

      // decoded telegrams
      ctx.font = '10px ui-monospace, SFMono-Regular, Menlo, monospace'
      const lastLabelX = {}
      for (const m of markers) {
        if (m.t < t0) continue
        const li = laneIdx(lanes, m.band, m.f)
        if (li < 0) continue
        const x = gutL + ((m.t - t0) / 1000) * 5 * colW, y = gutT + li * laneH + laneH / 2
        ctx.fillStyle = m.wm ? '#2dd4bf' : '#ffffff'
        ctx.shadowColor = m.wm ? '#2dd4bf' : BAND_COL[m.band]; ctx.shadowBlur = 8
        ctx.beginPath(); ctx.arc(x, y, m.wm ? 2 : 3, 0, Math.PI * 2); ctx.fill()
        ctx.shadowBlur = 0
        if (!m.wm && laneH >= 16 && (lastLabelX[li] == null || x - lastLabelX[li] > 110)) {
          ctx.fillStyle = 'rgba(226,234,243,0.85)'
          ctx.fillText(m.label, Math.min(x + 6, w - 120), y - 5)
          lastLabelX[li] = x
        }
      }

      // lane labels
      ctx.textBaseline = 'middle'
      lanes.forEach((l, i) => {
        const y = gutT + i * laneH + laneH / 2
        ctx.fillStyle = BAND_COL[l.band]
        ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, monospace'
        ctx.fillText(l.f.toFixed(2), 8, y)
      })
      ctx.textBaseline = 'alphabetic'
      // band separator
      const firstB868 = lanes.findIndex(l => l.band === '868')
      if (firstB868 > 0) {
        ctx.strokeStyle = 'rgba(122,136,158,0.35)'; ctx.lineWidth = 1
        ctx.beginPath(); ctx.moveTo(0, gutT + firstB868 * laneH - 0.5); ctx.lineTo(w, gutT + firstB868 * laneH - 0.5); ctx.stroke()
      }
      // time axis
      ctx.fillStyle = 'rgba(122,136,158,0.9)'; ctx.font = '10px ui-monospace, SFMono-Regular, Menlo, monospace'
      for (let s = 0; s <= seconds; s += seconds >= 120 ? 30 : 15) {
        const x = gutL + plotW - (s / seconds) * plotW
        ctx.fillRect(x, gutT + plotH, 1, 4)
        ctx.fillText(s === 0 ? 'jetzt' : `−${s}s`, Math.min(x - (s === 0 ? 24 : 12), w - 30), height - 4)
      }
    }
    raf = requestAnimationFrame(draw)
    const tick = setInterval(() => { dirty = true }, 1000)
    return () => { cancelAnimationFrame(raf); clearInterval(tick); ro.disconnect(); offFrame(); offSig() }
  }, [height, seconds, palette])

  return (
    <div ref={wrap} class="w-full rounded-xl overflow-hidden border border-line/60">
      <canvas ref={cv} class="block" role="img" aria-label={`Wasserfall der letzten ${seconds} Sekunden: Empfangspegel je Funkkanal auf 433 und 868 MHz, Punkte markieren empfangene Telegramme`} />
    </div>
  )
}

/** Small legend for the waterfall colour scale */
export const WaterfallLegend = ({ palette = 'ice' }) => { const LUT = LUTS[palette] || LUTS.ice; return (
  <div class="flex items-center gap-2 text-[11px] text-muted">
    <span>Rauschen</span>
    <span class="h-2 w-28 rounded-full" aria-hidden="true" style={{ background: `linear-gradient(90deg, ${LUT[15]}, ${LUT[80]}, ${LUT[130]}, ${LUT[180]}, ${LUT[225]}, ${LUT[255]})` }} />
    <span>+{SPAN_DB} dB</span>
    <span class="ml-2 inline-flex items-center gap-1"><span class="w-2 h-2 rounded-full bg-white shadow-[0_0_6px_#fff]" />Telegramm</span>
    <span class="inline-flex items-center gap-1"><span class="w-3 h-2 rounded-sm" style={{ background: 'rgba(45,212,191,0.5)' }} />wM-Bus-Paketmodus</span>
  </div>
) }
