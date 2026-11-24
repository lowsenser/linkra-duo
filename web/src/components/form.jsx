import { useEffect, useRef, useState } from 'preact/hooks'
import { Eye, EyeOff, Check, Circle, AlertTriangle, Info, CheckCircle2 } from 'lucide-preact'
import { passwordStrength, cleanTotp } from '../lib/authlogic'

let idSeq = 0
const useId = (p) => { const r = useRef(null); if (!r.current) r.current = `${p}-${++idSeq}`; return r.current }

/** Label + control + hint/error, wired with for/aria-describedby */
export function Field({ label, hint, error, children, id: given }) {
  const auto = useId('f')
  const id = given || auto
  const hintId = hint || error ? `${id}-d` : undefined
  return (
    <div class="space-y-1.5">
      <label for={id} class="block text-[12.5px] font-medium text-fg/90">{label}</label>
      {children({ id, 'aria-describedby': hintId, 'aria-invalid': error ? 'true' : undefined })}
      {(error || hint) && <div id={hintId} class={`text-[12px] ${error ? 'text-bad' : 'text-muted'}`}>{error || hint}</div>}
    </div>
  )
}

export function PasswordInput({ value, onInput, autocomplete = 'current-password', inputRef, ...rest }) {
  const [show, setShow] = useState(false)
  return (
    <div class="relative">
      <input {...rest} ref={inputRef} class="input input-lg w-full pr-11" type={show ? 'text' : 'password'} value={value}
        onInput={(e) => onInput(e.currentTarget.value)} autocomplete={autocomplete} spellcheck={false} autocapitalize="off" />
      <button type="button" class="icon-btn absolute right-1 top-1/2 -translate-y-1/2" aria-pressed={show}
        aria-label={show ? 'Passwort verbergen' : 'Passwort anzeigen'} onClick={() => setShow(!show)}>
        {show ? <EyeOff class="w-4 h-4" /> : <Eye class="w-4 h-4" />}
      </button>
    </div>
  )
}

const STRENGTH_TONE = ['bg-line', 'bg-bad', 'bg-warn', 'bg-ok', 'bg-ok']
export function StrengthMeter({ password }) {
  const { score, label } = passwordStrength(password)
  return (
    <div class="flex items-center gap-3" aria-live="polite">
      <div class="flex-1 grid grid-cols-4 gap-1" aria-hidden="true">
        {[1, 2, 3, 4].map(i => <span key={i} class={`h-1.5 rounded-full transition-colors ${i <= score ? STRENGTH_TONE[score] : 'bg-line'}`} />)}
      </div>
      <span class="text-[12px] text-muted w-28 text-right">{password ? <>Stärke: <span class="text-fg">{label}</span></> : 'Stärke'}</span>
    </div>
  )
}

export function RuleList({ rules }) {
  return (
    <ul class="space-y-1 text-[12.5px]" aria-label="Anforderungen">
      {rules.map(r => (
        <li key={r.id} class={`flex items-center gap-2 ${r.ok ? 'text-ok' : 'text-muted'}`}>
          {r.ok ? <Check class="w-3.5 h-3.5 shrink-0" aria-hidden="true" /> : <Circle class="w-3 h-3 shrink-0 mx-px" aria-hidden="true" />}
          <span>{r.text}</span><span class="sr-only">{r.ok ? ' – erfüllt' : ' – noch nicht erfüllt'}</span>
        </li>
      ))}
    </ul>
  )
}

const ALERT = {
  bad: ['border-bad/40 bg-bad/10', AlertTriangle, 'text-bad'],
  warn: ['border-warn/40 bg-warn/10', AlertTriangle, 'text-warn'],
  info: ['border-accent/30 bg-accent/5', Info, 'text-accent'],
  ok: ['border-ok/40 bg-ok/10', CheckCircle2, 'text-ok'],
}
/** Inline message; errors are announced (role=alert) */
export function Alert({ tone = 'info', title, children, class: cls = '' }) {
  const [box, Icon, ic] = ALERT[tone] || ALERT.info
  return (
    <div role={tone === 'bad' ? 'alert' : 'status'} class={`flex gap-3 rounded-xl border px-3.5 py-3 text-[13px] ${box} ${cls}`}>
      <Icon class={`w-[18px] h-[18px] shrink-0 mt-px ${ic}`} aria-hidden="true" />
      <div class="min-w-0">
        {title && <div class="font-semibold text-fg">{title}</div>}
        {children && <div class={title ? 'text-fg/80 mt-0.5' : 'text-fg/90'}>{children}</div>}
      </div>
    </div>
  )
}

/**
 * Six-digit code: one real input (so OS autofill of one-time codes and pasting
 * a whole code work) drawn as six slots. Calls onComplete with 6 digits.
 */
export function CodeInput({ value, onChange, onComplete, disabled, autoFocus, label = 'Sechsstelliger Code', inputRef }) {
  const own = useRef(null)
  const ref = inputRef || own
  const [focus, setFocus] = useState(false)
  // also after a failed check (the field was disabled while busy and lost focus)
  useEffect(() => { if (autoFocus && !disabled) ref.current?.focus() }, [autoFocus, disabled])
  const set = (raw) => {
    const v = cleanTotp(raw)
    onChange(v)
    if (v.length === 6 && v !== value) onComplete?.(v)
  }
  return (
    <div class="relative" onClick={() => ref.current?.focus()}>
      <input ref={ref} value={value} disabled={disabled} aria-label={label}
        inputMode="numeric" autocomplete="one-time-code" pattern="[0-9]*" enterKeyHint="go" spellcheck={false}
        onInput={(e) => set(e.currentTarget.value)}
        onPaste={(e) => { const t = e.clipboardData?.getData('text'); if (t) { e.preventDefault(); set(t) } }}
        onFocus={() => setFocus(true)} onBlur={() => setFocus(false)}
        class="absolute inset-0 w-full h-full opacity-0 cursor-text" style={{ caretColor: 'transparent' }} />
      <div class="grid grid-cols-6 gap-2" aria-hidden="true">
        {Array.from({ length: 6 }, (_, i) => {
          const ch = value[i]
          const cur = focus && (i === Math.min(value.length, 5))
          return (
            <div key={i} class={`otp-slot ${cur ? 'otp-slot-active' : ''} ${ch ? 'text-fg' : 'text-muted/50'} ${disabled ? 'opacity-60' : ''}`}>
              {ch || (cur ? <span class="otp-caret" /> : '·')}
            </div>
          )
        })}
      </div>
    </div>
  )
}

export function Spinner({ class: cls = 'w-4 h-4' }) {
  return <svg class={`animate-spin ${cls}`} viewBox="0 0 24 24" fill="none" aria-hidden="true"><circle cx="12" cy="12" r="9" stroke="currentColor" stroke-opacity=".25" stroke-width="3" /><path d="M21 12a9 9 0 0 0-9-9" stroke="currentColor" stroke-width="3" stroke-linecap="round" /></svg>
}
