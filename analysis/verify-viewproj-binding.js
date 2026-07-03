const fs = require('fs');
const path = require('path');

const root = path.join(__dirname, '..');
const read = (rel) => fs.readFileSync(path.join(root, rel), 'utf8');

function assert(condition, message) {
  if (!condition) {
    console.error('FAIL:', message);
    process.exit(1);
  }
}

const effectSystem = read('src/rendering/effect_system.c');
const compositor = read('src/rendering/compositor.c');

for (const [name, source] of [
  ['effect_system.c', effectSystem],
  ['compositor.c', compositor],
]) {
  const match = source.match(
    /if\s*\(\s*info\.name\s*&&\s*strcmp\s*\(\s*info\.name\s*,\s*"ViewProj"\s*\)\s*==\s*0\s*\)\s*([\s\S]{0,80})\s*else\s*([\s\S]{0,80})/
  );
  assert(
    match,
    `${name} must branch specifically on ViewProj.`
  );
  assert(
    /gs_matrix_get\s*\(\s*&m\s*\)/.test(match[1]),
    `${name} must bind the current graphics matrix for ViewProj.`
  );
  assert(
    !/matrix4_identity\s*\(\s*&m\s*\)/.test(match[1]),
    `${name} must not reset ViewProj to identity.`
  );
}

console.log('ViewProj binding regression checks passed.');
