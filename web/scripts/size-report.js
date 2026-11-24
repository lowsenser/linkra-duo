// Build report: gzip size of every asset and of the start page (JS + CSS that
// index.html loads directly). Fails when the start budget is exceeded.
import { readFileSync, readdirSync } from 'node:fs'
import { gzipSync } from 'node:zlib'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const BUDGET_KB = 60
const data = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../../data')
const html = readFileSync(path.join(data, 'index.html'), 'utf8')
const gz = (f) => gzipSync(readFileSync(path.join(data, f)), { level: 9 }).length
const kb = (b) => (b / 1024).toFixed(1).padStart(6) + ' KB'

const start = new Set([...html.matchAll(/(?:src|href)="\/?(assets\/[^"]+\.(?:js|css))"/g)].map(m => m[1]))
const files = readdirSync(path.join(data, 'assets')).map(f => `assets/${f}`).sort()

console.log('\nAsset                                    roh        gzip   Start')
let startSum = 0
for (const f of files) {
  const raw = readFileSync(path.join(data, f)).length
  const g = /\.(js|css|html)$/.test(f) ? gz(f) : raw
  const s = start.has(f)
  if (s) startSum += g
  console.log(`${f.padEnd(38)} ${kb(raw)} ${kb(g)}   ${s ? 'ja' : (/\.(js)$/.test(f) ? 'bei Bedarf' : '')}`)
}
console.log(`\nStartseite JS+CSS (gzip -9): ${(startSum / 1024).toFixed(1)} KB  · Budget ${BUDGET_KB} KB`)
if (startSum > BUDGET_KB * 1024) { console.error('Budget überschritten'); process.exit(1) }
