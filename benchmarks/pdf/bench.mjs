#!/usr/bin/env node
// Node built-ins only. No packages, downloads, app settings or document writes.
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import crypto from 'node:crypto';
import zlib from 'node:zlib';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const PROTOCOL = 'speedynote-pdf-v1';
const sha = bytes => crypto.createHash('sha256').update(bytes).digest('hex');
const readJson = file => JSON.parse(fs.readFileSync(file, 'utf8'));
const writeJson = (file, value) => fs.writeFileSync(file, JSON.stringify(value, null, 2) + '\n', { flag: 'wx' });
const assert = (condition, message) => { if (!condition) throw new Error(message); };

// Small deterministic PDF writer with byte-accurate xref offsets. Base-14 font;
// no wall-clock dates, platform fonts, external resources or private documents.
class Pdf {
  objects = [];
  add(value) { this.objects.push(Buffer.isBuffer(value) ? value : Buffer.from(value)); return this.objects.length; }
  stream(dictionary, data) {
    const compressed = zlib.deflateSync(Buffer.from(data), { level: 6 });
    return this.add(Buffer.concat([Buffer.from(`<< ${dictionary} /Length ${compressed.length} /Filter /FlateDecode >>\nstream\n`), compressed, Buffer.from('\nendstream')]));
  }
  finish() {
    const chunks = [Buffer.from('%PDF-1.4\n%\xE2\xE3\xCF\xD3\n', 'latin1')];
    const offsets = [0];
    let length = chunks[0].length;
    this.objects.forEach((object, index) => {
      offsets.push(length);
      const bytes = Buffer.concat([Buffer.from(`${index + 1} 0 obj\n`), object, Buffer.from('\nendobj\n')]);
      chunks.push(bytes); length += bytes.length;
    });
    const xref = length;
    const table = offsets.slice(1).map(offset => `${String(offset).padStart(10, '0')} 00000 n \n`).join('');
    chunks.push(Buffer.from(`xref\n0 ${offsets.length}\n0000000000 65535 f \n${table}trailer\n<< /Size ${offsets.length} /Root 1 0 R >>\nstartxref\n${xref}\n%%EOF\n`));
    return Buffer.concat(chunks);
  }
}

export function fixture(kind) {
  assert(['text', 'scan', 'vector', 'giant'].includes(kind), 'Unknown fixture kind');
  const pdf = new Pdf();
  pdf.add('<< /Type /Catalog /Pages 2 0 R >>');
  pdf.add('');
  const font = pdf.add('<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>');
  const count = kind === 'text' ? 24 : kind === 'scan' ? 12 : kind === 'giant' ? 3 : 8;
  const width = kind === 'giant' ? 2384 : 595;
  const height = kind === 'giant' ? 3370 : 842;
  let randomState = 0x51eed;
  const random = () => { randomState = (Math.imul(randomState, 1664525) + 1013904223) >>> 0; return randomState; };
  const pages = [];
  for (let p = 0; p < count; p++) {
    let content = `BT /F1 16 Tf 24 ${height - 28} Td (SpeedyNote ${kind} page ${p + 1}) Tj ET\n`;
    let image = 0;
    if (kind === 'scan') {
      const w = 1240, h = 1754;
      const pixels = Buffer.alloc(w * h * 3);
      for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
        const i = (y * w + x) * 3;
        const ink = y % 42 < 4 && x > 50 && x < 1150 && x % 47 < 36;
        const tone = ink ? 30 : 238 + (random() % 18);
        pixels[i] = tone; pixels[i + 1] = Math.max(0, tone - (p % 5)); pixels[i + 2] = tone;
      }
      image = pdf.stream(`/Type /XObject /Subtype /Image /Width ${w} /Height ${h} /ColorSpace /DeviceRGB /BitsPerComponent 8`, pixels);
      content += `q ${width - 32} 0 0 ${height - 64} 16 16 cm /Im0 Do Q\n`;
    } else if (kind === 'text') {
      for (let row = 0; row < 58; row++)
        content += `BT /F1 9 Tf 24 ${height - 55 - row * 13} Td (Page ${p + 1} line ${row + 1}: PDF rendering 0123456789 abcdefghijklmnopqrstuvwxyz.) Tj ET\n`;
    } else {
      for (let i = 0; i < 7000; i++) {
        const x = random() % (width - 40), y = random() % (height - 50);
        const d = 2 + random() % 28;
        content += `${(random() % 100) / 100} G 0.4 w ${x} ${y} m ${x + d} ${y + 2} l ${x + d} ${y + d} l S\n`;
      }
    }
    const stream = pdf.stream('', content);
    pages.push(pdf.add(`<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ${width} ${height}] /Resources << /Font << /F1 ${font} 0 R >> ${image ? `/XObject << /Im0 ${image} 0 R >>` : ''} >> /Contents ${stream} 0 R >>`));
  }
  pdf.objects[1] = Buffer.from(`<< /Type /Pages /Count ${count} /Kids [${pages.map(page => `${page} 0 R`).join(' ')}] >>`);
  return { bytes: pdf.finish(), count };
}

