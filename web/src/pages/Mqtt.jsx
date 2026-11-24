import { useEffect, useState } from 'preact/hooks'
import { Share2, Server, Send, Save, Eye, EyeOff, X } from 'lucide-preact'
import { Card, Toggle, KV, Chip, Dot, Empty } from '../components/ui'
import { mqtt, deviceList, actions, pollMqtt } from '../lib/store'
import { go } from '../lib/router'
import { fmtDuration } from '../lib/format'

function Field({ label, hint, children }) {
  return (
    <label class="block">
      <div class="label mb-1">{label}</div>
      {children}
      {hint && <div class="text-[11.5px] text-muted mt-1">{hint}</div>}
    </label>
  )
}

function formFrom(m) {
  return {
    enabled: !!m?.enabled, host: m?.host || '', port: m?.port || 1883, tls: !!m?.tls,
    user: m?.user || '', pass: '', base_topic: m?.base_topic || 'linkra',
    discovery_prefix: m?.discovery_prefix || 'homeassistant',
    expire_min: Math.round((m?.expire_after ?? 3600) / 60),
  }
}

function Status({ m }) {
  const state = !m ? ['muted', '—'] : !m.enabled ? ['muted', 'aus'] : m.connected ? ['ok', 'verbunden'] : ['warn', 'nicht verbunden']
  return (
    <div class="grid grid-cols-2 lg:grid-cols-4 gap-3 mb-3">
      <div class="card px-4 py-3">
        <div class="label">Broker</div>
        <div class="mt-0.5 font-semibold flex items-center gap-2"><Dot tone={state[0]} live={m?.connected} />{state[1]}</div>
        {m?.enabled && !m.connected && m.last_error && <div class="text-[11.5px] text-warn mt-1">{m.last_error}</div>}
      </div>
      <div class="card px-4 py-3">
        <div class="label">Verbunden seit</div>
        <div class="mt-0.5 font-semibold num">{m?.connected ? fmtDuration(m.connected_for_s) : '—'}</div>
      </div>
      <div class="card px-4 py-3">
        <div class="label">Gesendet</div>
        <div class="mt-0.5 font-semibold num">{m?.published != null ? m.published.toLocaleString('de-DE') : '—'}</div>
      </div>
      <div class="card px-4 py-3">
        <div class="label">Verworfen / Fehler</div>
        <div class={`mt-0.5 font-semibold num ${m && (m.dropped || m.failed) ? 'text-warn' : ''}`}>{m ? `${m.dropped} / ${m.failed}` : '—'}</div>
      </div>
    </div>
  )
}

