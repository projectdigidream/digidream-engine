//
// THE SHELLS OPEN A WEB PAGE IN THE SYSTEM'S BROWSER (the studio's update
// notice, bridge.js openUrl): https only, in all three shells. The desktop
// rule runs here (links.js); the Android and iOS handlers are read from their
// source -- this machine cannot run them -- and must publish the call and
// refuse anything but https before opening.
//
import { readFileSync } from 'node:fs'
import { externalUrl } from '../links.js'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }

console.log('open_url: a web page in the system browser, https only')
check(externalUrl('https://github.com/projectdigidream/digidream-studio/releases/latest') === 'https://github.com/projectdigidream/digidream-studio/releases/latest', 'an https page is opened')
for (const bad of ['http://example.com', 'file:///etc/passwd', 'javascript:alert(1)', 'dnp://x', 'https://', '', null, 42, ' https://github.com'])
  check(externalUrl(bad) === null, `refused: ${JSON.stringify(bad)}`)

const src = (p) => readFileSync(new URL(p, import.meta.url), 'utf8')
const after = (text, mark, n = 600) => { const i = text.indexOf(mark); return i < 0 ? '' : text.slice(i, i + n) }
const java = after(src('../../android/java/com/digidream/app/MainActivity.java'), 'case "openUrl"')
check(/startsWith\("https:\/\/"\)/.test(java) && java.indexOf('startsWith("https://")') < java.indexOf('ACTION_VIEW'), 'Android: openUrl refuses anything but https before it opens')
const swift = after(src('../../ios/Shell.swift'), 'case "openUrl"')
check(/hasPrefix\("https:\/\/"\)/.test(swift) && swift.indexOf('hasPrefix("https://")') < swift.indexOf('UIApplication.shared.open'), 'iOS: openUrl refuses anything but https before it opens')
check(/openUrl: \(url\) => call\('openUrl'/.test(src('../../android/web/shell.js')), 'Android and iOS pages publish openUrl (shell.js)')
check(/openUrl: \(url\) => ipcRenderer\.invoke\('dnp:studio', 'openUrl'/.test(src('../preload.cjs')) && /case 'openUrl'[\s\S]{0,120}externalUrl\(a\)/.test(src('../main.js')), 'desktop: published (preload.cjs) and checked with links.js (main.js)')

console.log(`\nopen_url: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
