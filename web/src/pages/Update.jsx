import { useEffect, useState } from 'preact/hooks'
import { Download, RefreshCw, ExternalLink, ShieldCheck } from 'lucide-preact'
import { Card, Toggle, Chip, Progress } from '../components/ui'
import { update, pollUpdate, api, act } from '../lib/store'
import { fmtBytes } from '../lib/format'

const STATE = {
  idle: ['muted', 'noch nicht geprüft'],
  checking: ['muted', 'wird geprüft …'],
  up_to_date: ['ok', 'aktuell'],
  available: ['warn', 'Update verfügbar'],
  downloading: ['warn', 'wird geladen …'],
  installing: ['warn', 'wird installiert …'],
  error: ['bad', 'Prüfung fehlgeschlagen'],
}

function ago(s) {
  if (s == null) return null
  if (s < 90) return 'gerade eben'
  if (s < 5400) return `vor ${Math.round(s / 60)} min`
  if (s < 129600) return `vor ${Math.round(s / 3600)} h`
  return `vor ${Math.round(s / 86400)} Tagen`
}

export function UpdatePanel() {
  const u = update.value
  const [busy, setBusy] = useState(false)
  useEffect(() => { pollUpdate() }, [])

  const run = async (path, msg) => {
    setBusy(true)
    try { update.value = await act(api(path, { method: 'POST', timeoutMs: 10000 }), msg) } catch { } finally { setBusy(false) }
  }
  const check = () => run('/api/update/check', null)
  const install = () => {
    const l = u?.latest
    if (!l) return
    if (!confirm(`Version ${l.version} installieren? Das Gerät lädt die Firmware von GitHub, prüft die Prüfsumme und startet neu (ca. 1–2 Minuten). Läuft die neue Version nicht stabil, kehrt es automatisch zur bisherigen zurück.`)) return
    run('/api/update/install', 'Update gestartet')
  }
  const setOpt = (k) => async (v) => {
    try { update.value = await act(api('/api/update/settings', { json: { [k]: v } }), 'Gespeichert') } catch { }
  }

  const [tone, label] = STATE[u?.state] || STATE.idle
  const running = ['downloading', 'installing'].includes(u?.state)
  const l = u?.latest

  return (
    <Card title="Firmware-Updates" icon={Download}>
      <div class="flex flex-wrap items-center gap-x-4 gap-y-2">
        <div>
          <div class="label">Installiert</div>
          <div class="font-semibold num">{u?.installed || '—'}</div>
        </div>
        <Chip tone={tone}>{label}</Chip>
        <div class="text-[12.5px] text-muted">{u?.checked_ago_s != null && `zuletzt geprüft ${ago(u.checked_ago_s)}`}</div>
        <button class="btn btn-sm ml-auto" onClick={check} disabled={busy || running || u?.state === 'checking'}>
          <RefreshCw class={`w-3.5 h-3.5 ${u?.state === 'checking' ? 'animate-spin' : ''}`} />Jetzt prüfen
        </button>
      </div>

      {u?.error && <div class="mt-3 text-[12.5px] text-warn" role="status">{u.error}</div>}

      {l && (
        <div class="mt-4 rounded-xl border border-line bg-raised/40 p-4">
          <div class="flex flex-wrap items-center gap-2">
            <div class="font-semibold">{l.name || `Version ${l.version}`}</div>
            <Chip tone="warn">{l.version}</Chip>
            {l.size > 0 && <span class="text-[12px] text-muted num">{fmtBytes(l.size)}</span>}
            {l.url && <a class="text-[12.5px] text-accent inline-flex items-center gap-1 ml-auto" href={l.url} target="_blank" rel="noopener noreferrer">Versionshinweise<ExternalLink class="w-3.5 h-3.5" /></a>}
          </div>
          {l.notes && <pre class="mt-2 text-[12.5px] text-muted whitespace-pre-wrap font-sans max-h-40 overflow-auto">{l.notes}</pre>}
          {running ? (
            <div class="mt-3" role="status">
              <div class="flex justify-between text-[12.5px] mb-1">
                <span>{u.state === 'installing' ? 'Wird installiert, Gerät startet gleich neu …' : 'Firmware wird geladen …'}</span>
                <span class="num">{u.progress ?? 0} %</span>
              </div>
              <Progress value={u.progress ?? 0} max={100} />
            </div>
          ) : (
            <button class="btn-primary mt-3" onClick={install} disabled={busy}><Download class="w-4 h-4" />Jetzt installieren</button>
          )}
          <div class="mt-3 text-[11.5px] text-muted flex items-start gap-1.5">
            <ShieldCheck class="w-3.5 h-3.5 shrink-0 mt-px" />
            Download per HTTPS von GitHub, SHA-256-Prüfsumme wird vor dem Umschalten geprüft. Startet die neue Version nicht stabil, kehrt das Gerät zur bisherigen zurück.
          </div>
        </div>
      )}

      <div class="mt-4 divide-y divide-line/60">
        <div class="flex items-center justify-between gap-6 py-3">
          <div>
            <div class="font-medium" id="upd-auto">Täglich nach Updates suchen</div>
            <div class="text-[12.5px] text-muted mt-0.5 max-w-xl">Fragt einmal am Tag die Release-Liste auf GitHub ab ({u?.repo || '…'}). Dabei sieht GitHub die IP-Adresse deines Anschlusses, sonst werden keine Daten gesendet. Installiert wird nie automatisch.</div>
          </div>
          <Toggle labelledby="upd-auto" checked={!!u?.auto_check} disabled={!u} onChange={setOpt('auto_check')} />
        </div>
        <div class="flex items-center justify-between gap-6 py-3">
          <div>
            <div class="font-medium" id="upd-ha">Installation aus Home Assistant erlauben</div>
            <div class="text-[12.5px] text-muted mt-0.5 max-w-xl">Home Assistant zeigt verfügbare Updates immer an (per MQTT). Mit dieser Option erscheint dort auch „Installieren“ – jeder mit Schreibrechten auf deinem MQTT-Broker kann das Update dann starten.</div>
          </div>
          <Toggle labelledby="upd-ha" checked={!!u?.ha_install} disabled={!u} onChange={setOpt('ha_install')} />
        </div>
      </div>
    </Card>
  )
}
