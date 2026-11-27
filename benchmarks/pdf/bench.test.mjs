import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import crypto from 'node:crypto';
import zlib from 'node:zlib';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { fixture, quantile, summarize, coverageIntegral, enrich, compareRuns } from './bench.mjs';

const cli = fileURLToPath(new URL('./bench.mjs', import.meta.url));
const hash = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const frame = (at, missing, area = 100, unsharp = missing) => [at, 1, missing, unsharp, area, 1, .01];
function result(value = 100) {
  return {
    schema: 1, protocol: 'speedynote-pdf-v1', status: 'ok', instrumentation: 'probe-1',
    scenario: 'scroll', dark: false, zoom: 1, coverage_enabled: true,
    motion_start_ms: 100, motion_end_ms: 600, measurement_end_ms: 700,
    phases: [], frames: [frame(0, 0), frame(100, .5), frame(600, 0)],
    environment: { qt: 'test-only', dpr: 1 }, metrics: { cpu_ms: value },
    run: { case: 'text-scroll', pdf_sha256: 'pdf', machine: { cpu: 'test' },
      runner_sha256: 'runner', runtime_sha256: 'runtime', preflight: 'read_once' },
  };
}

// This checks PDF structure, not successful rendering by MuPDF. Native opening
// remains a separate smoke check on the compiled desktop program.
function checkPdf(bytes, count, kind) {
  const text = bytes.toString('latin1');
  const xrefOffset = Number(/startxref\n(\d+)\n%%EOF\n$/.exec(text)[1]);
  assert.equal(text.slice(xrefOffset, xrefOffset + 5), 'xref\n');
  const lines = text.slice(xrefOffset).split('\n');
  const size = Number(lines[1].split(' ')[1]);
  let pageCount = 0, images = 0, streams = 0;
  for (let id = 1; id < size; id++) {
    const offset = Number(lines[id + 2].slice(0, 10));
    assert.ok(text.slice(offset).startsWith(`${id} 0 obj\n`));
    const end = id + 1 < size ? Number(lines[id + 3].slice(0, 10)) : xrefOffset;
    const object = text.slice(offset, end);
    if (/\/Type \/Page\s/.test(object)) pageCount++;
    if (!object.includes('\nstream\n')) continue;
    streams++;
    const length = Number(/\/Length (\d+)/.exec(object)[1]);
    const start = offset + object.indexOf('\nstream\n') + 8;
    assert.equal(text.slice(start + length, start + length + 10), '\nendstream');
    const decoded = zlib.inflateSync(bytes.subarray(start, start + length));
    if (/\/Subtype \/Image/.test(object)) {
      images++;
      assert.equal(decoded.length, 1240 * 1754 * 3);
    } else {
      assert.match(decoded.toString(), /SpeedyNote/);
    }
  }
  assert.equal(pageCount, count);
  assert.equal(images, kind === 'scan' ? count : 0);
  assert.equal(streams, count + images);
  assert.match(text, new RegExp(`/Type /Pages /Count ${count} `));
}

test('deterministic PDF generation has stable bytes and correct object offsets', () => {
  for (const kind of ['text', 'vector', 'giant']) {
    const a = fixture(kind), b = fixture(kind);
    assert.equal(hash(a.bytes), hash(b.bytes));
    checkPdf(a.bytes, a.count, kind);
  }
  assert.throws(() => fixture('typo'), /Unknown/);
});

test('coverage integral retains long blank intervals and respects boundaries', () => {
  const frames = [frame(0, 0), frame(100, 1), frame(1100, .5), frame(1200, 0)];
  assert.deepEqual(coverageIntegral(frames, 50, 1300), { area_ms: 1050, measured_ms: 1250, fraction: 1050 / 1250 });
  assert.equal(coverageIntegral(frames, 150, 200).area_ms, 50);
  assert.equal(coverageIntegral(frames, 0, 100).area_ms, 0);
  assert.equal(coverageIntegral([frame(0, 0)], 0, 2000).area_ms, 0); // frozen sharp image
});

