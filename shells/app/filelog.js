//
// THE GAME'S LOG FILE, when the build asked for one (game.json `log: true`;
// build_app.sh --log, or the packager's "Write a log file"). The desktop twin
// of the Android shell's GameLog.java.
//
// <dir>/<id>-log.txt, started afresh at every launch, with the launch before
// it kept as <id>-log-previous.txt -- a crash that makes the player start the
// game again would otherwise overwrite the log that saw it. Every line is
// stamped with the time and written at once: a log that stops at a crash must
// hold what led to it.
//
import { appendFileSync, existsSync, mkdirSync, renameSync, rmSync, writeFileSync } from 'node:fs'
import { join } from 'node:path'

const stamp = () => {
  const d = new Date()
  const p = (n, w = 2) => String(n).padStart(w, '0')
  return `${p(d.getHours())}:${p(d.getMinutes())}:${p(d.getSeconds())}.${p(d.getMilliseconds(), 3)}`
}

/**
 * Start the log for this launch: { where, line(text) }. Null if it cannot be
 * written -- a game that cannot keep a log still runs.
 */
export function openGameLog (dir, id, header) {
  const where = join(dir, `${id}-log.txt`)
  const previous = join(dir, `${id}-log-previous.txt`)
  try {
    mkdirSync(dir, { recursive: true })
    if (existsSync(where)) {
      rmSync(previous, { force: true })
      renameSync(where, previous)
    }
    writeFileSync(where, `${stamp()} [shell] ${header}\n`)
  } catch {
    return null
  }
  let alive = true
  return {
    where,
    line (text) {
      if (!alive) return
      try {
        appendFileSync(where, String(text).split('\n').filter((l) => l !== '').map((l) => `${stamp()} ${l}\n`).join(''))
      } catch {
        alive = false
      }
    }
  }
}
