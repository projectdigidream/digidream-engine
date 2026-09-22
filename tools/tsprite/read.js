//
// Reading a ThreeSprite project (`.tsprite`) -- what it SAYS, not what it draws.
//
//   node tools/tsprite/read.js sprite.tsprite
//   node tools/tsprite/read.js sprite.tsprite --atlas SPRT.json
//
// WHAT A .tsprite IS. A zip holding `project.json` (format version 3), the
// part images at `parts/<part>/<variant>/v<k>.png`, and a thumbnail. It is
// ThreeSprite's EDITOR document: a 3D skeleton, hand-drawn parts hung off the
// bones, and animation keys.
//
// WHAT THIS DELIBERATELY DOES NOT DO. It does not compose frames. Turning a
// project into pixels means evaluating the rig -- posing bones, projecting them
// into each view, foreshortening, skinning a part across several bones,
// rendering the 3D solids with a depth buffer -- and that is ThreeSprite's own
// rasteriser, thousands of lines of it. Reimplementing it here would be a
// second implementation of somebody else's program, which is the trap this
// repository has paid for more than once and now grep-checks against.
//
// The runtime format is the ATLAS: ThreeSprite's own description of it says
// "for Unity, Godot, Phaser or your own engine". Export one of those and the
// client draws it (client/src/render/atlas.js).
//
// SO WHAT IS THIS FOR. Two things worth having:
//
//   Telling you what is in a file, without opening the editor.
//
//   Checking an atlas against the project it came from. The two state the same
//   facts -- the sprite's name, its canvas, how many views, how tall it stands,
//   what its animations are called and how long they run -- and an atlas
//   exported before the last edit agrees with nothing except itself. That is
//   the "two programs, one number" failure, and it is cheap to catch here.
//
import { readFileSync } from 'node:fs'
import { inflateRawSync } from 'node:zlib'

/** The project format this reads. ThreeSprite upgrades older ones on open. */
export const PROJECT_VERSION = 3

// --- just enough zip ---------------------------------------------------------
//
// A .tsprite is a zip and there is no zip reader in this repository, so here is
// one: find the end-of-central-directory record, walk the entries it points at,
// and inflate the ones asked for. Deflate and store are the only methods a
// writer would use for this, and anything else is refused by name rather than
// producing rubble.

function findEocd (buf) {
  // The record is 22 bytes plus a comment of up to 64 KB, so it is searched for
  // backwards rather than assumed to be at the very end.
  const min = Math.max(0, buf.length - (22 + 0xffff))
  for (let i = buf.length - 22; i >= min; i--) {
    if (buf.readUInt32LE(i) === 0x06054b50) return i
  }
  return -1
}

/** Every entry in a zip, as name -> Buffer. */
export function unzip (buf, wanted = null) {
  const eocd = findEocd(buf)
  if (eocd < 0) throw new Error('not a zip (no end-of-central-directory record)')
  const count = buf.readUInt16LE(eocd + 10)
  let p = buf.readUInt32LE(eocd + 16)
  const out = new Map()

  for (let i = 0; i < count; i++) {
    if (buf.readUInt32LE(p) !== 0x02014b50) throw new Error(`corrupt central directory at entry ${i}`)
    const method = buf.readUInt16LE(p + 10)
    const compressed = buf.readUInt32LE(p + 20)
    const nameLen = buf.readUInt16LE(p + 28)
    const extraLen = buf.readUInt16LE(p + 30)
    const commentLen = buf.readUInt16LE(p + 32)
    const localOff = buf.readUInt32LE(p + 42)
    const name = buf.toString('utf8', p + 46, p + 46 + nameLen)
    p += 46 + nameLen + extraLen + commentLen

    if (wanted && !wanted(name)) continue

    // The local header repeats the name and extra fields, and its lengths are
    // the ones that count -- a writer may put different extra data in each.
    if (buf.readUInt32LE(localOff) !== 0x04034b50) throw new Error(`corrupt local header for ${name}`)
    const lNameLen = buf.readUInt16LE(localOff + 26)
    const lExtraLen = buf.readUInt16LE(localOff + 28)
    const start = localOff + 30 + lNameLen + lExtraLen
    const raw = buf.subarray(start, start + compressed)

    if (method === 0) out.set(name, Buffer.from(raw))
    else if (method === 8) out.set(name, inflateRawSync(raw))
    else throw new Error(`${name}: compression method ${method} is not supported (store and deflate are)`)
  }
  return out
}

// --- the project -------------------------------------------------------------

/**
 * Open a `.tsprite` and read what it says about itself.
 *
 * Only `project.json` is inflated. The part images are the bulk of the file and
 * nothing here can use them -- see the header.
 */
