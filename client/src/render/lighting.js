//
// The lighting model (design §4).
//
// Sector light_level is the AMBIENT FLOOR a surface is lit to; a point light
// ADDS on top of it. That ordering is the whole model, and it is not the same
// as a conventional renderer's:
//
//   * Multiplying by the sector light -- which is what a standard lit material
//     with vertex colours does -- makes a torch in a dark room stay dark,
//     because the torch's contribution is scaled by the darkness it is meant to
//     relieve. Backwards.
//
//   * Replacing the sector light where a light reaches gives hard edges at the
//     radius and makes a dim lamp DARKEN a bright room.
//
// Adding is the only one of the three where "put a lamp here" does what a level
// author means by it. Doom raised a sector's light level; this is the same idea
// with distance falloff and surface orientation, which is what stops every
// floor and wall reading as one flat colour.
//
// The attenuation is exported and unit-tested. GLSL cannot be run in a test, so
// the shader below and this function are written to agree, and the properties
// worth asserting -- reaches zero AT the radius, never negative, monotonic --
// are asserted on the JS side.
//

/** Falloff at `dist` for a light of `radius`. 1 at the source, 0 at the edge. */
export function attenuation (dist, radius) {
  if (radius <= 0) return 0
  const t = 1 - dist / radius
  if (t <= 0) return 0
  // Squared, so the falloff steepens with distance the way real light does,
  // and -- more practically -- so a light's edge is not a visible ring.
  return t * t
}

/**
 * Light reaching a surface point, given its normal.
 *
 * The JS twin of the fragment shader. Exists so the model can be reasoned about
 * and tested at all; the GPU runs the GLSL.
 */
export function lightAt (point, normal, lights) {
  let r = 0
  let g = 0
  let b = 0
  for (const L of lights) {
    const dx = L.x - point.x
    const dy = L.y - point.y
    const dz = L.z - point.z
    const dist = Math.hypot(dx, dy, dz)
    const att = attenuation(dist, L.radius)
    if (att <= 0) continue
    // Surface orientation. A wall facing away from a lamp is not lit by it, and
    // without this every surface in range brightens equally -- which reads as
    // fog rather than as a light.
    const inv = dist > 0 ? 1 / dist : 0
    const ndl = Math.max(0, normal.x * dx * inv + normal.y * dy * inv + normal.z * dz * inv)
    if (ndl <= 0) continue
    const k = att * ndl * L.intensity
    r += L.r * k
    g += L.g * k
    b += L.b * k
  }
  return { r, g, b }
}

// --- wall occlusion (design §4) --------------------------------------------
//
// Geometry standing between the camera and the player is faded out in a soft
// window around the line of sight. PURELY COSMETIC: nothing about collision,
// visibility filtering or the simulation changes, and the fragment is still
// there -- it is drawn at reduced alpha and nothing else.
//
// Done in the fragment shader rather than by picking whole objects, because the
// map is merged into one mesh per texture: fading "the object in the way" would
// fade every wall in the map that happened to share a texture with it.
//
// The window is a distance from the camera-to-subject RAY, not a screen-space
// radius, so it stays the same size in the world as you zoom -- a screen-space
// window would swallow the whole room when zoomed in.

/**
 * How opaque a fragment should be, given what it stands in front of.
 *
 * The JS twin of the shader below, and the part worth testing: the geometry of
 * "is this between the camera and the player" is where this goes wrong, and it
 * goes wrong in ways that look like a rendering glitch rather than like a
 * predicate error.
 */
export function occlusionAlpha (fragment, camera, subject, {
  radius = 3.5, gap = 2.5, strength = 0.82
} = {}) {
  const bx = subject.x - camera.x
  const by = subject.y - camera.y
  const bz = subject.z - camera.z
  const bb = bx * bx + by * by + bz * bz
  if (bb < 1e-9) return 1

  const ax = fragment.x - camera.x
  const ay = fragment.y - camera.y
  const az = fragment.z - camera.z

  // Only geometry NEARER than the subject can be in front of it -- and by a
  // margin, so the floor at the player's own feet is not punched through.
  const dFrag = Math.sqrt(ax * ax + ay * ay + az * az)
  const dSubj = Math.sqrt(bb)
  if (dFrag >= dSubj - gap) return 1

  // Perpendicular distance from the line of sight.
  const t = (ax * bx + ay * by + az * bz) / bb
  if (t <= 0) return 1
  const cx = ax - bx * t
  const cy = ay - by * t
  const cz = az - bz * t
  const d = Math.sqrt(cx * cx + cy * cy + cz * cz)
  if (d >= radius) return 1

  // Soft-edged, so the window is a fade rather than a hole with a rim.
  const e = d / radius
  const smooth = e * e * (3 - 2 * e)
  return 1 - strength * (1 - smooth)
}

