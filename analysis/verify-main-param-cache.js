const fs = require('fs');

const html = fs.readFileSync('web-ui/vjlink-control.html', 'utf8');

function assert(condition, message) {
  if (!condition) {
    console.error(message);
    process.exitCode = 1;
  }
}

assert(/let\s+mainParamCache\s*=\s*\{\s*\}/.test(html),
  'mainParamCache is missing');
assert(/function\s+getMainParamValue\s*\(/.test(html),
  'getMainParamValue helper is missing');
assert(/function\s+setMainParamValue\s*\(/.test(html),
  'setMainParamValue helper is missing');
assert(/const\s+cachedVal\s*=\s*getMainParamValue\(p\)/.test(html),
  'renderParams does not render from the cached main value');
assert(/setMainParamValue\('\$\{p\.name\}'\s*,\s*this\.selectedIndex\)/.test(html),
  'main dropdown changes are not cached');
assert(/setMainParamValue\('\$\{p\.name\}'\s*,\s*parseFloat\(this\.value\)\)/.test(html),
  'main slider changes are not cached');
assert(/main_params:\s*Object\.assign\(\{\},\s*getMainParamCache\(\)\)/.test(html),
  'profile capture does not use mainParamCache when the Main tab is hidden');
assert(/if\s*\(\s*activeEffect\s*&&\s*activeBandIndex\s*<\s*0\s*\)\s*\{[\s\S]*settings\.main_params\[p\.name\]/.test(html),
  'profile capture must not scrape visible band parameter controls into main_params');

if (!process.exitCode) {
  console.log('main param cache regression check passed');
}
