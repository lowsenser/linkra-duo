// Sign-in state and API calls (contract: docs/auth-api.md)
import { signal } from '@preact/signals'
import { api, setUnauthorizedHandler } from './http'
import { viewFor } from './authlogic'
import { startLive, stopLive, toast } from './store'
import { backoffDelay } from './scheduler'

export const authStatus = signal(null)       // last /api/auth/status answer
export const authView = signal('checking')   // checking | setup | login | app | unreachable
export const authNotice = signal(null)       // shown on the sign-in page (e.g. session expired)

function apply(status) {
  authStatus.value = status
  const v = viewFor(status)
  authView.value = v
  if (v === 'app') startLive(); else stopLive()
}

let failures = 0, retryTimer = null
/** App start and every "try again": ask the gateway who we are */
export async function checkAuth() {
  clearTimeout(retryTimer)
  try {
    const s = await api('/api/auth/status', { timeoutMs: 6000 })
    failures = 0
    apply(s)
    return s
  } catch (e) {
    if (e.status === 404) {
      // Firmware without sign-in (before the auth release): keep the UI usable
      failures = 0
      apply({ setup_required: false, authenticated: true, legacy: true })
      return authStatus.value
    }
    failures++
    if (authView.value !== 'app') authView.value = 'unreachable'
    retryTimer = setTimeout(checkAuth, backoffDelay(failures, 3000, 30000))
    throw e
  }
}

// Any 401 "unauthorized" from a data request: stop talking to the gateway,
// show the sign-in page. The hash stays, so after signing in the same page returns.
setUnauthorizedHandler(() => {
  if (authView.value !== 'app') return
  stopLive()
  authNotice.value = 'Deine Sitzung ist abgelaufen. Bitte melde dich erneut an.'
  authStatus.value = { ...(authStatus.value || {}), authenticated: false, user: undefined }
  authView.value = 'login'
})

/** POST /api/auth/login – resolves with the status, rejects with ApiError (code) */
export async function login(body) {
  const s = await api('/api/auth/login', { json: body, timeoutMs: 15000 })
  authNotice.value = null
  apply(s || await api('/api/auth/status'))
  return s
}

export async function setupAccount(username, password) {
  const s = await api('/api/auth/setup', { json: { username, password }, timeoutMs: 15000 })
  apply(s || await api('/api/auth/status'))
  toast('Konto angelegt – willkommen!', 'ok')
  return s
}

/** Sign out and drop everything this tab knows (fresh page, same address) */
async function leave(path, msg) {
  try { await api(path, { method: 'POST' }) } catch { /* session may be gone already */ }
  stopLive()
  try { sessionStorage.setItem('linkra.bye', msg) } catch { }
  location.reload()
}
export const logout = () => leave('/api/auth/logout', 'Du wurdest abgemeldet.')
export const logoutAll = () => leave('/api/auth/logout-all', 'Alle Sitzungen wurden beendet.')

export function takeByeMessage() {
  try { const m = sessionStorage.getItem('linkra.bye'); sessionStorage.removeItem('linkra.bye'); return m } catch { return null }
}

export const changePassword = (current, next) => api('/api/auth/password', { json: { current, new: next }, timeoutMs: 15000 })
export const totpSetup = () => api('/api/auth/totp/setup', { method: 'POST', timeoutMs: 10000 })
export async function totpEnable(code) {
  const r = await api('/api/auth/totp/enable', { json: { code }, timeoutMs: 10000 })
  refreshStatus()
  return r
}
export async function totpDisable(body) {
  const r = await api('/api/auth/totp/disable', { json: body, timeoutMs: 15000 })
  await refreshStatus()
  return r
}
export async function refreshStatus() {
  try { authStatus.value = await api('/api/auth/status') } catch { }
}
