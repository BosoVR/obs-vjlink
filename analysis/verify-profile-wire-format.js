const fs = require('fs');
const path = require('path');

const htmlPath = path.join(__dirname, '..', 'web-ui', 'vjlink-control.html');
const html = fs.readFileSync(htmlPath, 'utf8');

function assert(condition, message) {
  if (!condition) {
    console.error('FAIL:', message);
    process.exit(1);
  }
}

assert(
  /function\s+extractProfileSettingsFromResponse\s*\(/.test(html),
  'Profile loading must parse backend LoadProfileV2 responses, including state_json.'
);

assert(
  /function\s+buildProfileSavePayload\s*\(/.test(html),
  'Profile saving must use one canonical payload builder.'
);

assert(
  /state_json\s*:\s*JSON\.stringify\(\s*snapshot\s*\)/.test(html) ||
    /const\s+state_json\s*=\s*JSON\.stringify\(\s*snapshot\s*\)/.test(html),
  'SaveProfileV2 must include state_json so the C backend receives an unambiguous JSON payload.'
);

assert(
  /extractProfileSettingsFromResponse\(\s*data\s*,\s*name\s*\)/.test(html),
  'loadProfile must use extractProfileSettingsFromResponse(data, name).'
);

assert(
  /saveLegacyProfilesToBackend[\s\S]*buildProfileSavePayload\(\s*name\s*,\s*snapshot\s*\)/.test(html),
  'Legacy profile import must send the same canonical payload shape.'
);

console.log('Profile wire-format regression checks passed.');