function generate(options) {
  const directory = path.resolve(options.out ?? 'out/pdf-benchmark/corpus');
  assert(!fs.existsSync(directory), `Output already exists: ${directory}`);
  fs.mkdirSync(directory, { recursive: true });
  const documents = [];
  for (const id of ['text', 'scan', 'vector', 'giant']) {
    const { bytes, count } = fixture(id);
    const file = `${id}.pdf`;
    fs.writeFileSync(path.join(directory, file), bytes, { flag: 'wx' });
    documents.push({ id, path: file, sha256: sha(bytes), pages: count, kind: id });
  }
  if (options.pdf) {
    const file = path.resolve(options.pdf);
    assert(fs.statSync(file).isFile(), 'Real PDF must be a file');
    documents.push({ id: 'real', path: file, sha256: sha(fs.readFileSync(file)), kind: 'real' });
  }
  const cases = [
    ['text-scroll', 'text', 'scroll', false], ['scan-scroll', 'scan', 'scroll', false],
    ['scan-dark', 'scan', 'scroll', true], ['vector-pan', 'vector', 'pan', false],
    ['vector-hand', 'vector', 'hand', false], ['giant-pan', 'giant', 'pan', false],
    ['scan-jump', 'scan', 'jump', false],
    ['vector-dark', 'vector', 'pan', true],
  ].map(([id, document, scenario, dark]) => ({ id, document, scenario, dark }));
  if (options.pdf) cases.push({ id: 'real-scroll', document: 'real', scenario: 'scroll', dark: false }, { id: 'real-pan', document: 'real', scenario: 'pan', dark: false });
  const manifest = { schema: 1, protocol: PROTOCOL, generator: sha(fs.readFileSync(fileURLToPath(import.meta.url))), node: process.version, zlib: process.versions.zlib, documents, cases };
  writeJson(path.join(directory, 'suite.json'), manifest);
  console.log(`Generated ${documents.length} documents, ${cases.length} cases: ${path.join(directory, 'suite.json')}`);
}

export function quantile(values, p) {
  assert(values.length > 0, 'Empty sample');
  assert(values.every(Number.isFinite) && p >= 0 && p <= 1, 'Invalid sample or quantile');
  const sorted = [...values].sort((a, b) => a - b);
  return sorted[Math.max(0, Math.ceil(sorted.length * p) - 1)];
}
export function summarize(values) {
  const middle = samples => {
    const sorted = [...samples].sort((a, b) => a - b), n = sorted.length;
    return n % 2 ? sorted[(n - 1) / 2] : (sorted[n / 2 - 1] + sorted[n / 2]) / 2;
  };
  assert(values.length > 0 && values.every(Number.isFinite), 'Invalid summary sample');
  const median = middle(values);
  return { n: values.length, median, min: Math.min(...values), max: Math.max(...values), mad: middle(values.map(v => Math.abs(v - median))) };
}

// Integrate last OBSERVED painted coverage until the next paint, including long
// gaps. A frozen but sharp frame is tracked by event-loop lateness, not white area.
export function coverageIntegral(frames, start, end, column = 2) {
  assert(Number.isFinite(start) && Number.isFinite(end) && end >= start, 'Invalid integration interval');
  let previous = 0, ratio = 1, areaKnown = false, total = 0, measured = 0;
  for (const row of [...frames, [end, 0, 1, 1, 0]]) {
    const at = Math.min(end, row[0]);
    assert(Number.isFinite(at) && at >= previous, 'Non-monotonic frame trace');
    const duration = Math.max(0, at - Math.max(previous, start));
    if (areaKnown) { total += duration * ratio; measured += duration; }
    previous = at;
    if (row[0] >= end) break;
    ratio = row[column];
    assert(Number.isFinite(ratio) && ratio >= 0 && ratio <= 1, 'Invalid coverage ratio');
    areaKnown = row[4] > 0;
  }
  return { area_ms: total, measured_ms: measured, fraction: measured ? total / measured : null };
}

