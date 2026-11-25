import { render } from 'preact'
import { LayoutDashboard, Radio, Boxes, Waves, Cpu, Settings, FlaskConical, LogOut, UserRound } from 'lucide-preact'
import './styles/index.css'
import { route } from './lib/router'
import { theme, expert } from './lib/prefs'
import { rf, liveFrame, toast } from './lib/store'
import { authView, authStatus, checkAuth, logout, takeByeMessage } from './lib/auth'
import { AuthScreen } from './pages/Auth'
import { Overview } from './pages/Overview'
import { Devices } from './pages/Devices'
import { SettingsPage } from './pages/Settings'
import { ConnState, LiveBadge, Notice, StaleBanner, Toasts, ErrorBoundary, lazy, UpdateHint } from './components/shell'
import logoDark from './assets/logo-dark.webp'
import logoLight from './assets/logo-light.webp'
import iconDark from './assets/icon-dark.webp'
import iconLight from './assets/icon-light.webp'

// Expert pages are loaded on first visit only (start page budget, review 07 §3.4)
const Live = lazy(() => import('./pages/Live').then(m => m.Live))
const RadioPage = lazy(() => import('./pages/Radio').then(m => m.RadioPage))
const SystemPage = lazy(() => import('./pages/System').then(m => m.SystemPage))

// [route, label, icon, page, expertOnly]
const NAV = [
  ['overview', 'Übersicht', LayoutDashboard, Overview, false],
  ['live', 'Live', Radio, Live, true],
  ['devices', 'Geräte', Boxes, Devices, false],
  ['radio', 'Funk & Profile', Waves, RadioPage, true],
  ['system', 'System', Cpu, SystemPage, true],
  ['settings', 'Einstellungen', Settings, SettingsPage, false],
]
const HIDDEN = { mqtt: ['mqtt', 'Einstellungen', Settings, SettingsPage, false] }

function Link({ id, label, Icon, active, badge }) {
  return (
    <a href={`#${id}`} aria-current={active ? 'page' : undefined}
      class={`relative flex items-center gap-3 px-3 h-10 rounded-xl text-[13.5px] font-medium transition-all ${active ? 'text-fg bg-accent/10' : 'text-muted hover:text-fg hover:bg-raised/70'}`}>
      {active && <span class="absolute left-0 top-2 bottom-2 w-[3px] rounded-r bg-accent shadow-[0_0_12px_rgb(var(--glow))]" aria-hidden="true" />}
      <Icon class={`w-[18px] h-[18px] ${active ? 'text-accent' : ''}`} aria-hidden="true" />{label}
      {badge && <LiveBadge />}
    </a>
  )
}

function LiveMini() {
  const f = liveFrame.value, o = rf.value
  return (
    <div class="mx-3 mb-3 p-3 rounded-xl border border-line bg-raised/40 space-y-2" aria-label="Empfänger">
      {['433', '868'].map(b => {
        const m = o?.modules.find(x => x.band === b)
        const p = f ? f['p' + b] : null, wm = b === '868' && f?.wm
        const rx = f && (f.rx & (b === '433' ? 1 : 2))
        return (
          <div key={b} class="flex items-center gap-2 text-[11.5px]">
            <span class={`w-1.5 h-1.5 rounded-full ${rx ? 'rx-on' : ''}`} style={{ background: `rgb(var(--b${b}))`, color: `rgb(var(--b${b}))` }} aria-hidden="true" />
            <span class="font-semibold" style={{ color: `rgb(var(--b${b}))` }}>{b}</span>
            <span class="font-mono text-muted truncate flex-1">{m?.profile || '—'}</span>
            <span class="font-mono num live-val" title={wm ? 'wM-Bus-Paketmodus' : 'Pegel in dBm'}>{wm ? 'Paket' : p ?? '—'}</span>
          </div>
        )
      })}
    </div>
  )
}

function UserBox() {
  const st = authStatus.value
  if (!st?.user) return null
  return (
    <div class="flex items-center gap-2 px-3 py-2 rounded-xl border border-line/70 bg-raised/30">
      <a href="#settings/account" class="flex items-center gap-2 min-w-0 flex-1 text-[12.5px] hover:text-accent" title="Konto">
        <UserRound class="w-4 h-4 text-muted shrink-0" aria-hidden="true" /><span class="truncate"><span class="sr-only">Angemeldet als </span>{st.user}</span>
      </a>
      <button class="icon-btn w-8 h-8" onClick={logout} aria-label="Abmelden" title="Abmelden"><LogOut class="w-4 h-4" /></button>
    </div>
  )
}

