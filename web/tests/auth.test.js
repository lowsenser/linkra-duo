import { test } from 'node:test'
import assert from 'node:assert/strict'
import {
  viewFor, credentialRules, rulesOk, passwordStrength, cleanTotp, cleanRecovery, recoveryComplete,
  groupSecret, authErrorText, fmtCountdown, loginInit, loginReducer, loginBody, recoveryFile,
} from '../src/lib/authlogic.js'

test('view for /api/auth/status', () => {
  assert.equal(viewFor(null), 'unreachable')
  assert.equal(viewFor({ setup_required: true, authenticated: false }), 'setup')
  assert.equal(viewFor({ setup_required: false, authenticated: false }), 'login')
  assert.equal(viewFor({ setup_required: false, authenticated: true, user: 'admin' }), 'app')
})

test('setup rules follow the contract (user 3–32 [A-Za-z0-9._-], password 10–128, repeat)', () => {
  const r = (u, p, rep = p) => rulesOk(credentialRules({ username: u, password: p, repeat: rep }))
  assert.equal(r('admin', 'abcdefghij'), true)
  assert.equal(r('ab', 'abcdefghij'), false)
  assert.equal(r('a'.repeat(33), 'abcdefghij'), false)
  assert.equal(r('tim.o_r-1', 'abcdefghij'), true)
  assert.equal(r('tim o', 'abcdefghij'), false)
  assert.equal(r('Tim@home', 'abcdefghij'), false)
  assert.equal(r('admin', 'abcdefghi'), false)
  assert.equal(r('admin', 'x'.repeat(129)), false)
  assert.equal(r('admin', 'abcdefghij', 'abcdefghiJ'), false)
  // password form without user name
  assert.equal(rulesOk(credentialRules({ password: 'abcdefghij', repeat: 'abcdefghij' }, { withUser: false })), true)
})

test('password strength: length dominates, common words and repeats weaken', () => {
  assert.equal(passwordStrength('').score, 0)
  assert.equal(passwordStrength('abc').score, 1)
  assert.equal(passwordStrength('Passwort123').score <= 2, true)
  assert.equal(passwordStrength('aaaaaaaaaaaa').score <= 2, true)
  assert.ok(passwordStrength('Kaffee-Tasse-Funk-73').score >= 3)
  assert.equal(passwordStrength('vL9#qT2m!Rx8@Kp4').score, 4)
  assert.equal(passwordStrength('Xy7!').score, 1)                          // short is always weak
})

test('code input cleaning: paste of formatted codes', () => {
  assert.equal(cleanTotp('123 456'), '123456')
  assert.equal(cleanTotp(' 123-456 \n'), '123456')
  assert.equal(cleanTotp('12345678'), '123456')
  assert.equal(cleanTotp('ab12'), '12')
  assert.equal(cleanRecovery('K7MP3XQ9'), 'k7mp-3xq9')
  assert.equal(cleanRecovery('k7mp-3x'), 'k7mp-3x')
  assert.equal(cleanRecovery('k7m'), 'k7m')
  assert.equal(recoveryComplete('k7mp-3xq9'), true)
  assert.equal(recoveryComplete('k7mp-3xq'), false)
  assert.equal(groupSecret('JBSWY3DPEHPK3PXP'), 'JBSW Y3DP EHPK 3PXP')
})

test('error texts', () => {
  assert.match(authErrorText('invalid_credentials'), /Benutzername oder Passwort falsch/)
  assert.match(authErrorText('time_not_synced'), /Uhrzeit/)
  assert.match(authErrorText('time_not_synced'), /Wiederherstellungscode/)
  assert.equal(authErrorText('whatever', 'Text vom Gerät'), 'Text vom Gerät')
  assert.equal(authErrorText(null), 'Unbekannter Fehler.')
  assert.equal(fmtCountdown(27.2), '0:28')
  assert.equal(fmtCountdown(125), '2:05')
  assert.equal(fmtCountdown(-3), '0:00')
})

const run = (events, s = loginInit()) => events.reduce(loginReducer, s)

test('login: password only', () => {
  const s = run([{ type: 'submit' }, { type: 'success' }])
  assert.equal(s.done, true); assert.equal(s.busy, false)
})

test('login: totp_required moves to the code step without an error', () => {
  const s = run([{ type: 'submit' }, { type: 'fail', code: 'totp_required', now: 0 }])
  assert.equal(s.step, 'second'); assert.equal(s.error, null); assert.equal(s.busy, false)
  assert.deepEqual(loginBody(s, { username: ' admin ', password: 'pw', code: '123 456' }), { username: 'admin', password: 'pw', totp: '123456' })
})

test('login: wrong code stays on the code step with an error', () => {
  const s = run([{ type: 'fail', code: 'totp_required', now: 0 }, { type: 'submit' }, { type: 'fail', code: 'totp_invalid', now: 0 }])
  assert.equal(s.step, 'second'); assert.equal(s.error.code, 'totp_invalid')
})

test('login: time_not_synced switches to the recovery code', () => {
  const s = run([{ type: 'fail', code: 'totp_required', now: 0 }, { type: 'fail', code: 'time_not_synced', now: 0 }])
  assert.equal(s.step, 'second'); assert.equal(s.useRecovery, true); assert.equal(s.error.code, 'time_not_synced')
  assert.deepEqual(loginBody(s, { username: 'a', password: 'p', code: 'K7MP3XQ9' }), { username: 'a', password: 'p', recovery_code: 'k7mp-3xq9' })
})

test('login: invalid_credentials returns to the password step; with recovery it is a code error', () => {
  let s = run([{ type: 'fail', code: 'totp_required', now: 0 }, { type: 'fail', code: 'invalid_credentials', now: 0 }])
  assert.equal(s.step, 'credentials'); assert.equal(s.error.code, 'invalid_credentials')
  s = run([{ type: 'fail', code: 'totp_required', now: 0 }, { type: 'recovery', on: true }, { type: 'fail', code: 'invalid_credentials', now: 0 }])
  assert.equal(s.step, 'second'); assert.equal(s.error.code, 'recovery_invalid')
})

test('login: locked counts down from retry_after_s and unlocks', () => {
  let s = run([{ type: 'submit' }, { type: 'fail', code: 'locked', retryAfter: 30, now: 1000 }])
  assert.equal(s.lockedUntil, 31000); assert.equal(s.error.code, 'locked')
  s = loginReducer(s, { type: 'unlock', now: 20000 })
  assert.equal(s.lockedUntil, 31000)
  s = loginReducer(s, { type: 'unlock', now: 31000 })
  assert.equal(s.lockedUntil, 0); assert.equal(s.error, null)
  // back keeps an active lock
  s = run([{ type: 'fail', code: 'locked', retryAfter: 60, now: 0 }, { type: 'back' }])
  assert.equal(s.lockedUntil, 60000)
})

test('login: back resets the second step', () => {
  const s = run([{ type: 'fail', code: 'totp_required', now: 0 }, { type: 'recovery', on: true }, { type: 'back' }])
  assert.equal(s.step, 'credentials'); assert.equal(s.useRecovery, false)
})

test('recovery codes file', () => {
  const t = recoveryFile(['aaaa-bbbb', 'cccc-dddd'], 'admin', new Date(0))
  assert.match(t, /Konto: admin/)
  assert.match(t, / 1\. aaaa-bbbb/)
  assert.match(t, / 2\. cccc-dddd/)
})
