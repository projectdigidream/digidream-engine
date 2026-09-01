//
// Make sure the Electron BINARY is actually there.
//
// `npm install` is supposed to fetch it through the electron package's
// postinstall. On at least one machine it does not: the installed package
// carries no `scripts` entry at all, so npm has nothing to run, and what lands
// in node_modules is the launcher stub with no dist/ and no path.txt. The
// failure arrives later and points somewhere else entirely --
//
//   Unable to find Electron app
//
// -- which blames the app, the one part that is fine.
//
// So this checks for the binary and runs electron's own installer if it is
// missing. It does not reimplement the download; install.js is shipped inside
// the package and is the right thing to call.
//
// Wired in as BOTH postinstall and a step of `npm start`: postinstall covers
// the normal path, and start covers an install that already happened without it.
const { existsSync } = require('node:fs')
const { join } = require('node:path')
const { execFileSync } = require('node:child_process')

const pkg = join(__dirname, '..', 'node_modules', 'electron')
if (!existsSync(pkg)) {
  console.error('[app] electron is not installed yet -- run `npm install` first')
  process.exit(0)          // not an error during postinstall ordering
}
if (existsSync(join(pkg, 'path.txt'))) process.exit(0)

console.error('[app] electron is installed but its binary is missing; fetching it')
try {
  execFileSync(process.execPath, ['install.js'], { cwd: pkg, stdio: 'inherit' })
} catch (e) {
  console.error('[app] could not fetch the electron binary: ' + e.message)
  process.exit(1)
}
console.error('[app] electron binary ready')
