// Derive touch-pan cases from an existing corpus without regenerating PDFs.
import fs from 'node:fs';
import path from 'node:path';
const [input, output] = process.argv.slice(2);
if (!input || !output) throw new Error('Usage: node pan-suite.mjs existing-suite.json new-suite.json');
const source = path.resolve(input), target = path.resolve(output);
const suite = JSON.parse(fs.readFileSync(source, 'utf8'));
if (suite.schema !== 1 || suite.protocol !== 'speedynote-pdf-v1') throw new Error('Unknown suite');
const cases = [];
for (const document of suite.documents) {
  document.path = path.relative(path.dirname(target), path.resolve(path.dirname(source), document.path));
  // Same input file and hashes; only gesture type and trajectory change.
  for (const [suffix, scenario] of [['fast', 'hand'], ['read', 'hand-read'], ['scroll', 'hand-scroll']])
    cases.push({ id: `${document.id}-hand-${suffix}`, document: document.id, scenario, dark: false });
}
suite.cases = cases;
fs.writeFileSync(target, JSON.stringify(suite, null, 2) + '\n', { flag: 'wx' });
console.log(`Wrote ${cases.length} pan cases to ${target}`);