export function readProject (bytes, name = 'project') {
  const entries = unzip(bytes, (n) => n === 'project.json')
  const json = entries.get('project.json')
  if (!json) throw new Error(`${name}: not a ThreeSprite project (no project.json)`)

  let doc
  try { doc = JSON.parse(json.toString('utf8')) } catch (e) { throw new Error(`${name}: project.json is not JSON (${e.message})`) }

  // REFUSED, NOT UPGRADED. ThreeSprite migrates version 1 and 2 projects when
  // it opens them, and it owns those migrations. Guessing at an older layout
  // here would read fields that have moved and report them confidently.
  if (doc.formatVersion !== PROJECT_VERSION) {
    throw new Error(`${name}: project format version ${doc.formatVersion}; this reads ${PROJECT_VERSION}. ` +
                    'Open it in ThreeSprite once to upgrade it.')
  }

  const anims = doc.animationOrder?.length
    ? doc.animationOrder.map((id) => doc.animations?.[id]).filter(Boolean)
    : Object.values(doc.animations ?? {})

  return {
    name,
    sprite: doc.spriteName ?? doc.name ?? null,
    title: doc.name ?? null,
    canvas: doc.canvas?.size ?? null,
    // The world height lives under `export` because it is a property of how the
    // sprite is EXPORTED, not of the document. The atlas copies it to the top
    // level of its manifest, which is why the two look different.
    worldHeight: doc.export?.worldHeight ?? null,
    views: {
      count: doc.views?.count ?? null,
      mirror: !!doc.views?.mirror,
      elevationDeg: doc.views?.elevationDeg ?? 0
    },
    animations: anims.map((a) => ({ name: a.name, fps: a.fps, loop: !!a.loop, frameCount: a.frameCount })),
    parts: Object.keys(doc.parts ?? {}).length,
    bones: Object.keys(doc.skeleton?.bones ?? {}).length,
    palettes: Object.keys(doc.palettes ?? {}).length,
    modifiedAt: doc.meta?.modifiedAt ?? null
  }
}

/**
 * Does this atlas come from this project, and is it still current?
 *
 * Every field compared here is stated by BOTH files. An atlas exported before
 * the last edit is the failure this catches: it loads, it draws, and it is one
 * animation or one view behind the project it claims to be.
 *
 * Returns a list of disagreements, empty when they match. Reported rather than
 * thrown: a caller may want to print all of them rather than the first.
 */
export function compare (project, atlas) {
  const out = []
  const say = (what, a, b) => out.push(`${what}: project says ${a}, atlas says ${b}`)

  if (project.sprite !== atlas.sprite) say('sprite name', project.sprite, atlas.sprite)
  if (project.canvas !== atlas.canvas) say('canvas', project.canvas, atlas.canvas)
  if (project.worldHeight !== atlas.worldHeight) say('world height', project.worldHeight, atlas.worldHeight)
  if (project.views.count !== atlas.views.count) say('view count', project.views.count, atlas.views.count)
  if (project.views.mirror !== atlas.views.mirror) say('mirror', project.views.mirror, atlas.views.mirror)

  // An atlas may hold a SUBSET of the project's animations -- the export dialog
  // picks which ones -- so a missing animation is not a disagreement. One that
  // is present and differs is.
  for (const a of atlas.animations) {
    const p = project.animations.find((x) => x.name === a.name)
    if (!p) { out.push(`animation '${a.name}': in the atlas, not in the project`); continue }
    if (p.fps !== a.fps) say(`animation '${a.name}' fps`, p.fps, a.fps)
    if (p.loop !== !!a.loop) say(`animation '${a.name}' loop`, p.loop, !!a.loop)
    if (p.frameCount !== a.frames.length) say(`animation '${a.name}' frames`, p.frameCount, a.frames.length)
  }
  return out
}

// --- CLI ---------------------------------------------------------------------

const isMain = process.argv[1] && process.argv[1].endsWith('read.js')
if (isMain) {
  const args = process.argv.slice(2)
  const file = args.find((a) => !a.startsWith('--'))
  const atlasAt = args.indexOf('--atlas')
  if (!file) {
    console.error('usage: node tools/tsprite/read.js <file.tsprite> [--atlas <sheet.json>]')
    process.exit(2)
  }

  const p = readProject(readFileSync(file), file)
  console.log(`${p.sprite}  ${p.canvas}x${p.canvas} canvas, ${p.views.count} views${p.views.mirror ? ' (mirrored)' : ''}` +
              `${p.views.elevationDeg ? `, elevated ${p.views.elevationDeg}deg` : ''}`)
  console.log(`  stands ${p.worldHeight} world units tall`)
  console.log(`  ${p.bones} bone(s), ${p.parts} part(s), ${p.palettes} palette(s)`)
  for (const a of p.animations) {
    console.log(`  animation '${a.name}': ${a.frameCount} frames at ${a.fps} fps${a.loop ? ', looping' : ''}`)
  }
  if (p.modifiedAt) console.log(`  last edited ${p.modifiedAt}`)

  if (atlasAt >= 0 && args[atlasAt + 1]) {
    const { parseAtlas, pivotDisagreement } = await import('../../client/src/render/atlas.js')
    const atlasFile = args[atlasAt + 1]
    const atlas = parseAtlas(JSON.parse(readFileSync(atlasFile, 'utf8')), atlasFile)
    const bad = compare(p, atlas)
    console.log()
    if (bad.length === 0) {
      console.log(`${atlasFile} matches this project`)
    } else {
      console.log(`${atlasFile} does NOT match this project:`)
      for (const b of bad) console.log(`  ${b}`)
    }
    const drift = pivotDisagreement(atlas)
    if (drift > 1e-6) console.log(`  warning: the atlas states its feet twice and they disagree by ${drift}`)
    process.exit(bad.length ? 1 : 0)
  }
}
