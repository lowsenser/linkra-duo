// RFC 6238 TOTP (HMAC-SHA1, 30 s, 6 digits) for the development mock and tests.
import { createHmac } from 'node:crypto'

const ALPHA = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ234567'

export function base32Decode(s) {
  const clean = String(s).toUpperCase().replace(/[^A-Z2-7]/g, '')
  let bits = 0, value = 0
  const out = []
  for (const c of clean) {
    value = (value << 5) | ALPHA.indexOf(c)
    bits += 5
    if (bits >= 8) { out.push((value >>> (bits - 8)) & 255); bits -= 8 }
  }
  return Buffer.from(out)
}

export function base32Encode(buf) {
  let bits = 0, value = 0, out = ''
  for (const b of buf) {
    value = (value << 8) | b
    bits += 8
    while (bits >= 5) { out += ALPHA[(value >>> (bits - 5)) & 31]; bits -= 5 }
  }
  if (bits > 0) out += ALPHA[(value << (5 - bits)) & 31]
  return out
}

export function hotp(secretB32, counter) {
  const key = base32Decode(secretB32)
  const msg = Buffer.alloc(8)
  msg.writeBigUInt64BE(BigInt(counter))
  const h = createHmac('sha1', key).update(msg).digest()
  const o = h[h.length - 1] & 15
  const n = ((h[o] & 0x7f) << 24) | (h[o + 1] << 16) | (h[o + 2] << 8) | h[o + 3]
  return String(n % 1e6).padStart(6, '0')
}

export const step = (ms = Date.now()) => Math.floor(ms / 1000 / 30)
export const totp = (secretB32, ms = Date.now()) => hotp(secretB32, step(ms))

/** Returns the matching time step (window ±1) or -1 */
export function verify(secretB32, code, ms = Date.now()) {
  const s = step(ms)
  for (const d of [0, -1, 1]) if (hotp(secretB32, s + d) === String(code)) return s + d
  return -1
}