test('no PDF area is excluded, malformed traces and intervals are rejected', () => {
  assert.deepEqual(coverageIntegral([frame(0, 1, 0), frame(100, .5)], 0, 200), { area_ms: 50, measured_ms: 100, fraction: .5 });
  assert.equal(coverageIntegral([], 0, 100).fraction, null);
  assert.throws(() => coverageIntegral([frame(100, 1), frame(50, 0)], 0, 200), /Non-monotonic/);
  assert.throws(() => coverageIntegral([frame(0, 1.1)], 0, 200), /Invalid coverage/);
  assert.throws(() => coverageIntegral([], 100, 99), /Invalid integration/);
});

test('content, sharpness, forward and return exposure are separate', () => {
  const r = result();
  r.frames = [frame(0, 0), frame(100, .5, 100, 1), frame(400, 0, 100, .5), frame(600, 0)];
  r.phases = [{ name: 'return', at_ms: 400 }];
  const enriched = enrich(r);
  assert.equal(enriched.metrics.unpainted_area_ms, 150);
  assert.equal(enriched.metrics.unsharp_area_ms, 400);
  assert.equal(enriched.metrics.forward_unpainted_area_ms, 150);
  assert.equal(enriched.metrics.return_unpainted_area_ms, 0);
  assert.equal(r.metrics.unpainted_area_ms, undefined);
  r.coverage_enabled = false;
  assert.equal(enrich(r).metrics.unpainted_area_ms, undefined);
});

test('summary uses conventional median/MAD, never pools frame percentiles', () => {
  assert.deepEqual(summarize([10, 30]), { n: 2, median: 20, min: 10, max: 30, mad: 10 });
  assert.equal(quantile([1, 2, 3, 1000], .99), 1000);
  assert.throws(() => summarize([]), /Invalid/);
  assert.throws(() => quantile([NaN], .5), /Invalid/);
  const report = compareRuns([result(10), result(100), result(20)], [result(5), result(25), result(15)]);
  assert.equal(report.rows[0].baseline.median, 20);
  assert.equal(report.rows[0].candidate.median, 15);
  assert.equal(report.rows[0].change_percent, -25);
  assert.equal(compareRuns([result(0)], [result(5)]).rows[0].change_percent, null);
});

test('a timeout or missing side prevents a success-only comparison', () => {
  const failed = result(); failed.status = 'final_timeout';
  assert.equal(compareRuns([result()], [result(), failed]).rows.length, 0);
  assert.equal(compareRuns([result()], [failed]).skipped.length, 1);
  assert.equal(compareRuns([result()], []).skipped.length, 1);
});

test('display, corpus, probe, runner, runtime and hardware differences block comparisons', () => {
  const modifications = [
    r => { r.environment.dpr = 2; }, r => { r.run.pdf_sha256 = 'different'; },
    r => { r.instrumentation = 'different'; }, r => { r.run.runner_sha256 = 'different'; },
    r => { r.run.runtime_sha256 = 'different'; }, r => { r.run.machine.cpu = 'different'; },
    r => { r.coverage_enabled = false; }, r => { r.dark = true; },
  ];
  for (const change of modifications) {
    const candidate = result(); change(candidate);
    assert.equal(compareRuns([result()], [candidate]).skipped.length, 1);
  }
  const candidate = result(); candidate.run.binary_sha256 = 'new implementation';
  assert.equal(compareRuns([result()], [candidate]).skipped.length, 0);
});

