//
// COMPOSITE TEXTURES (docs/COMPOSITE_TEXTURES.md): one texture name made of
// several pictures, laid over a surface so its repeat stops showing.
//
//   composite FLOOR1 STONE 4 STONE_CRACKED STONE_MOSS flip turn overlay GRIME scale 8
//   composite WALL1  BRICK 6 BRICK_CRACKED flip x shift
//   composite YARD   GRASS patches size 3
//
// A line in assets/animations.txt, beside `sheet`, and client-only for the same
// reason: which picture is on a wall changes nothing the simulation can see, so
// the map, its hash and the server never hear of it. animations.txt already
// reaches every build (devserver, apps, APK, iOS, web build) unchanged, and an
// older client skips a first word it does not know.
//
// ONE MATERIAL, ONE DRAW. The members are packed into one atlas, and the
// fragment shader works out, per pixel, which tile it is in and how that tile
// shows its member: which member (by weight), mirrored or turned, at what
// offset, and how it meets its neighbours. Nothing is split into per-tile
// geometry, so a composite costs the draw calls a plain texture does.
//
// THE SAME EVERY VISIT, ON EVERY DEVICE. Every pick comes from one integer
// hash (lowbias32), written twice -- in the GLSL below and in mix32 here -- and
// never from a float sine, whose last bits differ from one GPU to the next. The
// JS twin (compositeSources) is what the tests and the studio's thumbnails use;
// the browser check holds the two to each other.
//
// Two of the manners follow Inigo Quilez (Texture repetition, 2015). `shift`
// is his technique 1: each tile its own copy at its own offset, meeting its
// neighbours across a band. `patches` drops the tile grid for irregular cells
// round scattered points, the layout of his technique 2, each cell one copy.
// (His technique 3, a noise-picked index, was tried first: dithered, its eight
// copies came out as contour stripes a few texels wide, all seam.) His fades
// are offered as `seam blend`; the default, `seam dither`, makes the same
// choices per pixel with an ordered dither, so every pixel is one picture's own
// colour and only one copy is read. Nearest pixels and no mipmaps mean none of
// his derivative work is needed: a jump in the coordinates at a tile edge
// changes no LOD.
//

export const COMPOSITE_MAX_MEMBERS = 8
export const COMPOSITE_DEFAULT_PATCH = 3
export const COMPOSITE_DEFAULT_OVERLAY_SCALE = 4
export const COMPOSITE_DEFAULT_OVERLAY_STRENGTH = 0.5

// How wide the border between two patches is: in texels when dithered, so it
// stays a ragged pixel edge at any patch size; in patches when blended.
const PATCH_DITHER_TEXELS = 6
const PATCH_BLEND = 0.3

// The shader's flags, one bit each (cpFlags).
export const CP = { FLIP_X: 1, FLIP_Y: 2, TURN: 4, SHIFT: 8, BLEND: 16, PATCHES: 32, OVERLAY: 64 }

// Every tile and texel index is counted from this many tiles away, so they are
// all positive: GLSL's integer / and % are only exact for positive operands,
// and a uint made from a negative int is not something to rely on.
const OFF = 65536

const KEYWORDS = new Set(['flip', 'turn', 'shift', 'seam', 'patches', 'size', 'overlay', 'scale', 'strength', 'seed'])

// --- the hash ------------------------------------------------------------------

/** lowbias32 (Chris Wellons). The GLSL's cpMix, bit for bit. */
export function mix32 (x) {
  x = (x ^ (x >>> 16)) >>> 0
  x = Math.imul(x, 0x7feb352d) >>> 0
  x = (x ^ (x >>> 15)) >>> 0
  x = Math.imul(x, 0x846ca68b) >>> 0
  return (x ^ (x >>> 16)) >>> 0
}

/** Two non-negative ints and a seed to 32 bits. The GLSL's cpHash. */
export function hash2 (x, y, s) {
  return mix32((Math.imul(x >>> 0, 0x9E3779B1) ^ mix32(((y >>> 0) ^ s) >>> 0)) >>> 0)
}

/** A composite's own seed, from its name: 24 bits, so a float carries it exactly. */
export function nameSeed (name) {
  let h = 2166136261 >>> 0
  for (let i = 0; i < name.length; i++) {
    h ^= name.charCodeAt(i)
    h = Math.imul(h, 16777619) >>> 0
  }
  return mix32(h) & 0xFFFFFF
}

