import { useState } from 'preact/hooks'
import { SlidersHorizontal, Activity, Pin, RotateCw, Layers } from 'lucide-preact'
import { Card, PageHead, Segmented, Chip, Toggle, BandChip } from '../components/ui'
import { RadioCard } from '../components/RadioCard'
import { RssiChart } from '../components/RssiChart'
import { rf, actions, timeline } from '../lib/store'
import { fmtDuration, fmtMHz } from '../lib/format'

const SENS = [['0', 'Ultra'], ['1', 'Sensitiv'], ['2', 'Normal'], ['3', 'Strikt']]
const PALETTE = ['#22bef0', '#fbaa3c', '#a78bfa', '#34d378', '#f87171', '#facc15', '#2dd4bf', '#f472b6']

export function RadioPage() {
  const o = rf.value
  const [win, setWin] = useState(300)
  const [pinFor, setPinFor] = useState(60000)
  if (!o) return <PageHead title="Funk & Profile" sub="Lade …" />
  const m433 = o.modules.find(m => m.band === '433'), m868 = o.modules.find(m => m.band === '868')
  const colorOf = (id) => { const i = o.profiles.findIndex(p => p.id === id); return i < 0 ? '#8893a3' : PALETTE[i % PALETTE.length] }

  return (
    <>
      <PageHead title="Funk & Profile" sub="Beide CC1101 wechseln zeitgesteuert zwischen Empfangsprofilen, damit alle passenden rtl_433-Decoder Telegramme bekommen.">
        <div class="flex items-center gap-2 card px-3 h-9">
          <RotateCw class="w-4 h-4 text-muted" /><span class="text-[13px]">Profil-Rotation</span>
          <Toggle label="Profil-Rotation" checked={o.rotation} onChange={actions.setRotation} />
        </div>
      </PageHead>

      {o.pinned && (
        <div class="card px-4 py-2.5 mb-3 flex items-center gap-3 border-warn/50">
          <Pin class="w-4 h-4 text-warn" />
          <span class="text-[13px]">Profil <b class="font-mono">{o.pinned}</b> ist fixiert – noch {fmtDuration(o.pinnedRemainingMs / 1000)}.</span>
        </div>
      )}

      <div class="grid lg:grid-cols-2 gap-3 mb-3">
        <RadioCard m={m433} profiles={o.profiles} pinned={o.pinned} />
        <RadioCard m={m868} profiles={o.profiles} pinned={o.pinned} />
      </div>

      <Card title="Profil-Zeitleiste" icon={Layers} class="mb-3">
        {['433', '868'].map(b => <Timeline key={b} band={b} items={timeline.value[b]} colorOf={colorOf} />)}
        <div class="flex flex-wrap gap-3 mt-2">
          {o.profiles.map(p => <span key={p.id} class="inline-flex items-center gap-1.5 text-xs text-muted"><span class="w-2.5 h-2.5 rounded-sm" style={{ background: colorOf(p.id) }} />{p.id}</span>)}
        </div>
      </Card>

      <Card title="Empfangsprofile" icon={SlidersHorizontal} class="mb-3" body="p-0"
        right={<>
          <span class="text-xs text-muted">Fixieren für</span>
          <Segmented value={pinFor} onChange={setPinFor} options={[[30000, '30 s'], [60000, '1 min'], [300000, '5 min'], [600000, '10 min']]} />
        </>}>
        <div class="overflow-auto">
          <table class="tbl">
            <thead><tr>
              <th>Profil</th><th>Band</th><th>Modulation</th><th class="text-right">Frequenz</th><th class="text-right">Datenrate</th>
              <th class="text-right">BW</th><th class="text-right">Verweilzeit</th><th class="text-right">Aktiv</th><th class="text-right">Decodiert</th>
              <th class="text-right">Roh</th><th class="text-right">Decodiert/h</th><th>Letzter Treffer</th><th></th>
            </tr></thead>
            <tbody>
              {o.profiles.map(p => {
                const active = (p.band === '433' ? m433 : m868)?.profile === p.id
                const bandTotal = o.profiles.filter(x => x.band === p.band).reduce((s, x) => s + x.activeMs, 0) || 1
                const perHour = p.activeMs > 60000 ? (p.decodes / (p.activeMs / 3600000)).toFixed(0) : '—'
                return (
                  <tr key={p.id} class={active ? '[&>td]:bg-accent/5' : ''}>
                    <td>
                      <div class="flex items-center gap-2">
                        <span class="w-2.5 h-2.5 rounded-sm" style={{ background: colorOf(p.id) }} />
                        <span class="font-mono font-medium">{p.id}</span>
                        {p.home && <Chip>Home</Chip>}
                        {active && <Chip tone="accent">aktiv</Chip>}
                      </div>
                      <div class="text-[11px] text-muted mt-0.5 max-w-xs truncate" title={p.note}>{p.note}</div>
                    </td>
                    <td><BandChip band={p.band} /></td>
                    <td>{p.modulation}</td>
                    <td class="text-right num">{fmtMHz(p.freqMHz)}</td>
                    <td class="text-right num">{p.bitrateKbps?.toFixed(2)} kbit/s</td>
                    <td class="text-right num">{p.bwKHz} kHz</td>
                    <td class="text-right num">{(p.dwellMs / 1000).toFixed(0)} s</td>
                    <td class="text-right num" title={`${p.visits} Besuche`}>{fmtDuration(p.activeMs / 1000)} <span class="text-muted">({((p.activeMs / bandTotal) * 100).toFixed(0)}%)</span></td>
                    <td class={`text-right num font-semibold ${p.decodes ? 'text-ok' : 'text-muted'}`}>{p.decodes}</td>
                    <td class="text-right num text-muted">{p.unparsed}</td>
                    <td class="text-right num">{perHour}</td>
                    <td class="text-xs">{p.lastDecodeAgoS >= 0 ? `vor ${fmtDuration(p.lastDecodeAgoS)}` : '—'}</td>
                    <td class="text-right"><button class="btn btn-sm" aria-label={`Profil ${p.id} fixieren`} onClick={() => actions.pinProfile(p.id, pinFor)}><Pin class="w-3.5 h-3.5" aria-hidden="true" />Fixieren</button></td>
                  </tr>
                )
              })}
            </tbody>
          </table>
        </div>
      </Card>

      <div class="grid lg:grid-cols-2 gap-3 mb-3">
        {['433', '868'].map(b => (
          <Card key={b} title={`Pegel ${b} MHz`} icon={Activity}
            right={b === '433' && <Segmented value={win} onChange={setWin} options={[[60, '1 min'], [300, '5 min'], [900, '15 min']]} />}>
            <RssiChart bands={[b]} height={200} window={win} />
          </Card>
        ))}
      </div>

      <Card title="Empfindlichkeit (RSSI-Schwelle über Rauschen)" icon={SlidersHorizontal}>
        <div class="grid sm:grid-cols-2 gap-4">
          {[m433, m868].map(m => m && (
            <div key={m.band} class="flex items-center justify-between gap-3">
              <div><BandChip band={m.band} /> <span class="text-[13px] ml-1">Schwelle {m.threshold} dBm (Rauschen {m.noiseFloor})</span></div>
              <Segmented value={String(m.sensitivity)} onChange={(v) => actions.setSensitivity(m.band, Number(v))} options={SENS} />
            </div>
          ))}
        </div>
        <p class="text-xs text-muted mt-3">Ultra = +3 dB über Rauschen, Strikt = +12 dB. Höhere Empfindlichkeit findet schwache Sender, erzeugt aber mehr Roh-Telegramme und Decoderlast.</p>
      </Card>
    </>
  )
}

function Timeline({ band, items, colorOf }) {
  const span = 10 * 60 * 1000, now = Date.now(), from = now - span
  const segs = items.map((it, i) => ({ id: it.id, a: Math.max(from, it.t), b: i + 1 < items.length ? items[i + 1].t : now })).filter(s => s.b > from)
  return (
    <div class="flex items-center gap-3 mb-2">
      <span class="w-10"><BandChip band={band} /></span>
      <div class="relative flex-1 h-6 rounded-md bg-raised overflow-hidden">
        {segs.map((s, i) => (
          <div key={i} class="absolute inset-y-0 border-r border-panel/60" title={`${s.id} · ${new Date(s.a).toLocaleTimeString('de-DE')}`}
            style={{ left: `${((s.a - from) / span) * 100}%`, width: `${((s.b - s.a) / span) * 100}%`, background: colorOf(s.id), opacity: .85 }} />
        ))}
      </div>
      <span class="text-[11px] text-muted w-14 text-right">10 min</span>
    </div>
  )
}