export function enrich(result) {
  assert(result.schema === 1 && result.protocol === PROTOCOL, 'Unknown result protocol');
  const output = structuredClone(result);
  if (result.coverage_enabled && Number.isFinite(result.motion_start_ms) && Number.isFinite(result.motion_end_ms)) {
    const missing = coverageIntegral(result.frames, result.motion_start_ms, result.measurement_end_ms);
    const unsharp = coverageIntegral(result.frames, result.motion_start_ms, result.measurement_end_ms, 3);
    output.metrics.unpainted_area_ms = missing.area_ms;
    output.metrics.unsharp_area_ms = unsharp.area_ms;
    output.metrics.mean_unpainted_fraction = missing.fraction;
    output.metrics.coverage_measured_ms = missing.measured_ms;
    const returning = result.phases.find(p => p.name === 'return')?.at_ms;
    const pause = result.phases.find(p => p.name === 'pause')?.at_ms;
    if (returning !== undefined) {
      output.metrics.forward_unpainted_area_ms = coverageIntegral(result.frames, result.motion_start_ms, pause ?? returning).area_ms;
      output.metrics.return_unpainted_area_ms = coverageIntegral(result.frames, returning, result.motion_end_ms).area_ms;
    }
  }
  return output;
}

// Fingerprint the deployed Windows runtime once before a campaign. A/B binaries
// can differ; changing a DLL or plugin makes performance attribution ambiguous.
function runtimeFingerprint(directory) {
  const entries = [];
  const visit = folder => {
    for (const entry of fs.readdirSync(folder, { withFileTypes: true })) {
      const file = path.join(folder, entry.name);
      if (entry.isDirectory()) visit(file);
      else if (entry.isFile() && /\.dll$/i.test(entry.name))
        entries.push([path.relative(directory, file).replaceAll('\\', '/'), sha(fs.readFileSync(file))]);
    }
  };
  visit(directory);
  return entries.length ? sha(JSON.stringify(entries.sort((a, b) => a[0].localeCompare(b[0])))) : null;
}