/**
 * A wall surface's seed (mapgeom.js wallQuad). Walls count u from each side's
 * own end, so without it every wall in a room would start with the same tiles.
 * Taken from the side's two ends and which field it is, quantised to 1/16 of a
 * unit, so a re-bake or an edit elsewhere in the map leaves it alone. Flats
 * have 0: their tiles are counted in world units and are unique already.
 */
export function sideSeed (ax, ay, bx, by, kind) {
  const q = (v) => Math.round(v * 16) >>> 0
  return mix32((q(ax) ^ mix32((q(ay) ^ mix32((q(bx) ^ mix32((q(by) ^ mix32(kind + 1)) >>> 0)) >>> 0)) >>> 0)) >>> 0) & 0xFFFFFF
}

/** The seed one view draws a composite with: the line's, turned by the level's. */
export function viewSeed (def, level = 0) {
  return ((def.seed ^ mix32((level >>> 0) ^ 0x2545F491)) & 0xFFFFFF) >>> 0
}

// --- the line ------------------------------------------------------------------

const isNumber = (t) => /^[0-9]*\.?[0-9]+$/.test(t)

/**
 * One `composite` line, already split into words, to a definition -- or an
 * error naming what is wrong. `tok[0]` is 'composite'.
 */
export function parseCompositeLine (tok) {
  const name = (tok[1] ?? '').toUpperCase()
  if (!name || KEYWORDS.has(tok[1])) return { error: 'a composite needs a texture name' }
  const def = {
    name, members: [], flipX: false, flipY: false, turn: false,
    shift: false, patches: 0, seam: 'dither', overlay: null, seed: nameSeed(name)
  }
  let seamSaid = false
  let i = 2
  const err = (m) => ({ error: `${name}: ${m}` })
  while (i < tok.length) {
    const t = tok[i]
    if (t === 'flip') {
      if (tok[i + 1] === 'x') { def.flipX = true; i += 2 } else if (tok[i + 1] === 'y') { def.flipY = true; i += 2 } else { def.flipX = def.flipY = true; i++ }
    } else if (t === 'turn') {
      def.turn = true; i++
    } else if (t === 'shift') {
      def.shift = true; i++
    } else if (t === 'seam') {
      const v = tok[i + 1]
      if (v !== 'dither' && v !== 'blend') return err(`'seam' is 'dither' or 'blend', not '${v ?? ''}'`)
      def.seam = v; seamSaid = true; i += 2
    } else if (t === 'patches') {
      def.patches = COMPOSITE_DEFAULT_PATCH; i++
      if (tok[i] === 'size') {
        const n = Number(tok[i + 1])
        if (!isNumber(tok[i + 1] ?? '') || !(n > 0)) return err(`'patches size' is a number of tiles above 0, not '${tok[i + 1] ?? ''}'`)
        def.patches = n; i += 2
      }
    } else if (t === 'overlay') {
      const o = tok[i + 1]
      if (!o || KEYWORDS.has(o) || isNumber(o)) return err("'overlay' needs a texture name")
      def.overlay = { name: o.toUpperCase(), scale: COMPOSITE_DEFAULT_OVERLAY_SCALE, strength: COMPOSITE_DEFAULT_OVERLAY_STRENGTH }
      i += 2
      while (tok[i] === 'scale' || tok[i] === 'strength') {
        const n = Number(tok[i + 1])
        if (!isNumber(tok[i + 1] ?? '')) return err(`'${tok[i]}' needs a number, not '${tok[i + 1] ?? ''}'`)
        if (tok[i] === 'scale') {
          if (!(n > 0)) return err("an overlay's 'scale' is above 0")
          def.overlay.scale = n
        } else {
          if (n > 1) return err("an overlay's 'strength' is 0 to 1")
          def.overlay.strength = n
        }
        i += 2
      }
    } else if (t === 'seed') {
      const n = Number(tok[i + 1])
      if (!/^[0-9]+$/.test(tok[i + 1] ?? '')) return err(`'seed' is a whole number, not '${tok[i + 1] ?? ''}'`)
      def.seed = n & 0xFFFFFF; i += 2
    } else if (t === 'size' || t === 'scale' || t === 'strength') {
      return err(`'${t}' belongs after '${t === 'size' ? 'patches' : 'overlay NAME'}'`)
    } else if (isNumber(t)) {
      const last = def.members[def.members.length - 1]
      if (!last || last.said) return err(`a weight (${t}) comes right after the texture it weighs`)
      if (!(Number(t) > 0)) return err(`a weight is above 0, not ${t}`)
      last.weight = Number(t); last.said = true; i++
    } else {
      def.members.push({ name: t.toUpperCase(), weight: 1, said: false }); i++
    }
  }
  if (def.members.length === 0) return err('a composite needs at least one texture')
  if (def.members.length > COMPOSITE_MAX_MEMBERS) return err(`at most ${COMPOSITE_MAX_MEMBERS} textures, not ${def.members.length}`)
  if (def.members.some((m) => m.name === name)) return err('a composite cannot be one of its own textures')
  if (def.shift && def.patches) return err("'shift' and 'patches' are two ways of doing one thing; pick one")
  if (seamSaid && !def.shift && !def.patches) return err("'seam' goes with 'shift' or 'patches'")
  for (const m of def.members) delete m.said
  return { def }
}

