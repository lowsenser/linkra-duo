// Settings → "Konto": session, password, two-factor authentication
import { useEffect, useRef, useState } from 'preact/hooks'
import { UserRound, LogOut, MonitorSmartphone, KeyRound, ShieldCheck, ShieldOff, ShieldPlus, Copy, Download, LifeBuoy, Smartphone, Check, Cpu } from 'lucide-preact'
import { Card, Chip } from '../components/ui'
import { Field, PasswordInput, StrengthMeter, RuleList, Alert, CodeInput, Spinner } from '../components/form'
import { QrCode } from '../components/QrCode'
import { authStatus, logout, logoutAll, changePassword, totpSetup, totpEnable, totpDisable } from '../lib/auth'
import { credentialRules, rulesOk, authErrorText, formErrorText, groupSecret, recoveryFile, cleanRecovery, recoveryComplete } from '../lib/authlogic'
import { toast, reconnectLive } from '../lib/store'
import { download } from '../lib/format'

/** Clipboard API needs a secure context; the gateway runs on plain http → fallback */
export async function copyText(text) {
  try { if (navigator.clipboard && window.isSecureContext) { await navigator.clipboard.writeText(text); return true } } catch { }
  const ta = document.createElement('textarea')
  ta.value = text; ta.setAttribute('readonly', ''); ta.style.position = 'fixed'; ta.style.opacity = '0'
  document.body.appendChild(ta); ta.select()
  let ok = false
  try { ok = document.execCommand('copy') } catch { }
  ta.remove()
  return ok
}

export function AccountPanel() {
  const st = authStatus.value
  if (st?.legacy) {
    return (
      <Alert tone="info" title="Anmeldung noch nicht verfügbar">
        Die Firmware auf diesem Gerät unterstützt noch keine Anmeldung. Nach dem nächsten Firmware-Update kannst du hier ein Konto und die Zwei-Faktor-Authentifizierung einrichten.
      </Alert>
    )
  }
  return (
    <div class="grid lg:grid-cols-2 gap-3">
      <SessionCard user={st?.user} />
      <PasswordCard />
      <TwoFactorCard st={st} />
      <RecoveryHint />
    </div>
  )
}

function SessionCard({ user }) {
  const [busy, setBusy] = useState(null)
  return (
    <Card title="Angemeldet" icon={UserRound}>
      <div class="flex items-center gap-3">
        <div class="w-11 h-11 rounded-2xl grid place-items-center bg-accent/10 text-accent text-[17px] font-semibold uppercase" aria-hidden="true">{(user || '?').slice(0, 1)}</div>
        <div class="min-w-0">
          <div class="font-semibold truncate">{user || '—'}</div>
          <div class="text-[12.5px] text-muted">Administrator dieses Geräts</div>
        </div>
      </div>
      <p class="text-[12.5px] text-muted mt-4">Eine Anmeldung bleibt 7 Tage gültig und verlängert sich bei jeder Nutzung. Nach einem Neustart des Geräts musst du dich neu anmelden.</p>
      <div class="flex flex-wrap gap-2 mt-4">
        <button class="btn" disabled={!!busy} onClick={() => { setBusy('one'); logout() }}>{busy === 'one' ? <Spinner /> : <LogOut class="w-4 h-4" />}Abmelden</button>
        <button class="btn-danger" disabled={!!busy}
          onClick={() => confirm('Alle Sitzungen beenden? Jeder Browser und jedes Handy – auch dieses – muss sich danach neu anmelden.') && (setBusy('all'), logoutAll())}>
          {busy === 'all' ? <Spinner /> : <MonitorSmartphone class="w-4 h-4" />}Überall abmelden
        </button>
      </div>
    </Card>
  )
}

