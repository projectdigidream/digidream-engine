//
// What a server said before it died: the lines that say why (a refusal, a
// failure, an error) first, then the end of its output for context. A packaged
// app has no console, so this is what its error dialog quotes (main.js). The
// same rule as the Android shell's GameServer.lastWords.
//
export function serverSaid (lines) {
  const why = lines.filter((l) => /refus|fail|error|cannot|could not|problem/i.test(l))
  const end = lines.slice(-15)
  return [...why, ...(why.length ? [''] : []), ...end].join('\n').trim()
}
