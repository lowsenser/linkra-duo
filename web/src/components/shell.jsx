// Small shell pieces that subscribe to fast-changing signals themselves, so the
// app shell does not re-render every second (review 07 M1).
import { Component } from 'preact'
import { useEffect, useState } from 'preact/hooks'
import { AlertTriangle, RefreshCw, WifiOff, X } from 'lucide-preact'
import { conn, online, offlineSince, counters, notice, toasts, retryNow } from '../lib/store'

/** Three-step status wording (review 07 N14) */
export function connText(isOnline, sse) {
  if (!isOnline) return ['bad', 'nicht erreichbar']
  if (sse === 'live') return ['ok', 'verbunden · Live']
  if (sse === 'paused') return ['warn', 'verbunden · Live pausiert']
  return ['warn', 'verbunden · ohne Live-Daten']
}

export function ConnState({ compact }) {
  const [tone, text] = connText(online.value, conn.value)
  return (
    <span class="inline-flex items-center gap-1.5" role="status">
      <span class={`w-1.5 h-1.5 rounded-full ${{ ok: 'bg-ok', warn: 'bg-warn', bad: 'bg-bad' }[tone]}`} aria-hidden="true" />
      {compact ? <span class="hidden sm:inline">{text}</span> : text}
      {compact && <span class="sr-only sm:hidden">{text}</span>}
    </span>
  )
}

export function LiveBadge() {
  const n = counters.value.decoded
  return n > 0 ? <span class="ml-auto text-[10px] num px-1.5 rounded-md bg-ok/15 text-ok" aria-label={`${n} decodierte Telegramme`}>{n}</span> : null
}

export function Notice() {
  return notice.value ? <span class="text-warn" role="status">{notice.value}</span> : null
}

/** "Connection lost – values stale" with its own 1 s clock (only while offline) */
export function StaleBanner() {
  const off = !online.value
  const since = offlineSince.value
  const [, tick] = useState(0)
  useEffect(() => {
    document.documentElement.classList.toggle('is-stale', off)
    if (!off) return
    const t = setInterval(() => tick(x => x + 1), 1000)
    return () => clearInterval(t)
  }, [off])
  if (!off) return null
  const secs = since ? Math.round((Date.now() - since) / 1000) : null
  return (
    <div role="alert" class="sticky top-14 z-20 mx-4 md:mx-6 mt-3 flex flex-wrap items-center gap-x-3 gap-y-2 rounded-xl border border-warn/40 bg-panel px-4 py-2.5 text-[13px] shadow-lg">
      <WifiOff class="w-4 h-4 text-warn shrink-0" aria-hidden="true" />
      <span class="font-semibold">Verbindung zum Gerät verloren – Werte veraltet</span>
      <span class="text-muted">
        {since ? `Letzte Daten ${new Date(since).toLocaleTimeString('de-DE')}${secs != null ? ` (vor ${secs < 120 ? `${secs} s` : `${Math.round(secs / 60)} min`})` : ''}` : ''} · neuer Versuch läuft automatisch
      </span>
      <button class="btn btn-sm ml-auto" onClick={retryNow}><RefreshCw class="w-3.5 h-3.5" aria-hidden="true" />Jetzt versuchen</button>
    </div>
  )
}

export function Toasts() {
  const list = toasts.value
  const dismiss = (id) => { toasts.value = toasts.value.filter(t => t.id !== id) }
  return (
    <div class="fixed bottom-20 md:bottom-4 right-4 left-4 sm:left-auto z-50 flex flex-col items-end gap-2 pointer-events-none" aria-live="polite" aria-relevant="additions">
      {list.map(t => (
        <div key={t.id} role={t.kind === 'bad' ? 'alert' : 'status'}
          class={`card pointer-events-auto flex items-start gap-2 pl-4 pr-1.5 py-1.5 text-[13px] shadow-lg border-l-4 max-w-sm ${t.kind === 'ok' ? 'border-l-ok' : t.kind === 'bad' ? 'border-l-bad' : 'border-l-accent'}`}>
          <span class="py-1">{t.msg}</span>
          <button class="icon-btn w-7 h-7 shrink-0" aria-label="Meldung schließen" onClick={() => dismiss(t.id)}><X class="w-3.5 h-3.5" /></button>
        </div>
      ))}
    </div>
  )
}

/** A render error in one page must not blank the whole app (review 07 N2) */
export class ErrorBoundary extends Component {
  constructor() { super(); this.state = { err: null } }
  componentDidCatch(err) { this.setState({ err }); console.error(err) }
  render() {
    if (!this.state.err) return this.props.children
    return (
      <div class="card p-6 max-w-xl" role="alert">
        <div class="flex items-center gap-2 font-semibold"><AlertTriangle class="w-5 h-5 text-warn" aria-hidden="true" />Diese Ansicht konnte nicht angezeigt werden</div>
        <p class="text-[13px] text-muted mt-2">Die übrigen Seiten funktionieren weiter. Meist hilft ein Neuladen.</p>
        <pre class="mt-3 text-[11.5px] font-mono text-muted whitespace-pre-wrap break-words">{String(this.state.err?.message || this.state.err)}</pre>
        <div class="flex gap-2 mt-4">
          <button class="btn-primary" onClick={() => location.reload()}><RefreshCw class="w-4 h-4" />Neu laden</button>
          <button class="btn" onClick={() => this.setState({ err: null })}>Erneut versuchen</button>
        </div>
      </div>
    )
  }
}

/** Code-split component: loads on first render, shows a quiet placeholder meanwhile */
export function lazy(loader) {
  let Comp = null, promise = null, error = null
  return function Lazy(props) {
    const [, force] = useState(0)
    if (error) throw error
    if (Comp) return <Comp {...props} />
    if (!promise) promise = loader().then(c => { Comp = c }, e => { error = e; promise = null })
    promise?.then(() => force(x => x + 1))
    return <div class="py-16 grid place-items-center text-muted text-[13px]" role="status"><span class="animate-pulse">Lade …</span></div>
  }
}