test('CLI generates corpus, preserves real input, refuses overwrite, records failed processes', () => {
  const tempRoot = fs.realpathSync(os.tmpdir());
  const directory = fs.mkdtempSync(path.join(tempRoot, 'speedynote-pdf-test-'));
  try {
    const real = path.join(directory, 'user.pdf'), original = fixture('text').bytes;
    fs.writeFileSync(real, original);
    const corpus = path.join(directory, 'corpus');
    const call = args => spawnSync(process.execPath, [cli, ...args], { encoding: 'utf8', timeout: 30000 });
    let child = call(['generate', '--out', corpus, '--pdf', real]);
    assert.equal(child.status, 0, child.stderr);
    const manifest = JSON.parse(fs.readFileSync(path.join(corpus, 'suite.json')));
    assert.equal(manifest.documents.length, 5);
    assert.equal(manifest.cases.length, 10);
    for (const doc of manifest.documents.filter(d => d.kind !== 'real')) {
      const bytes = fs.readFileSync(path.join(corpus, doc.path));
      assert.equal(hash(bytes), doc.sha256);
      checkPdf(bytes, doc.pages, doc.kind);
    }
    assert.equal(hash(fs.readFileSync(real)), hash(original));
    const panSuite = path.join(directory, 'pan-suite.json');
    const panCli = fileURLToPath(new URL('./pan-suite.mjs', import.meta.url));
    const panArgs = [panCli, path.join(corpus, 'suite.json'), panSuite];
    child = spawnSync(process.execPath, panArgs, { encoding: 'utf8', timeout: 30000 });
    assert.equal(child.status, 0, child.stderr);
    const pan = JSON.parse(fs.readFileSync(panSuite));
    assert.equal(pan.cases.length, manifest.documents.length * 3);
    assert.ok(pan.cases.some(c => c.id === 'real-hand-read' && c.scenario === 'hand-read'));
    assert.ok(pan.cases.some(c => c.id === 'real-hand-scroll' && c.scenario === 'hand-scroll'));
    for (const doc of pan.documents)
      assert.equal(hash(fs.readFileSync(path.resolve(path.dirname(panSuite), doc.path))), doc.sha256);
    assert.notEqual(spawnSync(process.execPath, panArgs, { encoding: 'utf8' }).status, 0);
    assert.equal(fs.readFileSync(path.join(corpus, 'suite.json'), 'utf8'), JSON.stringify(manifest, null, 2) + '\n');
    assert.notEqual(call(['generate', '--out', corpus]).status, 0);
    assert.notEqual(call(['generate', '--ot', corpus]).status, 0);
    const output = path.join(directory, 'runs');
    // Node is deliberately NOT a benchmark executable. Verify nonzero exits
    // produce failed samples and that the second checkpoint replaces the first.
    child = call(['run', '--exe', process.execPath, '--suite', path.join(corpus, 'suite.json'), '--out', output, '--case', 'text-scroll', '--repeat', '2']);
    assert.equal(child.status, 2, child.stderr);
    const campaign = JSON.parse(fs.readFileSync(path.join(output, 'campaign.json')));
    assert.equal(campaign.runs.length, 2);
    assert.equal(JSON.parse(fs.readFileSync(path.join(output, campaign.runs[0]))).status, 'no_result');
    // Simulate an interruption after one successful iteration out of two.
    const early = result(); early.run.label = 'baseline'; early.run.iteration = 1;
    fs.writeFileSync(path.join(output, campaign.runs[0]), JSON.stringify(early));
    campaign.runs.length = 1;
    fs.writeFileSync(path.join(output, 'campaign.json'), JSON.stringify(campaign));
    const reportPath = path.join(directory, 'comparison.json');
    child = call(['compare', '--baseline', output, '--candidate', output, '--candidate-label', 'baseline', '--out', reportPath]);
    assert.equal(child.status, 2, child.stderr);
    const report = JSON.parse(fs.readFileSync(reportPath));
    assert.equal(report.rows.length, 0);
    assert.ok(report.skipped[0].baseline_status.includes('missing_run'));
  } finally {
    assert.equal(path.dirname(fs.realpathSync(directory)), tempRoot);
    fs.rmSync(directory, { recursive: true });
  }
});