function Shell() {
  const { page, arg } = route.value
  const nav = NAV.filter(n => !n[4] || expert.value)
  const entry = NAV.find(n => n[0] === page) || HIDDEN[page] || NAV[0]
  const Page = entry[3]
  const dark = theme.value === 'dark'
  const isActive = (id) => entry[0] === id || (id === 'settings' && entry[0] === 'mqtt')

  return (
    <div class="min-h-screen md:pl-60">
      <a href="#main" class="skip-link btn-primary" onClick={(e) => { e.preventDefault(); document.getElementById('main')?.focus() }}>Zum Inhalt springen</a>
      <aside class="hidden md:flex fixed inset-y-0 left-0 w-60 flex-col border-r border-line bg-panel/70 backdrop-blur-xl z-20">
        <div class="px-5 pt-5 pb-4">
          <img src={dark ? logoDark : logoLight} alt="Linkra Duo" class="h-12 w-auto -ml-1" />
          <div class="text-[10.5px] uppercase tracking-[.2em] text-muted mt-2">433 · 868 MHz Funk-Gateway</div>
        </div>
        <LiveMini />
        <nav class="flex-1 px-3 space-y-1" aria-label="Hauptnavigation">
          {nav.map(([id, label, Icon]) => <Link key={id} id={id} label={label} Icon={Icon} active={isActive(id)} badge={id === 'live'} />)}
        </nav>
        <div class="p-3 space-y-2">
          <UserBox />
          <div class="px-1 text-[11px] text-muted flex items-center justify-between gap-2">
            <ConnState />
            {expert.value && <a href="#settings/look" class="inline-flex items-center gap-1 text-accent" title="Expertenmodus aktiv"><FlaskConical class="w-3.5 h-3.5" aria-hidden="true" /><span class="sr-only">Expertenmodus</span></a>}
          </div>
        </div>
      </aside>

      <header class="sticky top-0 z-30 h-14 flex items-center gap-3 px-4 md:px-6 border-b border-line/70 bg-bg/70 backdrop-blur-xl">
        <img src={dark ? iconDark : iconLight} alt="" class="md:hidden w-8 h-8" />
        <h1 class="text-[15px] font-semibold tracking-tight">{entry[1]}</h1>
        <div class="ml-auto flex items-center gap-3 text-[12px] text-muted">
          <Notice />
          <UpdateHint />
          <ConnState compact />
          <a href="#settings/account" class="md:hidden icon-btn" aria-label="Konto"><UserRound class="w-4 h-4" /></a>
        </div>
      </header>

      <StaleBanner />

      <main id="main" tabIndex={-1} class="p-4 md:p-6 pb-24 md:pb-8 max-w-[1600px] outline-none">
        <ErrorBoundary key={`${entry[0]}/${entry[0] === 'settings' ? arg : ''}`}><Page /></ErrorBoundary>
      </main>

      <nav class="md:hidden fixed bottom-0 inset-x-0 z-30 grid border-t border-line bg-panel/90 backdrop-blur-xl pb-[env(safe-area-inset-bottom)]"
        style={{ gridTemplateColumns: `repeat(${nav.length}, minmax(0, 1fr))` }} aria-label="Hauptnavigation">
        {nav.map(([id, label, Icon]) => (
          <a key={id} href={`#${id}`} aria-current={isActive(id) ? 'page' : undefined}
            class={`flex flex-col items-center justify-center gap-0.5 min-h-[52px] py-1.5 text-[10px] ${isActive(id) ? 'text-accent' : 'text-muted'}`}>
            <Icon class="w-5 h-5" aria-hidden="true" />{label.split(' ')[0]}
          </a>
        ))}
      </nav>
    </div>
  )
}

function App() {
  return (
    <>
      {authView.value === 'app' ? <Shell /> : <AuthScreen />}
      <Toasts />
    </>
  )
}

const bye = takeByeMessage()
if (bye) setTimeout(() => toast(bye, 'ok'), 300)
checkAuth().catch(() => { })
render(<App />, document.getElementById('app'))