function PasswordCard() {
  const [current, setCurrent] = useState('')
  const [next, setNext] = useState('')
  const [repeat, setRepeat] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState(null)
  const [done, setDone] = useState(false)
  const rules = credentialRules({ password: next, repeat }, { withUser: false })
  const ok = current && rulesOk(rules)

  const submit = async (e) => {
    e.preventDefault()
    if (!ok || busy) return
    setBusy(true); setError(null); setDone(false)
    try {
      await changePassword(current, next)
      setCurrent(''); setNext(''); setRepeat(''); setDone(true)
      toast('Passwort geändert', 'ok')
      reconnectLive()          // the gateway closes all event streams on a password change
    } catch (err) {
      setError(err.code === 'invalid_credentials' ? 'Das aktuelle Passwort stimmt nicht.' : formErrorText(err))
    }
    setBusy(false)
  }
  return (
    <Card title="Passwort ändern" icon={KeyRound}>
      <form onSubmit={submit} class="space-y-3.5" noValidate>
        {error && <Alert tone="bad">{error}</Alert>}
        {done && <Alert tone="ok">Passwort geändert. Alle anderen Sitzungen wurden beendet.</Alert>}
        <Field label="Aktuelles Passwort">
          {(a) => <PasswordInput {...a} value={current} onInput={setCurrent} autocomplete="current-password" disabled={busy} />}
        </Field>
        <Field label="Neues Passwort">
          {(a) => <PasswordInput {...a} value={next} onInput={setNext} autocomplete="new-password" maxLength={128} disabled={busy} />}
        </Field>
        {next && <StrengthMeter password={next} />}
        <Field label="Neues Passwort wiederholen" error={repeat && repeat !== next ? 'Die Passwörter stimmen nicht überein.' : null}>
          {(a) => <PasswordInput {...a} value={repeat} onInput={setRepeat} autocomplete="new-password" maxLength={128} disabled={busy} />}
        </Field>
        {next && <RuleList rules={rules} />}
        <button type="submit" class="btn-primary" disabled={!ok || busy}>{busy ? <Spinner /> : <Check class="w-4 h-4" />}Passwort ändern</button>
        <p class="text-[12px] text-muted">Andere angemeldete Browser werden dabei abgemeldet.</p>
      </form>
    </Card>
  )
}

