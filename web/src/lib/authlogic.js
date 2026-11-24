// Pure logic for sign-in, setup and 2FA (contract: docs/auth-api.md).
// No DOM, no fetch: unit-tested in tests/auth.test.js.

export const USER_RE = /^[A-Za-z0-9._-]{3,32}$/
export const PW_MIN = 10, PW_MAX = 128

/** Which screen the app shows for a /api/auth/status answer */
export function viewFor(status) {
  if (!status) return 'unreachable'
  if (status.setup_required) return 'setup'
  if (!status.authenticated) return 'login'
  return 'app'
}

/** Rule checklist for the setup and password forms (shown live as the user types) */
export function credentialRules({ username, password, repeat }, { withUser = true } = {}) {
  const rules = []
  if (withUser) {
    const u = username || ''
    rules.push({ id: 'user_len', ok: u.length >= 3 && u.length <= 32, text: 'Benutzername 3 bis 32 Zeichen' })
    rules.push({ id: 'user_chars', ok: u.length > 0 && /^[A-Za-z0-9._-]+$/.test(u), text: 'Nur Buchstaben, Ziffern, Punkt, Unterstrich und Bindestrich' })
  }
  const p = password || ''
  rules.push({ id: 'pw_len', ok: p.length >= PW_MIN && p.length <= PW_MAX, text: `Passwort mindestens ${PW_MIN} Zeichen` })
  rules.push({ id: 'pw_repeat', ok: p.length > 0 && p === repeat, text: 'Wiederholung stimmt überein' })
  return rules
}
export const rulesOk = (rules) => rules.every(r => r.ok)

const COMMON = ['passwort', 'password', 'linkra', 'admin', 'qwertz', 'qwerty', '123456', 'abcdef', 'letmein', 'hallo', 'geheim']

/**
 * Rough password strength 0..4 (no dictionary download: must stay tiny).
 * Length dominates; character variety and missing patterns add a little.
 */
export function passwordStrength(pw = '') {
  if (!pw) return { score: 0, label: '' }
  const lower = pw.toLowerCase()
  let classes = 0
  if (/[a-z]/.test(pw)) classes++
  if (/[A-Z]/.test(pw)) classes++
  if (/[0-9]/.test(pw)) classes++
  if (/[^A-Za-z0-9]/.test(pw)) classes++
  const unique = new Set(pw).size
  let bits = pw.length * Math.log2(Math.max(10, [0, 26, 52, 62, 90][classes]))
  if (unique < pw.length / 2) bits *= 0.6                     // aaaa, abab …
  if (COMMON.some(w => lower.includes(w))) bits *= 0.5
  if (/(0123|1234|2345|3456|4567|5678|6789|abcd|bcde|cdef)/.test(lower)) bits *= 0.8
  let score = bits < 35 ? 1 : bits < 55 ? 2 : bits < 75 ? 3 : 4
  if (pw.length < PW_MIN) score = Math.min(score, 1)
  return { score, label: ['', 'schwach', 'mittel', 'gut', 'stark'][score] }
}

/** "123 456", "123-456", pasted text → "123456" (max 6 digits) */
export const cleanTotp = (s) => String(s || '').replace(/\D/g, '').slice(0, 6)

/** Recovery code as the user types: lower case, a-z0-9, dash after 4 chars */
export function cleanRecovery(s) {
  const raw = String(s || '').toLowerCase().replace(/[^a-z0-9]/g, '').slice(0, 8)
  return raw.length > 4 ? `${raw.slice(0, 4)}-${raw.slice(4)}` : raw
}
export const recoveryComplete = (s) => /^[a-z0-9]{4}-[a-z0-9]{4}$/.test(s)

/** Base32 secret in groups of four for typing it into an app */
export const groupSecret = (s) => String(s || '').replace(/\s/g, '').replace(/(.{4})/g, '$1 ').trim()