function run(options) {
  assert(options.exe && options.suite && options.out, 'run requires --exe, --suite and --out');
  const suitePath = path.resolve(options.suite), suite = readJson(suitePath);
  assert(suite.protocol === PROTOCOL && suite.schema === 1, 'Unknown suite protocol');
  const output = path.resolve(options.out);
  assert(!fs.existsSync(output), `Output already exists: ${output}`);
  const repeat = Number(options.repeat ?? 5);
  assert(Number.isInteger(repeat) && repeat >= 1 && repeat <= 100, '--repeat must be 1..100');
  const cases = suite.cases.filter(test => !options.case || test.id === options.case);
  assert(cases.length, 'No matching cases');
  const binaries = [{ label: options.label ?? 'baseline', path: path.resolve(options.exe) }];
  if (options.against) binaries.push({ label: 'candidate', path: path.resolve(options.against) });
  assert(new Set(binaries.map(b => b.label)).size === binaries.length, 'Labels must differ');
  for (const binary of binaries) {
    assert(fs.statSync(binary.path).isFile(), 'Executable is not a file');
    binary.sha256 = sha(fs.readFileSync(binary.path));
    binary.runtime_sha256 = runtimeFingerprint(path.dirname(binary.path));
  }
  // Deliberate preflight read ONCE per campaign. This is not an OS-cold test.
  for (const doc of suite.documents) {
    doc.absolutePath = path.resolve(path.dirname(suitePath), doc.path);
    assert(sha(fs.readFileSync(doc.absolutePath)) === doc.sha256, `Corpus changed: ${doc.id}`);
  }
  fs.mkdirSync(output, { recursive: true });
  const machine = { hostname: os.hostname(), cpu: os.cpus()[0]?.model ?? 'unknown', threads: os.cpus().length, total_memory: os.totalmem(), os: `${os.platform()} ${os.release()}`, arch: os.arch() };
  const runnerHash = sha(fs.readFileSync(fileURLToPath(import.meta.url)));
  const campaign = { schema: 1, protocol: PROTOCOL, runner_sha256: runnerHash, machine, binaries, repeats: repeat, preflight: 'read_each_document_once_os_cache_not_flushed', cases, runs: [] };
  let failed = 0;
  for (const test of cases) for (let iteration = 0; iteration < repeat; iteration++) {
    // A B / B A alternation, paired within each case, reduces order drift.
    const order = iteration % 2 ? [...binaries].reverse() : binaries;
    for (const binary of order) {
      const doc = suite.documents.find(d => d.id === test.document);
      assert(doc, `Missing document for ${test.id}`);
      const stem = `${test.id}-${binary.label}-${iteration + 1}`;
      assert(/^[a-zA-Z0-9_.-]+$/.test(stem), 'Unsafe case/label name');
      const nativeFile = path.join(output, `${stem}.native.json`);
      const args = ['--benchmark-pdf', doc.absolutePath, '--benchmark-output', nativeFile, '--benchmark-scenario', test.scenario];
      if (test.dark) args.push('--benchmark-dark');
      if (options['no-coverage']) args.push('--benchmark-no-coverage');
      console.log(`${stem} ...`);
      // libuv maps windowsHide to SW_HIDE for GUI windows too. This benchmark
      // needs an exposed desktop window to receive paint events.
      const child = spawnSync(binary.path, args, { cwd: path.dirname(binary.path), timeout: 60000, windowsHide: false, encoding: 'utf8', maxBuffer: 4 * 1024 * 1024 });
      let result;
      try {
        result = fs.existsSync(nativeFile) ? enrich(readJson(nativeFile)) : { schema: 1, protocol: PROTOCOL, status: 'no_result', metrics: {} };
      } catch (error) {
        result = { schema: 1, protocol: PROTOCOL, status: 'invalid_result', metrics: {}, error: error.message };
      }
      if (child.status !== 0 && result.status === 'ok') result.status = 'process_failed';
      result.run = { case: test.id, label: binary.label, iteration: iteration + 1, pdf_sha256: doc.sha256, binary_sha256: binary.sha256, runtime_sha256: binary.runtime_sha256, runner_sha256: runnerHash, machine, exit_code: child.status, signal: child.signal, error: child.error?.message ?? null, preflight: campaign.preflight };
      writeJson(path.join(output, `${stem}.json`), result);
      if (child.stderr || child.stdout) fs.writeFileSync(path.join(output, `${stem}.log`), `${child.stdout ?? ''}\n${child.stderr ?? ''}`, { flag: 'wx' });
      campaign.runs.push(`${stem}.json`);
      writeCampaignCheckpoint(output, campaign);
      if (result.status !== 'ok') { failed++; console.error(`${stem}: ${result.status}`); }
    }
  }
  console.log(`Finished: ${campaign.runs.length} runs, ${failed} unsuccessful. ${output}`);
  if (failed) process.exitCode = 2;
}

function writeCampaignCheckpoint(directory, data) {
  // Only replaces this campaign's own checkpoint, never an earlier result.
  const temp = path.join(directory, 'campaign.tmp');
  fs.writeFileSync(temp, JSON.stringify(data, null, 2) + '\n');
  fs.renameSync(temp, path.join(directory, 'campaign.json'));
}

export function compatibility(result) {
  const e = result.environment ?? {}, r = result.run ?? {};
  return JSON.stringify({ protocol: result.protocol, instrumentation: result.instrumentation,
    scenario: result.scenario, dark: result.dark, zoom: result.zoom, coverage: result.coverage_enabled,
    pdf: r.pdf_sha256, machine: r.machine, preflight: r.preflight,
    runtime: r.runtime_sha256, runner: r.runner_sha256,
    environment: e });
}

function loadRuns(directory, label) {
  const campaign = readJson(path.join(directory, 'campaign.json'));
  assert(campaign.binaries.some(binary => binary.label === label), `Unknown campaign label: ${label}`);
  const runs = campaign.runs.map(file => readJson(path.join(directory, file))).filter(run => run.run.label === label);
  // An interrupted campaign may contain only successful early samples. Keep
  // missing repetitions visible so comparison cannot quietly select survivors.
  for (const test of campaign.cases) for (let iteration = 1; iteration <= campaign.repeats; iteration++) {
    const matches = runs.filter(run => run.run.case === test.id && run.run.iteration === iteration);
    assert(matches.length <= 1, 'Duplicate campaign iteration');
    if (!matches.length) runs.push({ status: 'missing_run', metrics: {}, run: { case: test.id, label, iteration } });
  }
  return runs;
}

