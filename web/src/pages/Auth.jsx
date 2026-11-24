// Sign-in, first-run setup and "gateway unreachable" screens (before the app shell)
import { useEffect, useReducer, useRef, useState } from 'preact/hooks'
import { ArrowLeft, ArrowRight, KeyRound, LifeBuoy, LogIn, RefreshCw, ShieldCheck, UserRound, WifiOff, Clock } from 'lucide-preact'
import { Field, PasswordInput, StrengthMeter, RuleList, Alert, CodeInput, Spinner } from '../components/form'
import { authView, authStatus, authNotice, checkAuth, login, setupAccount } from '../lib/auth'
import { loginInit, loginReducer, loginBody, credentialRules, rulesOk, cleanRecovery, recoveryComplete, fmtCountdown, authErrorText } from '../lib/authlogic'
import { theme } from '../lib/prefs'
import logoDark from '../assets/logo-dark.webp'
import logoLight from '../assets/logo-light.webp'

export function AuthScreen() {
  const v = authView.value
  return (
    <div class="auth-bg min-h-screen flex flex-col items-center justify-center px-4 py-10">
      <main class="w-full max-w-[420px] page-in" aria-labelledby="auth-title">
        <div class="flex flex-col items-center mb-6">
          <img src={theme.value === 'dark' ? logoDark : logoLight} alt="Linkra Duo" class="h-14 w-auto" />
          <div class="mt-2 text-[10.5px] uppercase tracking-[.22em] text-muted">433 · 868 MHz Funk-Gateway</div>
        </div>
        {v === 'checking' && <Checking />}
        {v === 'unreachable' && <Unreachable />}
        {v === 'setup' && <Setup />}
        {v === 'login' && <Login />}
      </main>
      <BandLines />
    </div>
  )
}

/** Decorative: the two receive bands as quiet signal traces under the card */
function BandLines() {
  return (
    <svg class="auth-lines" viewBox="0 0 1200 120" preserveAspectRatio="none" aria-hidden="true">
      <path d="M0 70 C150 70 180 30 260 30 S380 95 470 95 S600 50 700 50 S860 88 960 88 S1110 60 1200 60" stroke="rgb(var(--b433))" />
      <path d="M0 90 C120 90 200 60 300 60 S420 100 540 100 S690 40 800 40 S930 80 1040 80 S1160 70 1200 70" stroke="rgb(var(--b868))" />
    </svg>
  )
}

function Checking() {
  return (
    <div class="card auth-card flex items-center justify-center gap-3 py-10 text-muted" role="status">
      <Spinner class="w-5 h-5 text-accent" /> Verbinde mit dem Gerät …
    </div>
  )
}

function Unreachable() {
  const [busy, setBusy] = useState(false)
  const retry = async () => { setBusy(true); try { await checkAuth() } catch { } setBusy(false) }
  return (
    <section class="card auth-card text-center">
      <div class="mx-auto w-12 h-12 rounded-2xl grid place-items-center bg-warn/10 text-warn mb-4"><WifiOff class="w-6 h-6" aria-hidden="true" /></div>
      <h1 id="auth-title" class="text-lg font-semibold">Gerät nicht erreichbar</h1>
      <p class="text-[13px] text-muted mt-1.5">Das Gateway antwortet gerade nicht. Ist es eingeschaltet und im selben WLAN? Wir versuchen es automatisch weiter.</p>
      <button class="btn-primary btn-lg w-full mt-5" onClick={retry} disabled={busy}>{busy ? <Spinner /> : <RefreshCw class="w-4 h-4" />}Erneut versuchen</button>
    </section>
  )
}

