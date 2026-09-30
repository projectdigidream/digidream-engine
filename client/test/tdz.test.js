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

  // Function bodies, and where each function is CALLED from module scope. Used
  // for the one-level-deep check at the end.
  const functions = new Map()      // name -> [{ name, line }]
  const topLevelCalls = new Map()  // name -> line of the call
  let fnName = null
  let fnBraceDepth = -1      // brace depth OUTSIDE the body; -1 = body not open
  let fnBody = null
  // Braces only, tracked apart from the general bracket depth. A function's
  // body is delimited by braces, and its parameter list is not: counting all
  // brackets together closed the function on the `)` of its own parameters,
  // before the body had opened, and every body came out empty. Which is a check
  // that silently passes everything.
  let braceDepth = 0
  // The previous identifier seen at module scope, to tell `function name (`
  // from a call to `name (`.
  let prevWord = null

  // Identifiers reached through a dot are PROPERTIES, not bindings: the `body`
  // in `document.body` has nothing to do with a module-level `let body`. Object
  // literal keys are the same kind of false positive and are not handled --
  // narrowness is the trade this check makes, see the header.
  const isIdent = (c) => c !== undefined && /[A-Za-z0-9_$]/.test(c)

  while (i < text.length) {
    const c = text[i]
    if (c === '\n') { line++; i++; continue }
    if (c === '{' || c === '(' || c === '[') {
      depth++
      if (c === '{') {
        // The first brace after `function NAME` opens its body.
        if (fnName !== null && fnBraceDepth < 0) fnBraceDepth = braceDepth
        braceDepth++
      }
      i++
      continue
    }
    if (c === '}' || c === ')' || c === ']') {
      depth = Math.max(0, depth - 1)
      if (c === '}') {
        braceDepth = Math.max(0, braceDepth - 1)
        if (fnName !== null && fnBraceDepth >= 0 && braceDepth <= fnBraceDepth) {
          functions.set(fnName, fnBody)
          fnName = null
          fnBraceDepth = -1
        }
      }
      i++
      continue
    }
    if (!/[A-Za-z_$]/.test(c)) { i++; continue }

    let j = i
    while (isIdent(text[j])) j++
    const word = text.slice(i, j)

    // Preceded by a dot (skipping spaces): a property name.
    let k = i - 1
    while (k >= 0 && (text[k] === ' ' || text[k] === '\t')) k--
    const isProperty = k >= 0 && text[k] === '.' && text[k - 1] !== '.'

    // Inside a module-scope function body: collect what it reads.
    if (fnName !== null && !isProperty && word !== 'function') {
      fnBody.push({ name: word, line })
    }

    if (depth === 0 && !isProperty) {
      // `function NAME` at module scope opens a body worth collecting.
      if (word === 'function') {
        let m = j
        while (m < text.length && /[\s*]/.test(text[m])) m++
        let n = m
        while (isIdent(text[n])) n++
        if (n > m) { fnName = text.slice(m, n); fnBraceDepth = -1; fnBody = [] }
      }
      // NAME( at module scope is a call. Recorded even before the function is
      // seen, because a call may precede its declaration -- hoisting is exactly
      // what makes this bug possible.
      // NAME( at module scope is a call -- unless the NAME is the one being
      // declared, whose parameter list looks exactly the same. Without that
      // exclusion a function is recorded as called on the line it is DEFINED,
      // and every binding it reads below that line is reported as a dead-zone
      // use. Which is legal code, and a false positive is worse here than a
      // miss: it makes the check something people turn off.
      let k2 = j
      while (k2 < text.length && (text[k2] === ' ' || text[k2] === '\t')) k2++
      if (text[k2] === '(' && prevWord !== 'function' && !topLevelCalls.has(word)) {
        topLevelCalls.set(word, line)
      }

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
    if (depth === 0) prevWord = word
    i = j
  }

  const bad = []
  for (const { name, line: at } of mentions) {
    const decl = declaredAt.get(name)
    if (decl !== undefined && at < decl) bad.push({ name, line: at, at: decl })
  }

  // THE SAME BUG ONE CALL DEEP.
  //
  // A module-scope CALL to a function whose body reads a `let` declared below
  // the call is the same temporal dead zone, and the check above cannot see it:
  // the reference is inside a function body, at brace depth greater than zero,
  // where this deliberately does not look.
  //
  // It is not hypothetical. loadMapTextures() was invoked two hundred lines
  // above the `let animCatalogue` it reads, threw a ReferenceError before it
  // reached the loader, and the entire map rendered untextured -- with one
  // unhandled rejection in a console nobody was reading as the only sign.
  //
  // Deliberately shallow: one level, and only for functions declared in this
  // file. Chasing the call graph would need a real parser, and a check that
  // guesses is worse than one with a stated limit.
  for (const [fname, body] of functions) {
    const calledAt = topLevelCalls.get(fname)
    if (calledAt === undefined) continue
    for (const { name, line } of body) {
      const decl = declaredAt.get(name)
      if (decl !== undefined && calledAt < decl) {
        bad.push({ name, line, at: decl, via: `${fname}() called at line ${calledAt}` })
      }
    }
  }
  return bad
}