export function MqttPanel() {
  const m = mqtt.value
  const [f, setF] = useState(() => formFrom(m))
  const [dirty, setDirty] = useState(false)
  const [showPass, setShowPass] = useState(false)
  const [busy, setBusy] = useState(false)

  // Take over server values until the user starts editing
  useEffect(() => { if (!dirty) setF(formFrom(m)) }, [m, dirty])
  // the scheduler polls every 5 s while this tab is open (store.js mqttEvery)
  useEffect(() => { pollMqtt() }, [])

  const set = (k) => (e) => { setDirty(true); setF({ ...f, [k]: e?.target ? (e.target.type === 'number' ? Number(e.target.value) : e.target.value) : e }) }

  const save = async (e) => {
    e?.preventDefault()
    setBusy(true)
    try {
      await actions.saveMqtt({
        enabled: f.enabled, host: f.host.trim(), port: Number(f.port) || 1883, tls: f.tls,
        user: f.user.trim(), pass: f.pass, base_topic: f.base_topic.trim(),
        discovery_prefix: f.discovery_prefix.trim(), expire_after: Math.max(0, Math.round(Number(f.expire_min) * 60)),
      })
      setDirty(false)
    } catch { } finally { setBusy(false) }
  }

  const released = deviceList.value.filter(d => d.mqttEnabled)
  const candidates = deviceList.value.filter(d => d.decoded && d.persisted && !d.mqttEnabled)

  return (
    <>
      <Status m={m} />
      <div class="grid lg:grid-cols-5 gap-3">
        <Card title="Broker" icon={Server} class="lg:col-span-3">
          <form onSubmit={save} class="space-y-4">
            <div class="flex items-center justify-between gap-4">
              <div>
                <div class="font-medium">MQTT aktiv</div>
                <div class="text-[12.5px] text-muted">Freigegebene Sensoren erscheinen in Home Assistant automatisch als Geräte.</div>
              </div>
              <Toggle label="MQTT aktiv" checked={f.enabled} onChange={(v) => { setDirty(true); setF({ ...f, enabled: v }) }} />
            </div>
            <div class="grid sm:grid-cols-[1fr_7rem] gap-3">
              <Field label="Adresse" hint="IP oder Name des Brokers, z. B. die Adresse von Home Assistant">
                <input class="input w-full" required={f.enabled} placeholder="192.168.1.10" value={f.host} onInput={set('host')} autocomplete="off" spellcheck={false} />
              </Field>
              <Field label="Port">
                <input class="input w-full num" type="number" min="1" max="65535" value={f.port} onInput={set('port')} />
              </Field>
            </div>
            <div class="grid sm:grid-cols-2 gap-3">
              <Field label="Benutzer">
                <input class="input w-full" value={f.user} onInput={set('user')} autocomplete="username" spellcheck={false} />
              </Field>
              <Field label="Passwort" hint={m?.has_password && !f.pass ? 'Gespeichert – leer lassen, um es zu behalten' : null}>
                <div class="relative">
                  <input class="input w-full pr-9" type={showPass ? 'text' : 'password'} value={f.pass} onInput={set('pass')}
                    placeholder={m?.has_password ? '••••••••' : ''} autocomplete="new-password" />
                  <button type="button" class="absolute right-2 top-1/2 -translate-y-1/2 text-muted hover:text-fg" aria-label={showPass ? 'Passwort verbergen' : 'Passwort zeigen'}
                    onClick={() => setShowPass(!showPass)}>{showPass ? <EyeOff class="w-4 h-4" /> : <Eye class="w-4 h-4" />}</button>
                </div>
              </Field>
            </div>
            <details class="group">
              <summary class="cursor-pointer text-[13px] text-muted hover:text-fg select-none">Erweitert</summary>
              <div class="grid sm:grid-cols-3 gap-3 mt-3">
                <Field label="Topic-Präfix"><input class="input w-full font-mono" value={f.base_topic} onInput={set('base_topic')} spellcheck={false} /></Field>
                <Field label="Discovery-Präfix"><input class="input w-full font-mono" value={f.discovery_prefix} onInput={set('discovery_prefix')} spellcheck={false} /></Field>
                <Field label="Nicht verfügbar nach" hint="Minuten ohne Telegramm, 0 = nie">
                  <input class="input w-full num" type="number" min="0" max="10080" value={f.expire_min} onInput={set('expire_min')} />
                </Field>
              </div>
              <div class="flex items-center justify-between gap-4 mt-3">
                <div>
                  <div class="font-medium">TLS</div>
                  <div class="text-[12.5px] text-muted">Verschlüsselte Verbindung (meist Port 8883, Zertifikat muss gültig sein)</div>
                </div>
                <Toggle label="TLS" checked={f.tls} onChange={(v) => { setDirty(true); setF({ ...f, tls: v, port: v && f.port === 1883 ? 8883 : !v && f.port === 8883 ? 1883 : f.port }) }} />
              </div>
            </details>
            <div class="flex items-center gap-2 pt-1">
              <button class="btn-primary" type="submit" disabled={busy || !dirty}><Save class="w-4 h-4" />Speichern und verbinden</button>
              {dirty && <button type="button" class="btn" onClick={() => { setDirty(false); setF(formFrom(m)) }}>Verwerfen</button>}
            </div>
          </form>
        </Card>

        <Card title="Per MQTT gesendet" icon={Send} class="lg:col-span-2" body="p-0">
          {released.length === 0 ? (
            <Empty icon={Share2} title="Noch keine Sensoren freigegeben">Unten oder unter „Geräte“ einen gespeicherten Sensor wählen.</Empty>
          ) : (
            <ul class="divide-y divide-line/60">
              {released.map(d => (
                <li key={d.key} class="flex items-center justify-between gap-3 px-4 py-2.5">
                  <button class="min-w-0 text-left hover:text-accent" onClick={() => go('devices', d.key)}>
                    <div class="font-medium truncate">{d.model}{d.devId != null && <span class="text-muted"> #{d.devId}</span>}</div>
                    <div class="text-[11.5px] text-muted font-mono truncate">{m?.base_topic || 'linkra'}/sensor/…</div>
                  </button>
                  <button class="btn btn-sm" title="Nicht mehr per MQTT senden" aria-label={`${d.key} nicht mehr per MQTT senden`} onClick={() => actions.toggleMqtt(d)}><X class="w-3.5 h-3.5" /></button>
                </li>
              ))}
            </ul>
          )}
          {candidates.length > 0 && (
            <div class="border-t border-line px-4 py-3">
              <div class="label mb-2">Gespeichert, noch nicht freigegeben</div>
              <div class="flex flex-wrap gap-2">
                {candidates.map(d => <button key={d.key} class="btn btn-sm" onClick={() => actions.toggleMqtt(d)}><Send class="w-3.5 h-3.5" />{d.model}{d.devId != null ? ` #${d.devId}` : ''}</button>)}
              </div>
            </div>
          )}
          {m?.node_id && <div class="border-t border-line px-4 py-3"><KV rows={[['Gerätekennung', <span class="font-mono text-xs">{m.node_id}</span>], ['In Warteschlange', m.queue]]} /></div>}
        </Card>
      </div>
      {m?.enabled && m?.connected && (
        <div class="mt-3 text-[12.5px] text-muted flex items-center gap-2"><Chip tone="ok">Tipp</Chip>In Home Assistant erscheinen die Sensoren unter Einstellungen → Geräte & Dienste → MQTT.</div>
      )}
    </>
  )
}