/** The shader's flag bits for a definition. */
export function flagsOf (def, { square = true, overlay = !!def.overlay } = {}) {
  let f = 0
  if (def.flipX) f |= CP.FLIP_X
  if (def.flipY) f |= CP.FLIP_Y
  if (def.turn && square) f |= CP.TURN
  if (def.shift) f |= CP.SHIFT
  if (def.patches) f |= CP.PATCHES
  if (def.seam === 'blend') f |= CP.BLEND
  if (overlay) f |= CP.OVERLAY
  return f
}

/**
 * The cumulative weights, as the GPU holds them: float32, the last exactly 1,
 * padded to the uniform's eight. A tile with r below entry k shows member k.
 */
export function cumulativeWeights (members) {
  const out = new Float32Array(COMPOSITE_MAX_MEMBERS).fill(1)
  const total = members.reduce((a, m) => a + m.weight, 0)
  let acc = 0
  members.forEach((m, k) => { acc += m.weight; out[k] = k === members.length - 1 ? 1 : acc / total })
  return out
}

// --- the JS twin of the shader ---------------------------------------------------

const BAYER = [0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5]
const f32 = Math.fround
const bayer = (x, y) => f32((BAYER[(y & 3) * 4 + (x & 3)] + 0.5) / 16)
function smooth (e0, e1, x) {
  const t = Math.min(1, Math.max(0, f32(f32(x - e0) / f32(e1 - e0))))
  return f32(f32(t * t) * f32(3 - f32(2 * t)))
}
const unit = (h) => (h >>> 8) / 16777216

function memberOf (h, cum, count) {
  const r = unit(h)
  for (let k = 0; k < count - 1; k++) if (r < cum[k]) return k
  return count - 1
}

// How one tile (or one patch's copy) shows its member at local texel (x, y).
function place (h, x, y, W, H, flags) {
  const h2 = mix32((h ^ 0x68E31DA4) >>> 0)
  if ((flags & CP.FLIP_X) && (h2 & 1)) x = W - 1 - x
  if ((flags & CP.FLIP_Y) && (h2 & 2)) y = H - 1 - y
  if ((flags & CP.TURN) && W === H) {
    const k = (h2 >>> 2) & 3
    if (k === 1) [x, y] = [W - 1 - y, x]
    else if (k === 2) [x, y] = [W - 1 - x, H - 1 - y]
    else if (k === 3) [x, y] = [y, H - 1 - x]
  }
  if (flags & (CP.SHIFT | CP.PATCHES)) {
    const h3 = mix32((h ^ 0xB5297A4D) >>> 0)
    x = (x + (h3 % W)) % W
    y = (y + ((h3 >>> 16) % H)) % H
  }
  return [x, y]
}

function fetch (h, x, y, p) {
  const member = memberOf(h, p.cum, p.count)
  const [px, py] = place(h, x, y, p.cell[0], p.cell[1], p.flags)
  return { member, x: px, y: py }
}