// ---------- Two-factor ----------
function TwoFactorCard({ st }) {
  const [mode, setMode] = useState('idle')     // idle | setup | codes | disable
  const [setup, setSetup] = useState(null)     // { secret, otpauth_uri }
  const [codes, setCodes] = useState(null)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState(null)
  const enabled = !!st?.totp_enabled
  const headRef = useRef(null)

  // Recovery codes on screen: warn before the tab is closed
  useEffect(() => {
    if (mode !== 'codes') return
    const h = (e) => { e.preventDefault(); e.returnValue = '' }
    addEventListener('beforeunload', h)
    return () => removeEventListener('beforeunload', h)
  }, [mode])
  useEffect(() => { if (mode === 'codes' || mode === 'idle' && headRef.current?.dataset.was) headRef.current?.focus(); if (headRef.current) headRef.current.dataset.was = mode }, [mode])

  const start = async () => {
    setBusy(true); setError(null)
    try { setSetup(await totpSetup()); setMode('setup') }
    catch (err) { setError(authErrorText(err.code, err.message)) }
    setBusy(false)
  }
  const reset = () => { setMode('idle'); setSetup(null); setCodes(null); setError(null) }

  return (
    <section class="card lg:col-span-2" aria-labelledby="tfa-title">
      <header class="card-h">
        <h2 id="tfa-title" class="card-t"><ShieldCheck class="w-4 h-4 text-muted" aria-hidden="true" />Zwei-Faktor-Authentifizierung</h2>
        {enabled ? <Chip tone="ok">aktiv</Chip> : <Chip>aus</Chip>}
      </header>
      <div class="p-4">
        <h3 ref={headRef} tabIndex={-1} class="sr-only">{{ idle: 'Übersicht', setup: 'Einrichten', codes: 'Wiederherstellungscodes', disable: 'Deaktivieren' }[mode]}</h3>
        {error && <Alert tone="bad" class="mb-4">{error}</Alert>}
        {st?.time_synced === false && mode !== 'codes' && (
          <Alert tone="warn" class="mb-4" title="Uhrzeit des Geräts noch nicht synchron">Codes aus der App können erst geprüft werden, wenn das Gerät die Uhrzeit aus dem Internet geholt hat (meist wenige Sekunden nach dem WLAN-Verbinden).</Alert>
        )}

        {mode === 'idle' && (
          <div class="grid md:grid-cols-[1fr,auto] gap-4 items-center">
            <div class="flex gap-3">
              <div class={`w-11 h-11 rounded-2xl grid place-items-center shrink-0 ${enabled ? 'bg-ok/10 text-ok' : 'bg-raised text-muted'}`} aria-hidden="true">
                {enabled ? <ShieldCheck class="w-5 h-5" /> : <ShieldOff class="w-5 h-5" />}
              </div>
              <div>
                <div class="font-semibold">{enabled ? 'Dein Konto ist zusätzlich geschützt' : 'Zusätzlicher Schutz für dein Konto'}</div>
                <p class="text-[13px] text-muted mt-0.5 max-w-2xl">
                  {enabled
                    ? 'Bei jeder Anmeldung fragt das Gerät nach dem Passwort und einem sechsstelligen Code aus deiner Authenticator-App.'
                    : 'Zusätzlich zum Passwort fragt das Gerät nach einem Code, den eine App auf deinem Handy alle 30 Sekunden neu erzeugt – zum Beispiel Aegis, 2FAS, Google Authenticator oder ein Passwort-Manager.'}
                </p>
              </div>
            </div>
            <div class="flex flex-wrap gap-2 md:justify-end">
              {enabled ? <>
                <button class="btn" onClick={start} disabled={busy}>{busy ? <Spinner /> : <Smartphone class="w-4 h-4" />}Neues Handy einrichten</button>
                <button class="btn-danger" onClick={() => { setError(null); setMode('disable') }}><ShieldOff class="w-4 h-4" />Deaktivieren</button>
              </> : (
                <button class="btn-primary" onClick={start} disabled={busy}>{busy ? <Spinner /> : <ShieldPlus class="w-4 h-4" />}Einrichten</button>
              )}
            </div>
          </div>
        )}

        {mode === 'setup' && setup && (
          <TotpSetup setup={setup} replacing={enabled} onCancel={reset}
            onExpired={(msg) => { reset(); setError(msg) }}
            onDone={(c) => { setCodes(c); setMode('codes') }} />
        )}
        {mode === 'codes' && codes && <RecoveryCodes codes={codes} user={st?.user} onDone={reset} />}
        {mode === 'disable' && <DisableTotp onCancel={reset} onDone={() => { reset(); toast('Zwei-Faktor-Authentifizierung deaktiviert', 'ok') }} />}
      </div>
    </section>
  )
}