// ---------- Sign-in ----------
function Login() {
  const [s, dispatch] = useReducer(loginReducer, undefined, loginInit)
  const [username, setUsername] = useState('')
  const [password, setPassword] = useState('')
  const [code, setCode] = useState('')
  const [now, setNow] = useState(Date.now())
  const userRef = useRef(null), pwRef = useRef(null), recRef = useRef(null), codeRef = useRef(null)
  const st = authStatus.value
  const notice = authNotice.value

  // countdown while locked
  useEffect(() => {
    if (!s.lockedUntil) return
    const t = setInterval(() => { const n = Date.now(); setNow(n); dispatch({ type: 'unlock', now: n }) }, 500)
    return () => clearInterval(t)
  }, [s.lockedUntil])

  // focus: first field, then the code field on the second step
  useEffect(() => {
    if (s.step === 'credentials') (username ? pwRef : userRef).current?.focus()
    else if (s.useRecovery) recRef.current?.focus()
    else codeRef.current?.focus()
  }, [s.step, s.useRecovery])
  // after a failed check the disabled field lost focus: give it back
  useEffect(() => { if (!s.busy && s.step === 'second' && s.useRecovery) recRef.current?.focus() }, [s.busy])
  // wrong password: select it for retyping
  useEffect(() => { if (s.error?.code === 'invalid_credentials') { pwRef.current?.focus(); pwRef.current?.select() } }, [s.error])

  const locked = s.lockedUntil > now
  const secondOk = s.useRecovery ? recoveryComplete(code) : code.length === 6

  const submit = async (e, codeOverride) => {
    e?.preventDefault()
    if (s.busy || locked) return
    if (s.step === 'credentials' && (!username.trim() || !password)) return
    if (s.step === 'second' && !codeOverride && !secondOk) return
    dispatch({ type: 'submit' })
    try {
      await login(loginBody(s, { username, password, code: codeOverride || code }))
      dispatch({ type: 'success' })
    } catch (err) {
      if (err.code === 'setup_required') { checkAuth().catch(() => { }); return }   // account was reset (BOOT button)
      if (err.code !== 'totp_required') setCode('')
      dispatch({ type: 'fail', code: err.code, message: err.message, retryAfter: err.retryAfter, now: Date.now() })
      if (err.code === 'invalid_credentials' && !s.useRecovery) setPassword('')
    }
  }

  const err = s.error
  return (
    <section class="card auth-card">
      {s.step === 'credentials' ? (
        <>
          <h1 id="auth-title" class="text-xl font-semibold tracking-tight">Anmelden</h1>
          <p class="text-[13px] text-muted mt-1">Melde dich an, um dein Funk-Gateway zu verwalten.{st?.totp_enabled ? ' Danach fragt das Gerät nach dem Code aus deiner Authenticator-App.' : ''}</p>
        </>
      ) : (
        <>
          <button type="button" class="link-btn -ml-1 mb-3" onClick={() => { setCode(''); dispatch({ type: 'back' }) }}><ArrowLeft class="w-4 h-4" />Zurück</button>
          <div class="flex items-center gap-3">
            <div class="w-10 h-10 rounded-xl grid place-items-center bg-accent/10 text-accent shrink-0">{s.useRecovery ? <LifeBuoy class="w-5 h-5" aria-hidden="true" /> : <ShieldCheck class="w-5 h-5" aria-hidden="true" />}</div>
            <div>
              <h1 id="auth-title" class="text-lg font-semibold tracking-tight">{s.useRecovery ? 'Wiederherstellungscode' : 'Bestätigungscode'}</h1>
              <p class="text-[13px] text-muted">{s.useRecovery ? 'Gib einen deiner gesicherten Codes ein.' : <>Code aus deiner Authenticator-App für <b class="text-fg font-medium">{username}</b>.</>}</p>
            </div>
          </div>
        </>
      )}

      {notice && s.step === 'credentials' && !err && <Alert tone="info" class="mt-4">{notice}</Alert>}
      {st && st.time_synced === false && st.totp_enabled && s.step === 'second' && !s.useRecovery && !err && (
        <Alert tone="warn" class="mt-4" title="Uhrzeit des Geräts noch nicht synchron">Codes aus der App können erst geprüft werden, wenn das Gerät die Uhrzeit aus dem Internet geholt hat. Nutze solange einen Wiederherstellungscode.</Alert>
      )}
      {err && (
        <Alert tone={err.code === 'time_not_synced' || err.code === 'locked' ? 'warn' : 'bad'} class="mt-4"
          title={err.code === 'locked' ? (locked ? `Bitte warten: ${fmtCountdown((s.lockedUntil - now) / 1000)}` : 'Du kannst es jetzt erneut versuchen') : err.code === 'time_not_synced' ? 'Uhrzeit des Geräts noch nicht synchron' : null}>
          {err.code === 'locked' ? 'Nach mehreren Fehlversuchen ist die Anmeldung kurz gesperrt. Das schützt dein Gerät vor dem Erraten des Passworts.' : err.text}
        </Alert>
      )}

      <form class="mt-5 space-y-4" onSubmit={submit} noValidate>
        {s.step === 'credentials' ? (
          <>
            <Field label="Benutzername">
              {(a) => <input {...a} ref={userRef} class="input input-lg w-full" value={username} onInput={e => setUsername(e.currentTarget.value)}
                autocomplete="username" autocapitalize="off" spellcheck={false} required disabled={s.busy} />}
            </Field>
            <Field label="Passwort">
              {(a) => <PasswordInput {...a} inputRef={pwRef} value={password} onInput={setPassword} autocomplete="current-password" required disabled={s.busy} />}
            </Field>
          </>
        ) : s.useRecovery ? (
          <Field label="Wiederherstellungscode" hint="Format: abcd-efgh. Jeder Code funktioniert nur einmal.">
            {(a) => <input {...a} ref={recRef} class="input input-lg w-full font-mono tracking-[.2em] text-center" value={code} placeholder="xxxx-xxxx"
              onInput={e => setCode(cleanRecovery(e.currentTarget.value))} autocomplete="off" autocapitalize="off" spellcheck={false} disabled={s.busy} />}
          </Field>
        ) : (
          <CodeInput inputRef={codeRef} value={code} onChange={setCode} disabled={s.busy || locked} autoFocus
            onComplete={(v) => submit(null, v)} label="Sechsstelliger Code aus der Authenticator-App" />
        )}

        <button type="submit" class="btn-primary btn-lg w-full" disabled={s.busy || locked || (s.step === 'second' && !secondOk)}>
          {s.busy ? <><Spinner />Prüfe …</> : s.step === 'credentials' ? <><LogIn class="w-4 h-4" />Anmelden</> : <>Bestätigen<ArrowRight class="w-4 h-4" /></>}
        </button>

        {s.step === 'second' && (
          <button type="button" class="link-btn mx-auto" onClick={() => { setCode(''); dispatch({ type: 'recovery', on: !s.useRecovery }) }}>
            {s.useRecovery ? <><ShieldCheck class="w-4 h-4" />Code aus der App verwenden</> : <><LifeBuoy class="w-4 h-4" />Wiederherstellungscode verwenden</>}
          </button>
        )}
      </form>
      {s.step === 'credentials' && (
        <details class="mt-5 text-[12.5px] text-muted">
          <summary class="cursor-pointer hover:text-fg select-none">Passwort vergessen?</summary>
          <p class="mt-2">Starte das Gerät neu (Strom kurz trennen). Drücke dann <b class="text-fg">innerhalb der ersten 60 Sekunden</b> die <b class="text-fg">BOOT-Taste</b> und halte sie <b class="text-fg">10 Sekunden</b> gedrückt. Dann werden nur die Zugangsdaten gelöscht und du kannst hier ein neues Konto anlegen. Geräte, WLAN und MQTT bleiben erhalten.</p>
          <p class="mt-1.5">Wichtig: Die Taste nicht schon beim Einschalten halten – dann startet das Gerät im Programmiermodus.</p>
        </details>
      )}
    </section>
  )
}

