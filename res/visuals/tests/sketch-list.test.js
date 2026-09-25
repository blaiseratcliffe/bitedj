// The plain-data sketch list at the top of sketches.js is what the admin
// service shows in its library, without running any JavaScript. This checks
// it against the sketch objects themselves: same names, same order, same
// flags. Every sketch object carries all its flags on the line that opens
// it (`{ name: 'x', cam: false, ..., run() {`), which is what makes a
// textual check possible here without loading hydra.
//
// Run from bitedj/:  node --test res/visuals/tests/sketch-list.test.js
'use strict';
const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');

const SKETCHES = path.join(__dirname, '..', 'sketches.js');
const FLAG_ORDER = ['cam', 'pattern', 'video', 'camMix', 'vidMix'];

// The same parse pi/bin/bitedj-visuals-admin does (contract 1.2): the text
// between the marker lines, after the first '=', before the last ';'.
function readList(text) {
  const lines = text.split(/\r?\n/);
  const begin = lines.findIndex(l => l.includes('SKETCH LIST BEGIN'));
  const end = lines.findIndex(l => l.includes('SKETCH LIST END'));
  if (begin < 0 || end < 0 || end < begin) throw new Error('SKETCH LIST BEGIN/END markers not found');
  const block = lines.slice(begin + 1, end).join('\n');
  const body = block.slice(block.indexOf('=') + 1, block.lastIndexOf(';'));
  return JSON.parse(body);
}

// Every rotation sketch object, in file order, with its flags. The idle
// sketch opens with `window.idleSketch = {` and is left out.
function readObjects(text) {
  const out = [];
  const re = /^\s*\{ name: '([^']+)',(.*)run\(\) \{/;
  text.split(/\r?\n/).forEach((line) => {
    const m = re.exec(line);
    if (!m) return;
    const rest = m[2];
    const flags = [];
    if (/\bcam: true\b/.test(rest)) flags.push('cam');
    if (/\bpattern: true\b/.test(rest)) flags.push('pattern');
    if (/\bvideo: true\b/.test(rest)) flags.push('video');
    if (/\bcamMix: '/.test(rest)) flags.push('camMix');
    if (/\bvidMix: '/.test(rest)) flags.push('vidMix');
    out.push({ name: m[1], flags });
  });
  return out;
}

const canonical = (flags) => FLAG_ORDER.filter(f => flags.includes(f));

test('the sketch list parses as plain JSON', () => {
  const list = readList(fs.readFileSync(SKETCHES, 'utf8'));
  assert.ok(Array.isArray(list));
  list.forEach((s) => {
    assert.equal(typeof s.name, 'string');
    assert.ok(Array.isArray(s.flags), s.name + ' has no flags array');
    s.flags.forEach(f => assert.ok(FLAG_ORDER.includes(f), s.name + ': unknown flag ' + f));
  });
});

test('the sketch list matches the sketch objects, in order', () => {
  const text = fs.readFileSync(SKETCHES, 'utf8');
  const list = readList(text).map(s => ({ name: s.name, flags: canonical(s.flags) }));
  const objects = readObjects(text).map(s => ({ name: s.name, flags: canonical(s.flags) }));
  assert.equal(objects.length, 27, 'expected 27 rotation sketch objects');
  assert.deepEqual(list, objects);
});

test('the idle sketch is not in the list', () => {
  const list = readList(fs.readFileSync(SKETCHES, 'utf8'));
  assert.ok(!list.some(s => s.name === 'idle-contours'));
});