function TotpSetup({ setup, replacing, onCancel, onDone, onExpired }) {
  const [code, setCode] = useState('')
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState(null)
  const secret = groupSecret(setup.secret)

  const enable = async (v = code) => {
    if (busy || v.length !== 6) return
    setBusy(true); setError(null)
    try { const r = await totpEnable(v); onDone(r?.recovery_codes || []) }
    catch (err) {
      setBusy(false); setCode('')
      // pending secret lives in RAM only: after a gateway restart the setup must start over
      if (err.code === 'totp_not_pending') return onExpired(authErrorText(err.code))
      setError(formErrorText(err))
      return
    }
    setBusy(false)
  }
  return (
    <div class="grid md:grid-cols-[auto,1fr] gap-6">
      <div class="flex flex-col items-center gap-2">
        <div class="p-2 rounded-2xl bg-white shadow-sm border border-line"><QrCode text={setup.otpauth_uri} size={196} label="QR-Code zum Einrichten in der Authenticator-App" /></div>
        <div class="text-[11.5px] text-muted">Mit der App scannen</div>
      </div>
      <div class="space-y-5 min-w-0">
        {replacing && <Alert tone="info">Dein bisheriger Zugang bleibt aktiv, bis du den neuen Code bestätigt hast.</Alert>}
        <div>
          <div class="text-[13px] font-semibold flex items-center gap-2"><span class="step-dot">1</span>Authenticator-App öffnen und QR-Code scannen</div>
          <p class="text-[12.5px] text-muted mt-1 ml-7">Kein Scanner zur Hand? Gib den Schlüssel von Hand ein (Typ „zeitbasiert“).</p>
          <div class="ml-7 mt-2 flex flex-wrap items-center gap-2">
            <code class="font-mono text-[13px] tracking-wider px-3 py-1.5 rounded-lg bg-raised border border-line break-all select-all" aria-label="Geheimer Schlüssel">{secret}</code>
            <button type="button" class="btn btn-sm" onClick={async () => toast(await copyText(setup.secret) ? 'Schlüssel kopiert' : 'Kopieren nicht möglich – bitte markieren', 'ok')}><Copy class="w-3.5 h-3.5" />Kopieren</button>
          </div>
        </div>
        <div>
          <div class="text-[13px] font-semibold flex items-center gap-2"><span class="step-dot">2</span>Code aus der App eingeben</div>
          <div class="ml-7 mt-2 max-w-[340px] space-y-3">
            {error && <Alert tone="bad">{error}</Alert>}
            <CodeInput value={code} onChange={setCode} onComplete={enable} disabled={busy} autoFocus label="Sechsstelliger Code aus der App" />
            <div class="flex gap-2">
              <button class="btn-primary" disabled={busy || code.length !== 6} onClick={() => enable()}>{busy ? <Spinner /> : <ShieldCheck class="w-4 h-4" />}Aktivieren</button>
              <button class="btn" onClick={onCancel} disabled={busy}>Abbrechen</button>
            </div>
          </div>
        </div>
      </div>
    </div>
  )
}

function RecoveryCodes({ codes, user, onDone }) {
  const [saved, setSaved] = useState(false)
  const text = recoveryFile(codes, user)
  return (
    <div class="space-y-4">
      <Alert tone="ok" title="Zwei-Faktor-Authentifizierung ist aktiv">Sichere jetzt deine Wiederherstellungscodes.</Alert>
      <Alert tone="warn" title="Diese Codes werden nur jetzt angezeigt">Mit jedem Code kannst du dich einmal anmelden, wenn dein Handy weg ist oder die Uhrzeit des Geräts nicht stimmt. Speichere sie an einem sicheren Ort, zum Beispiel in deinem Passwort-Manager.</Alert>
      <ol class="grid grid-cols-2 sm:grid-cols-5 gap-2" aria-label="Wiederherstellungscodes">
        {codes.map((c, i) => (
          <li key={c} class="rounded-lg bg-raised border border-line px-3 py-2 font-mono text-[14px] tracking-wider text-center select-all">
            <span class="sr-only">{i + 1}: </span>{c}
          </li>
        ))}
      </ol>
      <div class="flex flex-wrap gap-2">
        <button class="btn" onClick={async () => toast(await copyText(codes.join('\n')) ? 'Codes kopiert' : 'Kopieren nicht möglich – bitte herunterladen', 'ok')}><Copy class="w-4 h-4" />Kopieren</button>
        <button class="btn" onClick={() => download('linkra-duo-wiederherstellungscodes.txt', text, 'text/plain')}><Download class="w-4 h-4" />Als .txt herunterladen</button>
      </div>
      <label class="flex items-center gap-2.5 text-[13.5px] cursor-pointer select-none">
        <input type="checkbox" class="check" checked={saved} onChange={e => setSaved(e.currentTarget.checked)} />
        Ich habe die Codes gesichert
      </label>
      <button class="btn-primary" disabled={!saved} onClick={onDone}><Check class="w-4 h-4" />Fertig</button>
    </div>
  )
}