/** User-facing text for API errors in the sign-in flows */
export function authErrorText(code, message) {
  switch (code) {
    case 'invalid_credentials': return 'Benutzername oder Passwort falsch.'
    case 'totp_invalid': return 'Der Code stimmt nicht. Bitte den aktuellen Code aus der App eingeben.'
    case 'totp_required': return 'Bitte den Code aus deiner Authenticator-App eingeben.'
    case 'time_not_synced': return 'Die Uhrzeit des Geräts ist noch nicht synchron, deshalb kann es Codes aus der App gerade nicht prüfen. Melde dich mit einem Wiederherstellungscode an oder versuche es in einer Minute erneut.'
    case 'locked': return 'Zu viele Fehlversuche. Die Anmeldung ist kurz gesperrt.'
    case 'exists': return 'Auf diesem Gerät gibt es bereits ein Konto. Bitte anmelden.'
    case 'setup_required': return 'Auf diesem Gerät ist noch kein Konto eingerichtet.'
    case 'totp_not_pending': return 'Die Einrichtung ist abgelaufen (zum Beispiel nach einem Neustart des Geräts). Bitte starte sie neu.'
    case 'storage': return 'Das Gerät konnte die Änderung nicht speichern. Bitte erneut versuchen.'
    case 'invalid': return message || 'Die Eingabe erfüllt die Regeln nicht.'
    case 'csrf': return 'Die Anfrage wurde aus Sicherheitsgründen abgelehnt. Bitte die Seite neu laden.'
    case 'network': return message || 'Gerät nicht erreichbar.'
    case 'unauthorized': return 'Deine Sitzung ist abgelaufen. Bitte erneut anmelden.'
    default: return message || 'Unbekannter Fehler.'
  }
}

/** Error text for account forms; a lock names the waiting time */
export function formErrorText(err) {
  if (err?.code === 'locked') {
    const s = Math.max(1, Math.ceil(err.retryAfter || 30))
    return `Zu viele Fehlversuche. Bitte ${s < 120 ? `${s} Sekunden` : `${Math.ceil(s / 60)} Minuten`} warten.`
  }
  return authErrorText(err?.code, err?.message)
}

/** Seconds → "0:27" / "2:05" */
export const fmtCountdown = (s) => { const t = Math.max(0, Math.ceil(s)); return `${Math.floor(t / 60)}:${String(t % 60).padStart(2, '0')}` }

// ---------- Sign-in state machine ----------
// step: 'credentials' → (totp_required) → 'second'
// second factor: TOTP code or recovery code (useRecovery)
export const loginInit = () => ({ step: 'credentials', useRecovery: false, busy: false, error: null, lockedUntil: 0, done: false })

export function loginReducer(s, ev) {
  switch (ev.type) {
    case 'submit': return { ...s, busy: true, error: null }
    case 'success': return { ...s, busy: false, error: null, done: true }
    case 'recovery': return { ...s, useRecovery: !!ev.on, error: null }
    case 'back': return { ...loginInit(), lockedUntil: s.lockedUntil }
    case 'unlock': return s.lockedUntil && ev.now >= s.lockedUntil ? { ...s, lockedUntil: 0, error: s.error?.code === 'locked' ? null : s.error } : s
    case 'fail': {
      const base = { ...s, busy: false }
      const err = { code: ev.code, text: authErrorText(ev.code, ev.message) }
      switch (ev.code) {
        case 'totp_required':
          // first time: just move on to the code step; on the code step a missing code is an error
          return s.step === 'second' ? { ...base, error: err } : { ...base, step: 'second', error: null }
        case 'totp_invalid':
          return { ...base, step: 'second', error: err }
        case 'time_not_synced':
          return { ...base, step: 'second', useRecovery: true, error: err }
        case 'invalid_credentials':
          // a wrong recovery code is reported as invalid_credentials by some firmware: stay on the step
          return s.step === 'second' && s.useRecovery
            ? { ...base, error: { code: 'recovery_invalid', text: 'Der Wiederherstellungscode ist ungültig oder wurde schon benutzt.' } }
            : { ...base, step: 'credentials', useRecovery: false, error: err }
        case 'locked':
          return { ...base, error: err, lockedUntil: ev.now + Math.max(1, ev.retryAfter || 30) * 1000 }
        default:
          return { ...base, error: err }
      }
    }
    default: return s
  }
}

/** Login body for the current step */
export function loginBody(s, { username, password, code }) {
  const body = { username: username.trim(), password }
  if (s.step === 'second') {
    if (s.useRecovery) body.recovery_code = cleanRecovery(code)
    else body.totp = cleanTotp(code)
  }
  return body
}

/** Text file with the recovery codes (download) */
export function recoveryFile(codes, user, date = new Date()) {
  return [
    'Linkra Duo – Wiederherstellungscodes',
    `Konto: ${user || ''}`,
    `Erstellt: ${date.toLocaleString('de-DE')}`,
    '',
    'Jeder Code funktioniert genau einmal, wenn du keinen Zugriff auf deine',
    'Authenticator-App hast. Bewahre diese Datei sicher auf.',
    '',
    ...codes.map((c, i) => `${String(i + 1).padStart(2, ' ')}. ${c}`),
    '',
  ].join('\n')
}
