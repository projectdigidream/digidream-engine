//
// The writable game tree, for the studio.
//
// A packaged app's own files are read-only (and signed, on the platforms that
// care), so what the studio writes goes to the user's data directory: the
// same layout as the staged game -- modules/<id>/... -- so the server can be
// pointed at it with --module and needs to know nothing else. The bundled
// modules are SEEDED into it on first launch and after an update, so the
// studio's palettes and the server see one tree; a module the user made is
// never touched by the seeding.
//
// Every path arrives from a web page. Relative, forward slashes, no `..`, or
// it is refused -- the page is ours, but the rule is cheaper than the trust.
//
import { mkdir, readFile, writeFile, readdir, rm, stat, cp } from 'node:fs/promises'
import { join, dirname, sep } from 'node:path'

export function safeRelative (p) {
  if (typeof p !== 'string' || p.length === 0 || p.length > 1024) return null
  if (p.startsWith('/') || p.includes('\\') || p.includes('\0') || /^[A-Za-z]:/.test(p)) return null
  const parts = p.split('/')
  if (parts.some((s) => s === '' || s === '.' || s === '..')) return null
  return parts.join(sep)
}

export function studioFiles (root) {
  const at = (p) => {
    const rel = safeRelative(p)
    if (rel === null) throw new Error(`refused path: ${p}`)
    return join(root, rel)
  }
  return {
    root,

    async writeFile (p, bytes) {
      const full = at(p)
      await mkdir(dirname(full), { recursive: true })
      await writeFile(full, bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes))
      return true
    },

    async readFile (p) {
      try { return new Uint8Array(await readFile(at(p))) } catch { return null }
    },

    /** Every file under `prefix` (a directory), relative to the root. */
    async listFiles (prefix = '') {
      const base = prefix ? at(prefix) : root
      const out = []
      const walk = async (dir, rel) => {
        let entries
        try { entries = await readdir(dir, { withFileTypes: true }) } catch { return }
        for (const e of entries) {
          const r = rel ? `${rel}/${e.name}` : e.name
          if (e.isDirectory()) await walk(join(dir, e.name), r)
          else out.push(r)
        }
      }
      await walk(base, prefix.replace(/\/$/, ''))
      return out
    },

    async deleteFile (p) {
      try { await rm(at(p)); return true } catch { return false }
    },

    /**
     * Copy the bundled modules in when the stamp has changed. Other
     * directories under modules/ are left alone: they are the user's.
     */
    async seed (bundledModulesDir, stamp) {
      const marker = join(root, 'modules', '.seeded')
      const have = await readFile(marker, 'utf8').catch(() => null)
      if (have === stamp) return false
      let names = []
      try { names = (await readdir(bundledModulesDir, { withFileTypes: true })).filter((e) => e.isDirectory()).map((e) => e.name) } catch {}
      await mkdir(join(root, 'modules'), { recursive: true })
      for (const n of names) {
        await rm(join(root, 'modules', n), { recursive: true, force: true })
        await cp(join(bundledModulesDir, n), join(root, 'modules', n), { recursive: true })
      }
      await writeFile(marker, stamp)
      return true
    },

    /** The module directories present, bundled and the user's own. */
    async modules () {
      try {
        const entries = await readdir(join(root, 'modules'), { withFileTypes: true })
        return entries.filter((e) => e.isDirectory()).map((e) => join(root, 'modules', e.name))
      } catch { return [] }
    },

    async exists (p) { try { await stat(at(p)); return true } catch { return false } }
  }
}