/**
 * Where the texel at global texel (gx, gy) of a surface comes from: a list of
 * { member, x, y, w } -- one entry, or the four (shift) or two (patches) a
 * `seam blend` mixes. `p` is { cell: [w, h], cum, count, flags, seed, patch },
 * as uniformsFor makes them; `side` is the surface's own seed (sideSeed).
 */
export function compositeSources (p, gx, gy, side = 0) {
  const [W, H] = p.cell
  const X = gx + W * OFF
  const Y = gy + H * OFF
  const tx = Math.floor(X / W)
  const ty = Math.floor(Y / H)
  const lx = X - tx * W
  const ly = Y - ty * H
  const s = mix32((p.seed ^ mix32((side ^ 0x632BE5AB) >>> 0)) >>> 0)
  const one = (h) => [{ ...fetch(h, lx, ly, p), w: 1 }]

  if (p.flags & CP.PATCHES) {
    // Irregular cells about `patch` tiles across, round scattered points; each
    // cell one copy, and only the border between the nearest two is mixed.
    const qx = f32(f32(gx + 0.5) / f32(W * p.patch))
    const qy = f32(f32(gy + 0.5) / f32(H * p.patch))
    const ix = Math.floor(qx)
    const iy = Math.floor(qy)
    const fx = f32(qx - ix)
    const fy = f32(qy - iy)
    const ns = (s ^ 0x5EED) >>> 0
    let d1 = 8
    let d2 = 8
    let c1 = [ix, iy]
    let c2 = c1
    for (let j = -1; j <= 1; j++) {
      for (let i = -1; i <= 1; i++) {
        const k = [ix + i + OFF, iy + j + OFF]
        const h = hash2(k[0], k[1], ns)
        const ox = f32(f32(i + f32(unit(h))) - fx)
        const oy = f32(f32(j + f32(unit(mix32(h)))) - fy)
        const d = f32(f32(ox * ox) + f32(oy * oy))
        if (d < d1) { d2 = d1; c2 = c1; d1 = d; c1 = k } else if (d < d2) { d2 = d; c2 = k }
      }
    }
    const e = f32(f32(Math.sqrt(d2)) - f32(Math.sqrt(d1)))
    const band = p.flags & CP.BLEND ? PATCH_BLEND : f32(PATCH_DITHER_TEXELS / f32(W * p.patch))
    const w = f32(0.5 - f32(0.5 * smooth(0, band, e)))
    const ha = hash2(c1[0], c1[1], s)
    const hb = hash2(c2[0], c2[1], s)
    if (!(p.flags & CP.BLEND)) return one(w > bayer(X, Y) ? hb : ha)
    return [{ ...fetch(ha, lx, ly, p), w: 1 - w }, { ...fetch(hb, lx, ly, p), w }]
  }

  if (p.flags & CP.SHIFT) {
    const bx = smooth(0.25, 0.75, f32(f32(lx + 0.5) / W))
    const by = smooth(0.25, 0.75, f32(f32(ly + 0.5) / H))
    if (!(p.flags & CP.BLEND)) {
      const dx = bx > bayer(X, Y) ? 1 : 0
      const dy = by > bayer(Y + 3, X + 1) ? 1 : 0
      return one(hash2(tx + dx, ty + dy, s))
    }
    return [
      { ...fetch(hash2(tx, ty, s), lx, ly, p), w: (1 - bx) * (1 - by) },
      { ...fetch(hash2(tx + 1, ty, s), lx, ly, p), w: bx * (1 - by) },
      { ...fetch(hash2(tx, ty + 1, s), lx, ly, p), w: (1 - bx) * by },
      { ...fetch(hash2(tx + 1, ty + 1, s), lx, ly, p), w: bx * by }
    ]
  }

  return one(hash2(tx, ty, s))
}

/**
 * Everything the shader is told about one composite, as plain numbers: the
 * JS twin reads exactly this, so the tests and the GPU start from one source.
 */
export function uniformsFor (def, { cell, square = cell[0] === cell[1], overlay = !!def.overlay, level = 0 } = {}) {
  return {
    cell,
    cum: cumulativeWeights(def.members),
    count: def.members.length,
    flags: flagsOf(def, { square, overlay }),
    seed: viewSeed(def, level),
    patch: def.patches || COMPOSITE_DEFAULT_PATCH
  }
}

