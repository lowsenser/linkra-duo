import { useEffect, useState } from 'preact/hooks'

/**
 * QR code as SVG path. The encoder (uqr, ~12 KB) is loaded only when a code is
 * shown (2FA setup), never on the start page. No innerHTML: modules are drawn
 * from the boolean matrix.
 */
export function QrCode({ text, size = 200, label }) {
  const [qr, setQr] = useState(null)
  const [err, setErr] = useState(false)
  useEffect(() => {
    let alive = true
    import('uqr').then(({ encode }) => { if (alive) setQr(encode(text, { ecc: 'M', border: 2 })) }).catch(() => alive && setErr(true))
    return () => { alive = false }
  }, [text])

  if (err) return <div class="text-[12.5px] text-bad">QR-Code konnte nicht geladen werden – bitte den Schlüssel abtippen.</div>
  if (!qr) return <div class="rounded-xl bg-white animate-pulse" style={{ width: size, height: size }} aria-hidden="true" />
  const n = qr.size
  let d = ''
  for (let y = 0; y < n; y++) for (let x = 0; x < n; x++) if (qr.data[y][x]) d += `M${x} ${y}h1v1h-1z`
  return (
    <svg role="img" aria-label={label} viewBox={`0 0 ${n} ${n}`} width={size} height={size} shape-rendering="crispEdges" class="rounded-xl bg-white">
      <path d={d} fill="#0b1018" />
    </svg>
  )
}
