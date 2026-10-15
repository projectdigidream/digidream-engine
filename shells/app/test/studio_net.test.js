//
// WHAT LEAVES THE PHONE (the Android shell, MainActivity.java serve): a game,
// nothing -- every request off the app's own origin is answered with a 404.
// The STUDIO may reach https hosts its bundled plugins declare (`net:<host>`
// in plugin.json, the GitHub sync's api.github.com) and its update check's,
// and no other. The Java cannot run here, so its source is held to the rule;
// on the emulator the studio APK fetched api.github.com (200) and was refused
// example.com (2026-10-15).
//
import { readFileSync, readdirSync, existsSync } from 'node:fs'

let checks = 0
let fails = 0
const check = (ok, label) => { checks++; console.log(`  ${ok ? 'ok  ' : 'FAIL'} ${label}`); if (!ok) fails++ }
const src = (p) => readFileSync(new URL(p, import.meta.url), 'utf8')

console.log('studio_net: a game reaches nothing; the studio, only what its plugins declare')
const java = src('../../android/java/com/digidream/app/MainActivity.java')
check(/netHosts = java\.util\.Collections\.emptySet\(\)/.test(java), 'a build starts with no hosts at all')
check(/if \(studio\) netHosts = studioNetHosts\(\)/.test(java) && (java.match(/netHosts = /g) ?? []).length === 2, 'only a studio build is given any (start.txt studio=1), and nowhere else')
const serve = java.slice(java.indexOf('private WebResourceResponse serve(String url)'))
const pass = serve.indexOf('netHosts.contains(')
const refuse = serve.indexOf('refused an off-origin request')
check(pass > 0 && refuse > pass && /url\.startsWith\("https:\/\/"\)/.test(serve.slice(0, pass)), 'what is let through is https to a listed host, decided before the refusal')
check(refuse > 0 && /return notFound\(\)/.test(serve.slice(refuse, refuse + 120)), 'everything else off-origin is still a 404')
const hosts = java.slice(java.indexOf('private java.util.Set<String> studioNetHosts()'))
check(/hosts\.add\("api\.github\.com"\)/.test(hosts) && /startsWith\("net:"\)/.test(hosts) && /studio\/plugins\/index\.json/.test(hosts), 'its hosts: the update check, and each bundled plugin\'s net: permissions')

// The plugins that ship declare what they reach: the GitHub sync asks for api.github.com.
const plugins = new URL('../../../../studio/plugins/', import.meta.url)
if (existsSync(plugins)) {
  const index = JSON.parse(readFileSync(new URL('index.json', plugins), 'utf8')).plugins
  const declared = index.flatMap((id) => (JSON.parse(readFileSync(new URL(`${id}/plugin.json`, plugins), 'utf8')).permissions ?? []).filter((p) => p.startsWith('net:')))
  check(declared.includes('net:api.github.com'), `the bundled plugins declare the hosts they use (${declared.join(', ') || 'none'})`)
} else console.log('  (no studio beside the engine: the plugins check is skipped)')

console.log(`\nstudio_net: ${checks} checks, ${fails} failures`)
process.exit(fails === 0 ? 0 : 1)