// --- the shader ----------------------------------------------------------------

// Inserted inside `#ifdef COMPOSITE` after `uniform sampler2D map;`, where map
// is the ATLAS: the members side by side, `cpCols` to a row, each `cpCell` px.
// Read with texelFetch, from integer texel indices, so nothing bleeds from one
// member's cell into the next whatever the filter says.
export const COMPOSITE_GLSL = /* glsl */`
  // 32-bit, or the hash is not the hash: a mediump int may be 16 bits.
  precision highp int;
  uniform vec2 cpCell;
  uniform float cpCols;
  uniform int cpCount;
  uniform float cpWeights[${COMPOSITE_MAX_MEMBERS}];
  uniform int cpFlags;
  uniform float cpSeed;
  uniform float cpPatch;
  uniform sampler2D cpOverlay;
  uniform vec2 cpOverlayRel;
  uniform float cpOverlayStrength;

  const int CP_OFF = ${OFF};

  uint cpMix (uint x) {
    x ^= x >> 16u; x *= 0x7feb352du;
    x ^= x >> 15u; x *= 0x846ca68bu;
    return x ^ (x >> 16u);
  }
  uint cpHash (ivec2 c, uint s) { return cpMix((uint(c.x) * 0x9E3779B1u) ^ cpMix(uint(c.y) ^ s)); }
  float cpUnit (uint h) { return float(h >> 8u) * (1.0 / 16777216.0); }
  float cpBayer (ivec2 q) {
    int i = (q.y & 3) * 4 + (q.x & 3);
    const int B[16] = int[16](0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5);
    return (float(B[i]) + 0.5) / 16.0;
  }

  vec4 cpFetch (uint h, ivec2 L, ivec2 cell) {
    float r = cpUnit(h);
    int m = cpCount - 1;
    for (int k = 0; k < ${COMPOSITE_MAX_MEMBERS - 1}; k++) {
      if (k >= cpCount - 1) break;
      if (r < cpWeights[k]) { m = k; break; }
    }
    uint h2 = cpMix(h ^ 0x68E31DA4u);
    if ((cpFlags & ${CP.FLIP_X}) != 0 && (h2 & 1u) != 0u) L.x = cell.x - 1 - L.x;
    if ((cpFlags & ${CP.FLIP_Y}) != 0 && (h2 & 2u) != 0u) L.y = cell.y - 1 - L.y;
    if ((cpFlags & ${CP.TURN}) != 0 && cell.x == cell.y) {
      uint k = (h2 >> 2u) & 3u;
      if (k == 1u) L = ivec2(cell.x - 1 - L.y, L.x);
      else if (k == 2u) L = ivec2(cell.x - 1 - L.x, cell.y - 1 - L.y);
      else if (k == 3u) L = ivec2(L.y, cell.y - 1 - L.x);
    }
    if ((cpFlags & ${CP.SHIFT | CP.PATCHES}) != 0) {
      uint h3 = cpMix(h ^ 0xB5297A4Du);
      L = ivec2(int((uint(L.x) + h3 % uint(cell.x)) % uint(cell.x)),
                int((uint(L.y) + (h3 >> 16u) % uint(cell.y)) % uint(cell.y)));
    }
    int cols = int(cpCols);
    return texelFetch(map, ivec2((m - (m / cols) * cols) * cell.x + L.x, (m / cols) * cell.y + L.y), 0);
  }

  vec4 compositeTexel (vec2 tc, float sideSeed) {
    ivec2 cell = ivec2(cpCell);
    ivec2 g = ivec2(floor(tc * cpCell));
    ivec2 G = g + cell * CP_OFF;
    ivec2 tile = G / cell;
    ivec2 L = G - tile * cell;
    uint s = cpMix(uint(cpSeed) ^ cpMix(uint(sideSeed) ^ 0x632BE5ABu));
    vec4 c;
    if ((cpFlags & ${CP.PATCHES}) != 0) {
      // Irregular cells round scattered points, cpPatch tiles apart: each cell
      // one copy, and only the border between the nearest two is mixed.
      vec2 q = (vec2(g) + 0.5) / (cpCell * cpPatch);
      vec2 qi = floor(q);
      vec2 f = q - qi;
      ivec2 k0 = ivec2(qi) + ivec2(CP_OFF);
      uint ns = s ^ 0x5EEDu;
      float d1 = 8.0;
      float d2 = 8.0;
      ivec2 c1 = k0;
      ivec2 c2 = k0;
      for (int j = -1; j <= 1; j++) {
        for (int i = -1; i <= 1; i++) {
          ivec2 k = k0 + ivec2(i, j);
          uint h = cpHash(k, ns);
          vec2 o = vec2(float(i) + cpUnit(h), float(j) + cpUnit(cpMix(h))) - f;
          float d = dot(o, o);
          if (d < d1) { d2 = d1; c2 = c1; d1 = d; c1 = k; } else if (d < d2) { d2 = d; c2 = k; }
        }
      }
      float e = sqrt(d2) - sqrt(d1);
      bool blend = (cpFlags & ${CP.BLEND}) != 0;
      float band = blend ? ${PATCH_BLEND.toFixed(2)} : ${PATCH_DITHER_TEXELS.toFixed(1)} / (cpCell.x * cpPatch);
      float w = 0.5 - 0.5 * smoothstep(0.0, band, e);
      uint ha = cpHash(c1, s);
      uint hb = cpHash(c2, s);
      if (!blend) c = cpFetch(w > cpBayer(G) ? hb : ha, L, cell);
      else c = mix(cpFetch(ha, L, cell), cpFetch(hb, L, cell), w);
    } else if ((cpFlags & ${CP.SHIFT}) != 0) {
      vec2 b = smoothstep(0.25, 0.75, (vec2(L) + 0.5) / cpCell);
      if ((cpFlags & ${CP.BLEND}) == 0) {
        ivec2 d = ivec2(b.x > cpBayer(G) ? 1 : 0, b.y > cpBayer(G.yx + ivec2(3, 1)) ? 1 : 0);
        c = cpFetch(cpHash(tile + d, s), L, cell);
      } else {
        c = mix(mix(cpFetch(cpHash(tile, s), L, cell), cpFetch(cpHash(tile + ivec2(1, 0), s), L, cell), b.x),
                mix(cpFetch(cpHash(tile + ivec2(0, 1), s), L, cell), cpFetch(cpHash(tile + ivec2(1, 1), s), L, cell), b.x), b.y);
      }
    } else {
      c = cpFetch(cpHash(tile, s), L, cell);
    }
    if ((cpFlags & ${CP.OVERLAY}) != 0) {
      // At the CENTRE of this texel, so every pixel of the picture stays one
      // colour; grey is neutral, as Quake 3's detail textures were.
      vec3 o = texture2D(cpOverlay, (vec2(g) + 0.5) / cpCell * cpOverlayRel).rgb;
      c.rgb *= mix(vec3(1.0), 2.0 * o, cpOverlayStrength);
    }
    return c;
  }
`

