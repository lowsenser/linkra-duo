import { test } from 'node:test'
import assert from 'node:assert/strict'
import { bandOf, normalizeEvent, deviceKey, dbTimeToEpoch, measurements, fmtDuration, toCSV } from '../src/lib/format.js'

test('band comes from band field, then frequency; never guessed', () => {
  assert.equal(bandOf({ band: '868', frequency: 433.92 }), '868')
  assert.equal(bandOf({ frequency: 868.3 }), '868')
  assert.equal(bandOf({ frequency: 433.67 }), '433')
  assert.equal(bandOf({}), null)
})

test('classified 868 signal stays 868 and undecoded', () => {
  const e = normalizeEvent({ model: '868 MHz Gerät', status: 'classified', band: '868', frequency: 868.3, rx_profile: 'fsk868-8k', rssi: -99 }, 1, 1000)
  assert.equal(e.band, '868'); assert.equal(e.freq, 868.3); assert.equal(e.decoded, false); assert.equal(e.profile, 'fsk868-8k')
})

test('device key matches firmware model_id, id 0 is valid', () => {
  assert.equal(deviceKey(normalizeEvent({ model: 'LaCrosse-TX29IT', id: 0, decoded: true }, 1)), 'LaCrosse-TX29IT_0')
  assert.equal(deviceKey(normalizeEvent({ model: 'Marlec-Solar', decoded: true }, 1)), 'Marlec-Solar')
})

test('db boot-relative time: current boot converts, previous boot is null', () => {
  assert.equal(dbTimeToEpoch(1000, 5000, 100000), 96000)
  assert.equal(dbTimeToEpoch(9000, 5000, 100000), null)
  assert.equal(dbTimeToEpoch(1795000000000, 5000, 100000), 1795000000000)
})

test('measurements skip metadata and format battery', () => {
  const m = measurements({ model: 'x', id: 1, temperature_C: 7.25, humidity: 79, battery_ok: 0, rssi: -80 })
  assert.deepEqual(m.map(x => x.value), ['7.3 °C', '79 %', 'LOW'])
})

test('format helpers', () => {
  assert.equal(fmtDuration(3725), '1h 2m')
  assert.equal(toCSV([{ a: 'x;y' }], [['a', r => r.a]]), 'a\n"x;y"')
})
