import { signal } from '@preact/signals'

/** Parse "#page/arg" safely: a broken escape must never blank the app (review 07 N1) */
export function parseHash(hash) {
  let s = String(hash || '').replace(/^#\/?/, '')
  try { s = decodeURIComponent(s) } catch { return { page: 'overview', arg: null } }
  const [page, ...rest] = s.split('/')
  return { page: /^[a-z0-9-]{1,24}$/.test(page) ? page : 'overview', arg: rest.join('/') || null }
}

const parse = () => parseHash(typeof location !== 'undefined' ? location.hash : '')
export const route = signal(parse())
if (typeof addEventListener === 'function') addEventListener('hashchange', () => { route.value = parse() })
export const go = (page, arg) => { location.hash = arg ? `${page}/${encodeURIComponent(arg)}` : page }