/**
 * The uniforms a composite material carries, at the values a placeholder
 * needs: one member the placeholder's size, nothing turned. Created with the
 * material, so its see-through twins and a fill's top can share the very
 * objects (as they share `map`) before the pictures arrive.
 */
export function compositeUniforms (THREE) {
  return {
    cpCell: { value: new THREE.Vector2(128, 128) },
    cpCols: { value: 1 },
    cpCount: { value: 1 },
    cpWeights: { value: Array(COMPOSITE_MAX_MEMBERS).fill(1) },
    cpFlags: { value: 0 },
    cpSeed: { value: 0 },
    cpPatch: { value: COMPOSITE_DEFAULT_PATCH },
    cpOverlay: { value: null },
    cpOverlayRel: { value: new THREE.Vector2(1, 1) },
    cpOverlayStrength: { value: 0 }
  }
}

/** The composite uniforms of a material, to share with a twin; {} if it has none. */
export function compositeUniformsOf (uniforms) {
  if (!uniforms?.cpCell) return {}
  const out = {}
  for (const k of Object.keys(uniforms)) if (k.startsWith('cp')) out[k] = uniforms[k]
  return out
}

/** How many atlas columns for n members: as square as it gets. */
export const atlasColumns = (n) => Math.ceil(Math.sqrt(n))

