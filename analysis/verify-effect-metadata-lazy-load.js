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
  /function\s+loadSingleEffectMetadata\s*\(/.test(html),
  'Metadata loading must support per-effect lazy loading.'
);

assert(
  /function\s+loadEffectMetadataBatch\s*\(/.test(html),
  'Metadata loading must batch requests instead of flooding the plugin HTTP server.'
);

assert(
  /function\s+getPriorityEffectMetadataIds\s*\(/.test(html),
  'Metadata loading must prioritize visible/current effects before background effects.'
);

assert(
  /function\s+queueRemainingEffectMetadata\s*\(/.test(html),
  'Remaining metadata should be queued in the background.'
);

assert(
  !/await\s+Promise\.all\(\s*jobs\s*\)/.test(html),
  'Initial metadata loading must not wait for all effect metadata requests.'
);

assert(
  /loadEffectMetadataForEffect\(\s*effectId\s*\)/.test(html),
  'Selecting an effect must request its metadata lazily.'
);

console.log('Effect metadata lazy-load regression checks passed.');
