import { test } from 'node:test'
import assert from 'node:assert/strict'
import { hotp, totp, verify, base32Encode, base32Decode } from '../mock/totp.js'

// RFC 6238 appendix B, SHA1 key "12345678901234567890"
const KEY = base32Encode(Buffer.from('12345678901234567890'))

test('mock TOTP matches RFC 6238 test vectors', () => {
  assert.equal(totp(KEY, 59e3), '287082')
  assert.equal(totp(KEY, 1111111109e3), '081804')
  assert.equal(totp(KEY, 1234567890e3), '005924')
  assert.equal(totp(KEY, 2000000000e3), '279037')
  assert.equal(hotp(KEY, 0), '755224')                       // RFC 4226
})

test('window ±1 step, base32 round trip', () => {
  const t = 1234567890e3
  assert.ok(verify(KEY, totp(KEY, t - 30e3), t) >= 0)
  assert.ok(verify(KEY, totp(KEY, t + 30e3), t) >= 0)
  assert.equal(verify(KEY, totp(KEY, t - 90e3), t), -1)
  assert.equal(base32Decode(KEY).toString(), '12345678901234567890')
})