/**
 * The members side by side on one canvas, each drawn at the first one's size.
 * `pictures` are { image } with anything drawImage takes.
 */
export function buildAtlas (pictures, cell, doc = globalThis.document) {
  const cols = atlasColumns(pictures.length)
  const rows = Math.ceil(pictures.length / cols)
  const canvas = doc.createElement('canvas')
  canvas.width = cols * cell[0]
  canvas.height = rows * cell[1]
  const ctx = canvas.getContext('2d')
  ctx.imageSmoothingEnabled = false
  pictures.forEach((p, k) => {
    const img = p.image
    ctx.drawImage(img, 0, 0, img.width, img.height, (k % cols) * cell[0], Math.floor(k / cols) * cell[1], cell[0], cell[1])
  })
  return { canvas, cols, rows }
}

/**
 * A composite's atlas texture, with the warnings about its members said once.
 * `members` are the loaded pictures in the line's order, each
 * { name, image, w, h } -- missing ones already left out by the caller.
 * `configure` is render/textures.js configureTexture.
 */
export function compositeAtlas (THREE, def, members, { configure, warn = (m) => console.warn(m) }) {
  const cell = [members[0].w, members[0].h]
  for (const m of members.slice(1)) {
    if (m.w !== cell[0] || m.h !== cell[1]) {
      warn(`[anim] composite ${def.name}: ${m.name} is ${m.w}x${m.h}; drawn at ${members[0].name}'s ${cell[0]}x${cell[1]}`)
    }
  }
  if (def.turn && cell[0] !== cell[1]) warn(`[anim] composite ${def.name}: 'turn' needs square textures; ${cell[0]}x${cell[1]} is not turned`)
  const { canvas, cols } = buildAtlas(members, cell)
  if (canvas.width > 4096 || canvas.height > 4096) {
    warn(`[anim] composite ${def.name}: its pictures make a ${canvas.width}x${canvas.height} atlas, larger than some phones draw`)
  }
  const texture = configure(THREE, new THREE.CanvasTexture(canvas))
  texture.name = def.name
  return { texture, cols, cell }
}

/**
 * An overlay's texture. LINEAR, unlike every other picture: it is read once per
 * base texel, at its centre, so it shades the picture's pixels smoothly
 * without adding any pixels of its own.
 */
export function overlayTexture (THREE, image) {
  const t = new THREE.Texture(image)
  t.wrapS = t.wrapT = THREE.RepeatWrapping
  t.colorSpace = THREE.NoColorSpace
  t.magFilter = t.minFilter = THREE.LinearFilter
  t.generateMipmaps = false
  t.flipY = false
  t.needsUpdate = true
  return t
}

/**
 * Make `material` draw composite `def` from its atlas (compositeAtlas).
 * `members` as for the atlas, each with its `weight` and `cutout`; `overlay`
 * is { texture, w, h } or null. `level` turns the picks per level, so two
 * cells of a region do not lay their floors out alike.
 */
export function applyComposite (THREE, material, { def, members, atlas, overlay = null, pxPerUnit = 32, level = 0 }) {
  const { cell, cols } = atlas
  const u = material.uniforms
  for (const [k, v] of Object.entries(compositeUniforms(THREE))) if (!u[k]) u[k] = v
  const p = uniformsFor({ ...def, members }, { cell, overlay: !!overlay, level })
  u.map.value = atlas.texture
  u.repeat.value.set(pxPerUnit / cell[0], pxPerUnit / cell[1])
  u.cpCell.value.set(cell[0], cell[1])
  u.cpCols.value = cols
  u.cpCount.value = p.count
  u.cpWeights.value = Array.from(p.cum)
  u.cpFlags.value = p.flags
  u.cpSeed.value = p.seed
  u.cpPatch.value = p.patch
  if (overlay) {
    u.cpOverlay.value = overlay.texture
    u.cpOverlayRel.value.set(cell[0] / (overlay.w * def.overlay.scale), cell[1] / (overlay.h * def.overlay.scale))
    u.cpOverlayStrength.value = def.overlay.strength
  }
  const defines = { ...(material.defines ?? {}), COMPOSITE: '' }
  if (members.some((m) => m.cutout)) defines.CUTOUT = ''
  material.defines = defines
  material.needsUpdate = true
  return p
}
