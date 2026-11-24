// HTTP layer for the gateway API (contract: docs/auth-api.md).
// Pure apart from fetch/AbortController, which can be injected for tests.

/** Error with the machine readable code from {"error":{"code","message"}} */
export class ApiError extends Error {
  constructor(message, { status = 0, code = null, retryAfter = null } = {}) {
    super(message)
    this.name = 'ApiError'
    this.status = status
    this.code = code
    this.retryAfter = retryAfter
  }
}

/** Methods that must carry the CSRF header (everything except GET/HEAD) */
export const needsCsrf = (method) => !/^(GET|HEAD)$/i.test(method || 'GET')

/**
 * Turn a non-OK response body into an ApiError. Accepts the contract format,
 * the legacy {"error":"text"} format and plain text.
 */
export function toApiError(status, data) {
  const e = data && typeof data === 'object' ? data.error : null
  if (e && typeof e === 'object') {
    return new ApiError(e.message || fallbackText(status), {
      status, code: e.code || null,
      retryAfter: Number.isFinite(e.retry_after_s) ? e.retry_after_s : null,
    })
  }
  if (typeof e === 'string' && e) return new ApiError(e, { status })
  if (typeof data === 'string' && data && data.length < 200 && !/</.test(data)) return new ApiError(data, { status })
  return new ApiError(fallbackText(status), { status, code: status === 401 ? 'unauthorized' : null })
}

export function fallbackText(status) {
  if (status === 401) return 'Anmeldung erforderlich'
  if (status === 403) return 'Ungültige Anfrage'
  if (status === 404) return 'Funktion auf diesem Gerät nicht vorhanden'
  if (status === 429) return 'Zu viele Versuche'
  if (status >= 500) return 'Fehler im Gerät'
  return `Fehler ${status}`
}

let onUnauthorized = null
/** Called once per 401 "unauthorized" (session missing or expired) */
export function setUnauthorizedHandler(fn) { onUnauthorized = fn }

/** Routes whose 401 means "wrong input", not "session gone" */
const AUTH_INPUT = /^\/api\/auth\/(login|setup|status)\b/

export function createApi(fetchImpl = (...a) => fetch(...a)) {
  return async function api(path, opts = {}) {
    const { json, timeoutMs, ...rest } = opts
    const init = { cache: 'no-store', credentials: 'same-origin', ...rest }
    const headers = { ...(rest.headers || {}) }
    if (json !== undefined) {
      init.method = init.method || 'POST'
      headers['Content-Type'] = 'application/json'
      init.body = JSON.stringify(json)
    }
    if (needsCsrf(init.method)) headers['X-Linkra'] = '1'
    init.headers = headers
    // Hard timeout: a request the gateway dropped must not hold one of the
    // browser's 6 connections per host (that froze the UI until a reload).
    const ctrl = new AbortController()
    const timer = setTimeout(() => ctrl.abort(), timeoutMs || 4000)
    init.signal = ctrl.signal
    let r, text
    try {
      r = await fetchImpl(path, init)
      text = await r.text()
    } catch (e) {
      throw new ApiError(e?.name === 'AbortError' ? 'Zeitüberschreitung – das Gerät antwortet nicht' : 'Gerät nicht erreichbar', { code: 'network' })
    } finally { clearTimeout(timer) }
    let data = null
    try { data = text ? JSON.parse(text) : null } catch { data = text }
    if (!r.ok) {
      const err = toApiError(r.status, data)
      if (err.retryAfter == null) { const h = Number(r.headers?.get?.('Retry-After')); if (h > 0) err.retryAfter = h }
      if (r.status === 401 && err.code === 'unauthorized' && !AUTH_INPUT.test(path) && onUnauthorized) onUnauthorized(err)
      throw err
    }
    return data
  }
}

export const api = createApi()