export function compareRuns(baseline, candidate) {
  const rows = [], skipped = [];
  const cases = new Set([...baseline, ...candidate].map(run => run.run.case));
  for (const name of cases) {
    const a = baseline.filter(r => r.run.case === name), b = candidate.filter(r => r.run.case === name);
    if (!a.length || !b.length || [...a, ...b].some(r => r.status !== 'ok')) {
      skipped.push({ case: name, reason: 'missing or unsuccessful run', baseline_status: a.map(r => r.status), candidate_status: b.map(r => r.status) }); continue;
    }
    const key = compatibility(a[0]);
    if ([...a, ...b].some(r => compatibility(r) !== key)) {
      skipped.push({ case: name, reason: 'incompatible corpus, instrumentation, device, display, library or mode' }); continue;
    }
    for (const metric of Object.keys(a[0].metrics)) {
      if ([...a, ...b].some(r => !Number.isFinite(r.metrics[metric]))) continue;
      const av = summarize(a.map(r => r.metrics[metric])), bv = summarize(b.map(r => r.metrics[metric]));
      const delta = bv.median - av.median;
      rows.push({ case: name, metric, baseline: av, candidate: bv, delta,
        change_percent: av.median === 0 ? null : 100 * delta / av.median,
        // Descriptive noise indicator, NOT a significance test or pass/fail gate.
        within_observed_mad: Math.abs(delta) <= Math.max(av.mad, bv.mad) });
    }
  }
  return { schema: 1, rows, skipped, interpretation: 'Per-run median, min/max and MAD; negative delta means less. Five runs are exploratory, not proof of statistical significance.' };
}

function compare(options) {
  assert(options.baseline && options.candidate && options.out, 'compare requires --baseline, --candidate and --out');
  const a = loadRuns(path.resolve(options.baseline), options['baseline-label'] ?? 'baseline');
  const b = loadRuns(path.resolve(options.candidate), options['candidate-label'] ?? 'candidate');
  assert(a.length && b.length, 'No runs for requested labels');
  const report = compareRuns(a, b);
  writeJson(path.resolve(options.out), report);
  console.log('| Case | Metric | Baseline median | Candidate median | Change |');
  console.log('| --- | --- | ---: | ---: | ---: |');
  const wanted = new Set(['first_content_ms', 'first_sharp_ms', 'unpainted_area_ms', 'final_settle_ms', 'event_loop_p99_ms', 'cpu_ms', 'process_peak_rss_bytes']);
  for (const row of report.rows.filter(row => wanted.has(row.metric)))
    console.log(`| ${row.case} | ${row.metric} | ${row.baseline.median.toFixed(2)} | ${row.candidate.median.toFixed(2)} | ${row.change_percent === null ? 'n/a (zero baseline)' : row.change_percent.toFixed(1) + '%'} |`);
  for (const item of report.skipped) console.error(`Not compared: ${item.case}: ${item.reason}`);
  if (report.skipped.length) process.exitCode = 2;
}

function main() {
  const [command, ...args] = process.argv.slice(2);
  const allowed = {
    generate: ['out', 'pdf'],
    run: ['exe', 'suite', 'out', 'repeat', 'case', 'label', 'against', 'no-coverage'],
    compare: ['baseline', 'candidate', 'out', 'baseline-label', 'candidate-label'],
  };
  assert(allowed[command], 'Usage: node bench.mjs generate|run|compare [options]. See README.md.');
  const options = {};
  for (let i = 0; i < args.length; i++) {
    assert(args[i].startsWith('--'), `Unexpected argument ${args[i]}`);
    const key = args[i].slice(2);
    assert(allowed[command].includes(key) && !(key in options), `Unknown or duplicate option --${key}`);
    if (key === 'no-coverage') options[key] = true;
    else { assert(i + 1 < args.length && !args[i + 1].startsWith('--'), `Missing value for --${key}`); options[key] = args[++i]; }
  }
  if (command === 'generate') generate(options);
  else if (command === 'run') run(options);
  else if (command === 'compare') compare(options);
  else console.log('Usage: node benchmarks/pdf/bench.mjs generate|run|compare [options]. See benchmarks/pdf/README.md.');
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { main(); } catch (error) { console.error(error.message); process.exitCode = 1; }
}