const files = readdirSync(SRC, { recursive: true })
  .filter((f) => typeof f === 'string' && f.endsWith('.js'))

// --- and the one the scan above structurally cannot see -------------------
//
// A CONNECTION CALLBACK RUNS DURING THE JOIN, which is a top-level await, so it
// is module-scope code in every way that matters -- and it is not a named
// function called by name, so `topLevelCalls` never hears about it.
//
// It cost a crash on a level somebody had played: the SIDE_TEXTURE handler read
// `view` two hundred lines above `let view`, and a map with a switch already
// thrown sends one of those on join. A map with no switch ever thrown sends
// none, so the bug was invisible until a level had some history in it, and what
// it produced was a black page reading "can't access lexical declaration 'view'
// before initialization".
//
// Narrow ON PURPOSE, in the same spirit as the one-level limit above: this
// checks the handler object passed to the transport, by name, rather than
// guessing which of a file's callbacks might run early. A check with a stated
// limit beats one that guesses.
function scanCallback (src, opener) {
  const text = blank(src)
  const at = text.indexOf(opener)
  if (at < 0) return null
  let i = text.indexOf('{', at)
  if (i < 0) return null
  let depth = 0
  const start = i
  for (; i < text.length; i++) {
    if (text[i] === '{') depth++
    else if (text[i] === '}' && --depth === 0) { i++; break }
  }
  const body = text.slice(start, i)
  const bodyLine = src.slice(0, start).split('\n').length

  const lines = text.split('\n')
  // SHADOWED NAMES ARE NOT THE SAME NAME. A `const acc` in the function that
  // encloses this callback is what `acc` means inside it, whatever an unrelated
  // module-level `let acc` two thousand lines down is called. Anything declared
  // at any depth ABOVE the callback is already bound by the time it runs.
  const above = new Set()
  for (let n = 0; n < bodyLine; n++) {
    const m = /\b(?:let|const|var)\s+([A-Za-z_$][\w$]*)/.exec(lines[n])
    if (m) above.add(m[1])
  }
  // Module-scope declarations AFTER the callback: those are the dangerous ones.
  const later = new Map()
  for (let n = bodyLine; n < lines.length; n++) {
    const m = /^(?:let|const|var)\s+([A-Za-z_$][\w$]*)/.exec(lines[n])
    if (m && !later.has(m[1]) && !above.has(m[1])) later.set(m[1], n + 1)
  }
  const bad = []
  for (const m of body.matchAll(/(^|[^.\w$])([A-Za-z_$][\w$]*)/g)) {
    const decl = later.get(m[2])
    if (decl !== undefined) bad.push({ name: m[2], at: decl })
  }
  return bad
}

{
  const src = readFileSync(join(SRC, 'main.js'), 'utf8')
  const bad = scanCallback(src, 'onControl (m)')
  check(bad !== null, 'the connection handler is where it is expected to be')
  // `camera` is read here and declared far below, and that is DELIBERATE: every
  // use of it is behind `cameraReady`, a flag that exists for precisely this.
  // Named rather than silently tolerated, and the guard is asserted separately
  // below -- an exemption whose reason has quietly gone away is worse than no
  // exemption at all.
  const GUARDED = ['camera']
  const names = [...new Set((bad ?? []).map((b) => b.name))].filter((n) => !GUARDED.includes(n))
  check(names.length === 0,
        `a connection callback reads nothing declared below it (${names.join(', ') || 'none'})`)
  check(/let cameraReady = false/.test(src) && /cameraReady\s*\)/.test(src),
        'and the one exemption still has the guard its exemption rests on')
  // CONTROL: the detector can fail. A reference to a binding that really is
  // declared later must be reported, or the check above passes on anything.
  const injected = src.replace('onControl (m) {', 'onControl (m) {\n        void view')
  const caught = scanCallback(injected, 'onControl (m)')
  check((caught ?? []).some((b) => b.name === 'view'),
        'CONTROL: and an injected one is caught')
}

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

// And the same bug ONE CALL DEEP, which the depth-zero rule cannot see and
// which cost a session: loadMapTextures() was invoked two hundred lines above
// the `let` it reads, threw before it reached the loader, and the whole map
// rendered untextured with one unhandled rejection as the only sign.
{
  const deep = scan(`
async function loadStuff () {
  const g = catalogue.groups
  return g
}
loadStuff()
let catalogue = { groups: [] }
`)
  check(deep.length === 1 && deep[0].name === 'catalogue',
        `CONTROL: and finds it through a call (${deep.length} found)`)

  // The three shapes that are LEGAL and must stay quiet. A false positive here
  // is worse than a miss: it is what makes a check something people turn off.
  const clean = [
    ['declared above the function', `
let catalogue = { groups: [] }
async function loadStuff () { return catalogue.groups }
loadStuff()
`],
    ['called after the declaration', `
async function loadStuff () { return catalogue.groups }
let catalogue = { groups: [] }
loadStuff()
`],
    ['never called at module scope', `
async function loadStuff () { return catalogue.groups }
let catalogue = { groups: [] }
`]
  ]
  for (const [what, src] of clean) {
    const r = scan(src)
    check(r.length === 0, `and stays quiet when ${what} (${r.length} reported)`)
  }
}

console.log(`\ntdz: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