// ---------- First-run setup ----------
function Setup() {
  const [step, setStep] = useState(0)            // 0 welcome, 1 account
  const [username, setUsername] = useState('admin')
  const [password, setPassword] = useState('')
  const [repeat, setRepeat] = useState('')
  const [touched, setTouched] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState(null)
  const userRef = useRef(null)
  const rules = credentialRules({ username, password, repeat })
  const ok = rulesOk(rules)
  useEffect(() => { if (step === 1) userRef.current?.focus() }, [step])

  const submit = async (e) => {
    e.preventDefault()
    setTouched(true)
    if (!ok || busy) return
    setBusy(true); setError(null)
    try { await setupAccount(username.trim(), password) }
    catch (err) {
      if (err.code === 'exists') { checkAuth().catch(() => { }) }
      setError(authErrorText(err.code, err.message))
    }
    setBusy(false)
  }

  return (
    <section class="card auth-card">
      <ol class="flex items-center gap-2 mb-5 text-[11.5px]" aria-label="Fortschritt">
        {['Willkommen', 'Konto anlegen'].map((l, i) => (
          <li key={l} class={`flex items-center gap-2 ${i <= step ? 'text-fg' : 'text-muted'}`} aria-current={i === step ? 'step' : undefined}>
            <span class={`w-5 h-5 rounded-full grid place-items-center text-[10.5px] font-semibold ${i < step ? 'bg-ok text-on-accent' : i === step ? 'bg-accent text-on-accent' : 'bg-raised text-muted'}`}>{i + 1}</span>
            <span class={i === step ? 'font-medium' : 'hidden sm:inline'}>{l}</span>
            {i < 1 && <span class="w-4 h-px bg-line" aria-hidden="true" />}
          </li>
        ))}
      </ol>

      {step === 0 ? (
        <>
          <h1 id="auth-title" class="text-xl font-semibold tracking-tight">Willkommen bei Linkra Duo</h1>
          <p class="text-[13.5px] text-muted mt-2">Dein Gateway empfängt Funksensoren auf 433 und 868 MHz und gibt sie an Home Assistant weiter. Lege zuerst ein Konto an – nur damit kann die Oberfläche geöffnet werden.</p>
          <ul class="mt-4 space-y-2 text-[13px]">
            <li class="flex gap-2.5"><UserRound class="w-4 h-4 text-accent shrink-0 mt-0.5" aria-hidden="true" />Ein Konto mit Benutzername und Passwort</li>
            <li class="flex gap-2.5"><ShieldCheck class="w-4 h-4 text-accent shrink-0 mt-0.5" aria-hidden="true" />Später optional: Zwei-Faktor-Schutz mit einer Authenticator-App</li>
            <li class="flex gap-2.5"><Clock class="w-4 h-4 text-accent shrink-0 mt-0.5" aria-hidden="true" />Dauert eine Minute</li>
          </ul>
          <button class="btn-primary btn-lg w-full mt-6" onClick={() => setStep(1)}>Los geht's<ArrowRight class="w-4 h-4" /></button>
        </>
      ) : (
        <form onSubmit={submit} noValidate>
          <h1 id="auth-title" class="text-xl font-semibold tracking-tight">Konto anlegen</h1>
          <p class="text-[13px] text-muted mt-1">Wähle ein Passwort, das du sonst nirgends verwendest.</p>
          {error && <Alert tone="bad" class="mt-4">{error}</Alert>}
          <div class="mt-5 space-y-4">
            <Field label="Benutzername" hint="3 bis 32 Zeichen: Buchstaben, Ziffern, . _ -">
              {(a) => <input {...a} ref={userRef} class="input input-lg w-full" value={username} maxLength={32}
                onInput={e => setUsername(e.currentTarget.value)} autocomplete="username" autocapitalize="off" spellcheck={false} disabled={busy} />}
            </Field>
            <Field label="Passwort">
              {(a) => <PasswordInput {...a} value={password} onInput={setPassword} autocomplete="new-password" maxLength={128} disabled={busy} />}
            </Field>
            <StrengthMeter password={password} />
            <Field label="Passwort wiederholen" error={touched && repeat && repeat !== password ? 'Die Passwörter stimmen nicht überein.' : null}>
              {(a) => <PasswordInput {...a} value={repeat} onInput={setRepeat} autocomplete="new-password" maxLength={128} disabled={busy} />}
            </Field>
            <div class="rounded-xl bg-raised/60 border border-line/70 px-3.5 py-3"><RuleList rules={rules} /></div>
          </div>
          <button type="submit" class="btn-primary btn-lg w-full mt-5" disabled={busy || !ok}>
            {busy ? <><Spinner />Lege Konto an …</> : <><KeyRound class="w-4 h-4" />Konto anlegen</>}
          </button>
          <button type="button" class="link-btn mx-auto mt-3" onClick={() => setStep(0)}><ArrowLeft class="w-4 h-4" />Zurück</button>
        </form>
      )}
    </section>
  )
}