// How many lights one draw call considers. Uniform array sizes are fixed at
// compile time in GLSL, so this is a hard limit rather than a budget: lights
// beyond it are dropped, and dropping them silently is why the loader reports
// when a map has more.
export const MAX_ACTIVE_LIGHTS = 16

export const VERTEX_SHADER = /* glsl */`
  attribute vec3 color;
  varying vec2 vUv;
  varying vec3 vNormalW;
  varying vec3 vWorld;
  varying vec3 vSector;
  void main () {
    vUv = uv;
    vSector = color;
    vec4 world = modelMatrix * vec4(position, 1.0);
    vWorld = world.xyz;
    vNormalW = normalize(mat3(modelMatrix) * normal);
    gl_Position = projectionMatrix * viewMatrix * world;
  }
`

export const FRAGMENT_SHADER = /* glsl */`
  precision highp float;
  uniform sampler2D map;
  uniform vec2 repeat;
  uniform int numLights;
  uniform vec3 occludeTarget;
  uniform float occludeRadius;
  uniform float occludeGap;
  uniform float occludeStrength;
  uniform vec3 lightPos[${MAX_ACTIVE_LIGHTS}];
  uniform vec3 lightColor[${MAX_ACTIVE_LIGHTS}];
  uniform float lightRadius[${MAX_ACTIVE_LIGHTS}];
  uniform float lightIntensity[${MAX_ACTIVE_LIGHTS}];

  varying vec2 vUv;
  varying vec3 vNormalW;
  varying vec3 vWorld;
  varying vec3 vSector;

  void main () {
    vec4 texel = texture2D(map, vUv * repeat);
    vec3 n = normalize(vNormalW);

    // Sector light level: the floor this surface is lit to, before any lamp.
    vec3 lit = vSector;

    for (int i = 0; i < ${MAX_ACTIVE_LIGHTS}; i++) {
      if (i >= numLights) break;
      vec3 d = lightPos[i] - vWorld;
      float dist = length(d);
      float t = 1.0 - dist / max(lightRadius[i], 0.0001);
      if (t <= 0.0) continue;
      float att = t * t;
      float ndl = max(dot(n, d / max(dist, 0.0001)), 0.0);
      lit += lightColor[i] * (att * ndl * lightIntensity[i]);
    }

    // Wall occlusion: fade what stands between the camera and the player.
    // The twin of occlusionAlpha() above.
    float alpha = 1.0;
    vec3 b = occludeTarget - cameraPosition;
    float bb = dot(b, b);
    if (occludeStrength > 0.0 && bb > 1e-9) {
      vec3 a = vWorld - cameraPosition;
      float dFrag = length(a);
      float dSubj = sqrt(bb);
      float t = dot(a, b) / bb;
      if (dFrag < dSubj - occludeGap && t > 0.0) {
        float d = length(a - b * t);
        if (d < occludeRadius) {
          alpha = 1.0 - occludeStrength * (1.0 - smoothstep(0.0, occludeRadius, d));
        }
      }
    }

    gl_FragColor = vec4(texel.rgb * lit, alpha);
  }
`

/**
 * Build the uniform block for a light set.
 *
 * Fixed-length arrays, always: a GLSL uniform array has a compile-time size,
 * and resizing one means recompiling every material in the scene.
 */
export function lightUniforms (THREE, lights) {
  const pos = []
  const col = []
  const rad = []
  const inten = []
  for (let i = 0; i < MAX_ACTIVE_LIGHTS; i++) {
    const L = lights[i]
    pos.push(new THREE.Vector3(L ? L.px : 0, L ? L.py : 0, L ? L.pz : 0))
    col.push(new THREE.Color(L ? L.r : 0, L ? L.g : 0, L ? L.b : 0))
    rad.push(L ? L.radius : 0)
    inten.push(L ? L.intensity : 0)
  }
  return {
    // Off until something sets a target. A renderer with no player -- a map
    // viewer, an editor preview -- should not have a hole following the origin
    // around.
    occludeTarget: { value: new THREE.Vector3() },
    occludeRadius: { value: 3.5 },
    occludeGap: { value: 2.5 },
    occludeStrength: { value: 0 },
    numLights: { value: Math.min(lights.length, MAX_ACTIVE_LIGHTS) },
    lightPos: { value: pos },
    lightColor: { value: col },
    lightRadius: { value: rad },
    lightIntensity: { value: inten }
  }
}
