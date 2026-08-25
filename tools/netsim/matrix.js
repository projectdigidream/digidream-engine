#!/usr/bin/env node
//
// Runs the prediction test across a matrix of network conditions.
//
// This is the Phase 1 exit criterion made executable. "Zero mispredictions on
// loopback" is a much weaker claim than it sounds: loopback has no latency, no
// loss and no reordering, so the reconciliation path barely runs. Under real
// conditions the client predicts much further ahead, replays far more ticks per
// snapshot, and loses input and snapshots outright -- and the answer must STILL
// be exactly zero, because none of those things change what the arithmetic
// computes.
//
// Each cell runs on its own ports so cells cannot interfere, and with its own
// PRNG seed so a failing cell reproduces exactly.
//
//   node tools/netsim/matrix.js            full matrix
//   node tools/netsim/matrix.js --quick    a representative subset
//
import { spawn } from 'node:child_process'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const ROOT = join(HERE, '..', '..')
const EDGE = join(ROOT, 'server', 'edge')

const quick = process.argv.includes('--quick')

// Round-trip times chosen to bracket what the design has to survive: LAN,
// same-continent, cross-continent, and a genuinely bad connection.
const RTTS = quick ? [0, 150] : [0, 50, 150, 300]
const LOSSES = quick ? [0, 5] : [0, 2, 5]
const REORDER = quick ? [0] : [0, 2]

const cells = []
let port = 34600
for (const rtt of RTTS) {
  for (const loss of LOSSES) {
    for (const reorder of REORDER) {
      cells.push({
        rtt,
        loss,
        reorder,
        wtPort: port++,
        linkPort: port++,
        seed: 1 + cells.length
      })
    }
  }
}

const runCell = (c) => new Promise((resolve) => {
  const env = {
    ...process.env,
    DNP_LATENCY_MS: String(c.rtt / 2),      // one-way
    DNP_JITTER_MS: String(Math.round(c.rtt / 10)),
    DNP_LOSS_PCT: String(c.loss),
    DNP_REORDER_PCT: String(c.reorder),
    DNP_WT_PORT: String(c.wtPort),
    DNP_LINK_PORT: String(c.linkPort),
    DNP_SEED: String(c.seed),
    DNP_TICKS: String(quick ? 300 : 420)
  }
  const p = spawn('node', ['test/prediction.test.js'], { cwd: EDGE, env, stdio: ['ignore', 'pipe', 'pipe'] })
  let out = ''
  p.stdout.on('data', (d) => { out += d.toString() })
  p.stderr.on('data', () => {})
  p.on('close', (code) => {
    const line = out.split('\n').find((l) => l.startsWith('RESULT '))
    const result = line ? JSON.parse(line.slice(7)) : null
    resolve({ cell: c, code, result, out })
  })
})

console.log(`netsim matrix: ${cells.length} cells\n`)
console.log('  RTT   loss  reord | exact  mispred  starved  replayed  dropped | verdict')
console.log('  ----- ----- ----- | -----  -------  -------  --------  ------- | -------')

let failures = 0
const summaries = []

for (const c of cells) {
  const { code, result } = await runCell(c)
  const ok = code === 0 && result && result.mispredicted === 0

  if (!ok) failures++
  summaries.push({ c, result, ok })

  const r = result ?? {}
  console.log(
    `  ${String(c.rtt + 'ms').padEnd(5)} ${String(c.loss + '%').padEnd(5)} ${String(c.reorder + '%').padEnd(5)} | ` +
    `${String(r.exact ?? '-').padEnd(6)} ${String(r.mispredicted ?? '-').padEnd(8)} ` +
    `${String(r.starved ?? '-').padEnd(8)} ${String(r.replayedTicks ?? '-').padEnd(9)} ` +
    `${String(r.dropped ?? '-').padEnd(7)} | ${ok ? 'ok' : 'FAIL'}`
  )
  if (!ok) {
    console.log(`        reproduce: DNP_SEED=${c.seed} DNP_LATENCY_MS=${c.rtt / 2} ` +
                `DNP_LOSS_PCT=${c.loss} DNP_REORDER_PCT=${c.reorder} ` +
                `DNP_WT_PORT=${c.wtPort} DNP_LINK_PORT=${c.linkPort} node test/prediction.test.js`)
  }
}

// The headline number. Mispredictions must be zero in EVERY cell -- degrading
// gracefully with worse conditions would mean the simulation was not actually
// deterministic, only approximately so.
const totalMispredicted = summaries.reduce((n, s) => n + (s.result?.mispredicted ?? 0), 0)
const totalExact = summaries.reduce((n, s) => n + (s.result?.exact ?? 0), 0)
const totalDropped = summaries.reduce((n, s) => n + (s.result?.dropped ?? 0), 0)

console.log(`\n  ${totalExact} exact matches, ${totalMispredicted} mispredictions, ` +
            `${totalDropped} datagrams dropped across ${cells.length} cells`)
console.log(failures === 0
  ? '\nMATRIX PASSED: prediction is exact under every simulated condition'
  : `\nMATRIX FAILED: ${failures}/${cells.length} cells`)

process.exit(failures === 0 ? 0 : 1)
