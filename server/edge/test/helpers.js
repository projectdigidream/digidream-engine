import { existsSync } from 'node:fs'

/** Path to the baked test map, whichever arch directory it was built into. */
export function bakedMapPath (root) {
  if (process.env.DNP_MAP) return process.env.DNP_MAP
  for (const arch of [process.arch === 'x64' ? 'x86_64' : 'arm64', 'arm64', 'x86_64', 'native']) {
    const p = `${root}/build/${arch}/test.dnpm`
    if (existsSync(p)) return p
  }
  return null
}
