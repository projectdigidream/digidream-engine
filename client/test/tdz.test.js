//
// Use-before-declaration at module scope, in the client's own sources.
//
// This exists because of a bug the entire suite could not see. main.js assigned
// a `let` two hundred lines above the line that declared it -- legal-looking,
// syntactically fine, and a ReferenceError the moment the module ran, because
// `let` hoists the binding without initialising it. `node --check` passes it:
// it is a runtime error, not a parse error.
//
// And nothing here LOADS main.js. It wants a DOM, a WebGL context, three.js and
// a live WebTransport session, so it is the one file in the client with no
// automated coverage at all -- and it is the file that changes most. A cheap
// static check over the shape of the module beats no check.
//
// Deliberately narrow. It looks only at statements at BRACE DEPTH ZERO, which
// is where a module's own initialisation order is decided; everything inside a
// function body runs later, when the binding is live, and is not a question
// this can answer. Narrow and silent about the rest is the right trade for a
// check with no parser behind it: a false positive here would be a failing
// build for legal code, which is worse than a miss.
//
import { readFileSync, readdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

const HERE = dirname(fileURLToPath(import.meta.url))
const SRC = join(HERE, '..', 'src')

let checks = 0
let fails = 0
const check = (ok, label) => {
  checks++
  console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`)
  if (!ok) fails++
}

// Comments, strings, templates and regex literals removed, so that braces and
// identifiers inside them do not count. Crude by design -- it replaces their
// CONTENTS and keeps the line structure, so reported line numbers stay true.
function blank (src) {
  let out = ''
  let i = 0
  const keepNewlines = (s) => s.replace(/[^\n]/g, ' ')
  while (i < src.length) {
    const c = src[i]
    const two = src.slice(i, i + 2)
    if (two === '//') {
      const end = src.indexOf('\n', i)
      const stop = end < 0 ? src.length : end
      out += keepNewlines(src.slice(i, stop))
      i = stop
    } else if (two === '/*') {
      const end = src.indexOf('*/', i + 2)
      const stop = end < 0 ? src.length : end + 2
      out += keepNewlines(src.slice(i, stop))
      i = stop
    } else if (c === '"' || c === "'" || c === '`') {
      let j = i + 1
      while (j < src.length && src[j] !== c) { if (src[j] === '\\') j++; j++ }
      out += c + keepNewlines(src.slice(i + 1, j)) + (src[j] ?? '')
      i = j + 1
    } else {
      out += c
      i++
    }
  }
  return out
}

// Where each module-scope binding becomes usable, and where it is mentioned.
//
// Character by character, not line by line. A line-granular depth counter puts
// the body of a one-line function at depth zero -- the depth only rises after
// the line has been scanned -- and the planted control caught exactly that.
function scan (src) {
  const text = blank(src)
  const declaredAt = new Map()      // name -> 1-based line where it is declared
  const mentions = []               // { name, line }
  let depth = 0
  let line = 1
  let i = 0

  // Identifiers reached through a dot are PROPERTIES, not bindings: the `body`
  // in `document.body` has nothing to do with a module-level `let body`. Object
  // literal keys are the same kind of false positive and are not handled --
  // narrowness is the trade this check makes, see the header.
  const isIdent = (c) => c !== undefined && /[A-Za-z0-9_$]/.test(c)

  while (i < text.length) {
    const c = text[i]
    if (c === '\n') { line++; i++; continue }
    if (c === '{' || c === '(' || c === '[') { depth++; i++; continue }
    if (c === '}' || c === ')' || c === ']') { depth = Math.max(0, depth - 1); i++; continue }
    if (!/[A-Za-z_$]/.test(c)) { i++; continue }

    let j = i
    while (isIdent(text[j])) j++
    const word = text.slice(i, j)

    // Preceded by a dot (skipping spaces): a property name.
    let k = i - 1
    while (k >= 0 && (text[k] === ' ' || text[k] === '\t')) k--
    const isProperty = k >= 0 && text[k] === '.' && text[k - 1] !== '.'

    if (depth === 0 && !isProperty) {
      if (word === 'let' || word === 'const') {
        // Every name up to the initialiser is being declared here. Destructuring
        // and `let a, b` both land in this branch.
        let m = j
        const stop = text.indexOf('\n', j)
        const eq = text.indexOf('=', j)
        const limit = eq >= 0 && (stop < 0 || eq < stop) ? eq : (stop < 0 ? text.length : stop)
        while (m < limit) {
          if (/[A-Za-z_$]/.test(text[m])) {
            let n = m
            while (isIdent(text[n])) n++
            if (!declaredAt.has(text.slice(m, n))) declaredAt.set(text.slice(m, n), line)
            m = n
          } else m++
        }
      } else {
        mentions.push({ name: word, line })
      }
    }
    i = j
  }

  const bad = []
  for (const { name, line: at } of mentions) {
    const decl = declaredAt.get(name)
    if (decl !== undefined && at < decl) bad.push({ name, line: at, at: decl })
  }
  return bad
}

const files = readdirSync(SRC, { recursive: true })
  .filter((f) => typeof f === 'string' && f.endsWith('.js'))

console.log('tdz: module-scope bindings are declared before they are used')
let total = 0
for (const f of files) {
  const bad = scan(readFileSync(join(SRC, f), 'utf8'))
  total += bad.length
  for (const b of bad) {
    console.log(`     ${f}:${b.line} uses '${b.name}', declared at line ${b.at}`)
  }
  check(bad.length === 0, `src/${f}`)
}

// The control. Without it, a scanner that silently matched nothing -- a broken
// regex, a depth counter stuck above zero -- would report a clean bill of
// health for every file and look exactly like success.
const PLANTED = `
const fine = 1
later = 2
let later = 0
function f () { return alsoFine }
const alsoFine = 3
`
const planted = scan(PLANTED)
check(planted.length === 1 && planted[0].name === 'later',
      `CONTROL: the scanner finds a planted use-before-declaration (${planted.length} found)`)

console.log(`\ntdz: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
