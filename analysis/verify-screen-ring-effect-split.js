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

assert(exists('effects/geometric/screen_ring.effect'), 'Screen Ring shader must exist.');
assert(exists('effects/geometric/analog_name_field.effect'), 'Analog Name Field shader must exist.');
assert(exists('effects/retro/crt_slit_field.effect'), 'CRT Slit Field shader must exist.');
assert(exists('effects_meta/analog_name_field.json'), 'Analog Name Field metadata must exist.');
assert(exists('effects_meta/crt_slit_field.json'), 'CRT Slit Field metadata must exist.');

const screenShader = read('effects/geometric/screen_ring.effect');
const screenMeta = read('effects_meta/screen_ring.json');
const ui = read('web-ui/vjlink-control.html');

for (const restored of ['barcode_columns', 'slit_scan', 'slat_density', 'volumetric_text', 'depth_blur', 'signal_flicker']) {
  assert(screenMeta.includes(`"${restored}"`), `Screen Ring metadata must expose restored ${restored}.`);
}

assert(/barcodeMask\s*\(/.test(screenShader), 'Screen Ring shader must contain the restored barcode background field.');
assert(/slitScanMask\s*\(/.test(screenShader), 'Screen Ring shader must contain the restored slit-scan background field.');
assert(/volumetricTextMask\s*\(/.test(screenShader), 'Screen Ring shader must contain the restored volumetric text field.');
assert(!screenShader.includes('HALO WHO?'), 'Screen Ring shader must not ship HALO WHO? text.');
assert(!screenMeta.includes('HALO WHO?'), 'Screen Ring metadata must not ship HALO WHO? text.');
assert(/"effect_id"\s*:\s*"screen_ring"/.test(screenMeta), 'Screen Ring metadata id missing.');
assert(/"name"\s*:\s*"Screen Ring"/.test(screenMeta), 'Screen Ring display name missing.');

for (const id of ['analog_name_field', 'crt_slit_field']) {
  assert(ui.includes(id), `Web UI must expose ${id}.`);
}

const analogMeta = read('effects_meta/analog_name_field.json');
assert(/"name"\s*:\s*"Analog Name Field"/.test(analogMeta), 'Analog Name Field display name missing.');
assert(analogMeta.includes('"BOSO"'), 'Analog Name Field must include BOSO as a preset name.');

const slitMeta = read('effects_meta/crt_slit_field.json');
assert(/"name"\s*:\s*"CRT Slit Field"/.test(slitMeta), 'CRT Slit Field display name missing.');

console.log('Screen Ring effect split regression checks passed.');