function DisableTotp({ onCancel, onDone }) {
  const [password, setPassword] = useState('')
  const [code, setCode] = useState('')
  const [useRecovery, setUseRecovery] = useState(false)
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState(null)
  const ok = password && (useRecovery ? recoveryComplete(code) : code.length === 6)
  const submit = async (e) => {
    e?.preventDefault()
    if (!ok || busy) return
    setBusy(true); setError(null)
    try {
      await totpDisable(useRecovery ? { password, recovery_code: code } : { password, code })
      onDone()
    } catch (err) {
      setCode('')
      setError(err.code === 'invalid_credentials' ? (useRecovery ? 'Passwort oder Wiederherstellungscode falsch.' : 'Das Passwort stimmt nicht.')
        : err.code === 'totp_invalid' ? 'Der Code stimmt nicht. Bitte den aktuellen Code aus der App eingeben.'
          : formErrorText(err))
      setBusy(false)
    }
  }
  return (
    <form onSubmit={submit} class="max-w-md space-y-4" noValidate>
      <p class="text-[13px] text-muted">Zum Abschalten bestätige mit deinem Passwort und einem aktuellen Code. Danach reicht wieder das Passwort allein.</p>
      {error && <Alert tone="bad">{error}</Alert>}
      <Field label="Passwort">{(a) => <PasswordInput {...a} value={password} onInput={setPassword} autocomplete="current-password" disabled={busy} />}</Field>
      {useRecovery ? (
        <Field label="Wiederherstellungscode">
          {(a) => <input {...a} class="input input-lg w-full font-mono tracking-[.2em]" placeholder="xxxx-xxxx" value={code} onInput={e => setCode(cleanRecovery(e.currentTarget.value))} autocomplete="off" disabled={busy} />}
        </Field>
      ) : (
        <div class="space-y-1.5"><div class="text-[12.5px] font-medium">Code aus der App</div><CodeInput value={code} onChange={setCode} disabled={busy} label="Sechsstelliger Code aus der App" /></div>
      )}
      <button type="button" class="link-btn" onClick={() => { setCode(''); setUseRecovery(!useRecovery) }}>
        <LifeBuoy class="w-4 h-4" />{useRecovery ? 'Code aus der App verwenden' : 'Wiederherstellungscode verwenden'}
      </button>
      <div class="flex gap-2">
        <button type="submit" class="btn-danger" disabled={!ok || busy}>{busy ? <Spinner /> : <ShieldOff class="w-4 h-4" />}Deaktivieren</button>
        <button type="button" class="btn" onClick={onCancel} disabled={busy}>Abbrechen</button>
      </div>
    </form>
  )
}

function RecoveryHint() {
  return (
    <Card title="Zugang verloren?" icon={Cpu} class="lg:col-span-2">
      <div class="flex gap-3 text-[13px]">
        <div class="text-muted max-w-3xl">
          Wenn du Passwort und Wiederherstellungscodes verloren hast, setzt du nur die Zugangsdaten am Gerät selbst zurück:
          <ol class="list-decimal ml-5 mt-2 space-y-1 text-fg/90">
            <li>Gerät neu starten (Strom kurz trennen und wieder einstecken).</li>
            <li><b>Innerhalb der ersten 60 Sekunden</b> nach dem Start die <b>BOOT-Taste</b> drücken.</li>
            <li>Taste <b>10 Sekunden</b> gedrückt halten. Früheres Loslassen bricht ab.</li>
          </ol>
          <p class="mt-2">Danach zeigt die Oberfläche die Einrichtung eines neuen Kontos – ein Neustart ist nicht nötig. Geräte, WLAN und MQTT bleiben erhalten.</p>
          <p class="mt-1.5">Die Taste nicht schon beim Einschalten halten: Dann startet das Gerät im Programmiermodus und die Firmware läuft nicht.</p>
        </div>
      </div>
    </Card>
  )
}
