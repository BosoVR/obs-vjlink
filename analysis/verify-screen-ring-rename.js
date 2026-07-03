const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const read = (rel) => fs.readFileSync(path.join(root, rel), 'utf8');
const exists = (rel) => fs.existsSync(path.join(root, rel));

function assert(condition, message) {
  if (!condition) {
    console.error('FAIL:', message);
    process.exit(1);
  }
}

assert(exists('effects/geometric/screen_ring.effect'), 'screen_ring shader file must exist.');
assert(!exists('effects/geometric/halo_text_logo_tunnel.effect'), 'old halo_text_logo_tunnel shader file must be removed.');
assert(exists('effects_meta/screen_ring.json'), 'screen_ring metadata file must exist.');
assert(!exists('effects_meta/halo_text_logo_tunnel.json'), 'old halo_text_logo_tunnel metadata file must be removed.');

const meta = read('effects_meta/screen_ring.json');
assert(/"effect_id"\s*:\s*"screen_ring"/.test(meta), 'metadata effect_id must be screen_ring.');
assert(/"name"\s*:\s*"Screen Ring"/.test(meta), 'metadata display name must be Screen Ring.');

const ui = read('web-ui/vjlink-control.html');
assert(ui.includes("screen_ring:'Screen Ring'"), 'Web UI must list Screen Ring.');
assert(!ui.includes('halo_text_logo_tunnel'), 'Web UI must not reference old halo_text_logo_tunnel id.');

const compositor = read('src/rendering/compositor.c');
assert(compositor.includes('"screen_ring"'), 'compositor true-3D path must recognize screen_ring.');

const bandEffects = read('src/rendering/band_effects.c');
assert(bandEffects.includes('"screen_ring"'), 'band effects continuous slot path must recognize screen_ring.');

for (const rel of [
  'effects/geometric/screen_ring.effect',
  'effects_meta/screen_ring.json',
  'web-ui/vjlink-control.html'
]) {
  assert(!read(rel).includes('HALO WHO?'), `${rel} must not contain HALO WHO?.`);
}

console.log('Screen Ring rename regression checks passed.');
