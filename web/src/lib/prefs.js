import { signal, computed, effect } from '@preact/signals'

// Per-browser UI preferences (localStorage; the gateway itself is not involved)
const load = (k, d) => { try { const v = localStorage.getItem(k); return v == null ? d : JSON.parse(v) } catch { return d } }
const save = (k, v) => { try { localStorage.setItem(k, JSON.stringify(v)) } catch { } }

export const themePref = signal(load('linkra.theme', 'system'))   // system | light | dark
export const expert = signal(load('linkra.expert', false))          // full technical view
const reduced = typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches
export const motion = signal(load('linkra.motion', !reduced))       // animations (default follows the system)
export const variant = signal(load('linkra.variant', 'console'))     // design: studio | console

const mq = typeof matchMedia === 'function' ? matchMedia('(prefers-color-scheme: dark)') : null
const sysDark = signal(mq ? mq.matches : true)
mq?.addEventListener?.('change', (e) => { sysDark.value = e.matches })

/** Effective theme: 'dark' | 'light' */
export const theme = computed(() => themePref.value === 'system' ? (sysDark.value ? 'dark' : 'light') : themePref.value)

effect(() => {
  const dark = theme.value === 'dark'
  document.documentElement.classList.toggle('dark', dark)
  document.querySelector('meta[name="theme-color"]')?.setAttribute('content', dark ? '#05080d' : '#f1f4f8')
})
effect(() => save('linkra.theme', themePref.value))
effect(() => save('linkra.expert', expert.value))
effect(() => {
  save('linkra.motion', motion.value)
  document.documentElement.classList.toggle('no-motion', !motion.value)
})
effect(() => {
  save('linkra.variant', variant.value)
  document.documentElement.classList.toggle('v-console', variant.value === 'console')
})
