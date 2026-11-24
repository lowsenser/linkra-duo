import { useEffect, useRef } from 'preact/hooks'
import { rssiHist, histTick } from '../lib/store'
import { theme } from '../lib/prefs'

// uPlot (~23 KB gzip) is loaded on first use only – the standard view never needs it
let uPlotLib = null
const loadUPlot = () => uPlotLib || (uPlotLib = import('uplot').then(m => m.default))

const css = (v, a = 1) => `rgb(${getComputedStyle(document.documentElement).getPropertyValue(v).trim()} / ${a})`

/** Live RSSI / noise floor / threshold of one or both radios (uPlot, canvas) */
export function RssiChart({ bands = ['433', '868'], height = 220, window: winSec = 300 }) {
  const el = useRef(null)
  const plot = useRef(null)

  useEffect(() => {
    let alive = true, ro = null, unsub = null
    const series = [{ label: 'Zeit', value: (u, v) => v == null ? '—' : new Date(v * 1000).toLocaleTimeString('de-DE') }]
    const keys = []
    for (const b of bands) {
      const col = b === '433' ? '--b433' : '--b868'
      series.push({ label: `RSSI ${b}`, stroke: css(col), width: 1.5, points: { show: false }, value: (u, v) => v == null ? '—' : `${v} dBm` })
      series.push({ label: `Rauschen ${b}`, stroke: css(col, .45), width: 1, dash: [4, 3], points: { show: false }, value: (u, v) => v == null ? '—' : `${v}` })
      series.push({ label: `Schwelle ${b}`, stroke: css('--warn', .7), width: 1, dash: [2, 4], points: { show: false }, show: bands.length === 1, value: (u, v) => v == null ? '—' : `${v}` })
      keys.push(`r${b}`, `n${b}`, `th${b}`)
    }
    const grid = { stroke: css('--line'), width: 1 }
    const axis = { stroke: css('--muted'), grid, ticks: { stroke: css('--line') }, font: '11px ui-sans-serif, system-ui' }
    const opts = {
      width: el.current.clientWidth, height,
      series,
      scales: { x: { time: true }, y: { range: (u, lo, hi) => [Math.min(-115, Math.floor(lo / 5) * 5), Math.max(-40, Math.ceil(hi / 5) * 5)] } },
      axes: [{ ...axis, values: (u, sp) => sp.map(v => new Date(v * 1000).toLocaleTimeString('de-DE', { hour: '2-digit', minute: '2-digit' })) }, { ...axis, size: 44, values: (u, v) => v.map(x => x + '') }],
      legend: { live: true },
      cursor: { drag: { x: false, y: false } },
    }
    const data = () => {
      const t = rssiHist.t
      const from = t.length ? t[t.length - 1] - winSec : 0
      let i = 0; while (i < t.length && t[i] < from) i++
      return [t.slice(i), ...keys.map(k => rssiHist[k].slice(i))]
    }
    loadUPlot().then((UPlot) => {
      if (!alive || !el.current) return
      el.current.textContent = ''
      plot.current = new UPlot({ ...opts, width: el.current.clientWidth }, data(), el.current)
      ro = new ResizeObserver(() => plot.current && plot.current.setSize({ width: el.current.clientWidth, height }))
      ro.observe(el.current)
      unsub = histTick.subscribe(() => plot.current && plot.current.setData(data()))
    }).catch(() => { if (alive && el.current) el.current.textContent = 'Diagramm konnte nicht geladen werden' })
    return () => { alive = false; unsub?.(); ro?.disconnect(); plot.current?.destroy(); plot.current = null }
  }, [bands.join(), height, winSec, theme.value])

  return <div ref={el} class="w-full text-[12px] text-muted" style={{ minHeight: height }} role="img" aria-label={`Pegelverlauf ${bands.join(' und ')} MHz`} />
}
