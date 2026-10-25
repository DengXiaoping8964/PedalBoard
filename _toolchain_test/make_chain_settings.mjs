/*
  Writes a PedalBoard.settings that puts every effect in the chain at once, with
  every parameter at one extreme, so the running app can be probed for "does each
  of the 22 pedals construct, prepare, render and survive its own limits" without
  clicking through an add-menu 22 times.

  The parameter list is read out of the effect headers themselves - the same
  { "Name", min, max, default, "unit" } rows the app compiles - so the file cannot
  drift away from the code the way a hand-written fixture would.

  usage:
    node _toolchain_test/make_chain_settings.mjs --mode max   [--out <path>]
    node _toolchain_test/make_chain_settings.mjs --mode min
    node _toolchain_test/make_chain_settings.mjs --mode default
*/
import { readFileSync, readdirSync, statSync, writeFileSync } from 'node:fs';
import { join, relative } from 'node:path';

const args = process.argv.slice(2);
const sourceDir = join('D:/PedalBoard', 'Source');

const mode = args.includes('--mode') ? args[args.indexOf('--mode') + 1] : 'max';
if (!['max', 'min', 'default'].includes(mode)) {
    console.error(`unknown mode ${mode}`);
    process.exit(2);
}

function* walk (dir) {
    for (const entry of readdirSync(dir)) {
        const full = join(dir, entry);
        if (statSync(full).isDirectory()) yield* walk(full);
        else if (full.endsWith('.h')) yield full;
    }
}

// 1. the numeric id of every effect, straight from the enum the file format uses.
const config = readFileSync(join(sourceDir, 'Common', 'PedalConfig.h'), 'utf8');
const enumBody = /enum EffectTypeID\s*:\s*int\s*\{([\s\S]*?)\}/.exec(config)?.[1] ?? '';
const typeNames = enumBody.split(',')
    .map(line => line.trim())
    .filter(line => line.length > 0 && !line.startsWith('//'))
    .map(line => line.split('=')[0].trim());

const numEffectTypes = typeNames.indexOf('numEffectTypes');
if (numEffectTypes < 0) {
    console.error('could not find numEffectTypes in the enum');
    process.exit(1);
}

// 2. the parameter rows of every effect class.
const paramsByType = new Map();

for (const file of walk(join(sourceDir, 'Effects'))) {
    const text = readFileSync(file, 'utf8');

    for (const [, type, rows] of text.matchAll(/PedalEffect\s*\(\s*(\w+)\s*,\s*"[^"]+"\s*,\s*\{([\s\S]*?)\}\s*\)/g)) {
        if (!typeNames.includes(type)) continue;

        const params = [];

        for (const [, , row] of rows.matchAll(/\{\s*"([^"]*)"([^}]*)\}/g)) {
            const numbers = row.match(/-?\d+\.?\d*f?/g) ?? [];

            if (numbers.length < 3) {
                console.error(`  ${type}: could not read min/max/default from { "${row.trim()}" }`);
                process.exit(1);
            }

            const value = s => parseFloat (s.replace(/f$/, ''));
            const [min, max, dflt] = numbers.map(value);
            params.push({ min, max, dflt });
        }

        paramsByType.set(type, params);
    }
}

const missing = typeNames.slice(0, numEffectTypes).filter(t => !paramsByType.has(t));

if (missing.length) {
    console.error(`no parameter block found for: ${missing.join(', ')}`);
    process.exit(1);
}

// 3. the file itself. The chain holds maxChainSlots (16) effects, so a full sweep of
//    all 22 has to be run in overlapping windows rather than in one file.
const start = args.includes('--start') ? parseInt (args[args.indexOf('--start') + 1], 10) : 0;
const count = args.includes('--count') ? parseInt (args[args.indexOf('--count') + 1], 10) : 16;

const chosen = typeNames.slice(start, Math.min (start + count, numEffectTypes));

const lines = [
    'inputType=', 'inputDevice=', 'outputType=', 'outputDevice=',
    'sampleRate=48000', 'bufferSize=480', 'monitor=1', 'globalWet=1.0000',
    'inputTrimDb=0.00', 'virtualOn=0', 'language=0', 'chosen=1', 'midiDevice=',
    `chain=${chosen.map((_, i) => start + i).join(',')}`,
];

for (const [offset, type] of chosen.entries()) {
    const values = paramsByType.get(type).map(p => {
        const v = mode === 'max' ? p.max : mode === 'min' ? p.min : p.dflt;
        return v.toFixed(5);
    });

    lines.push(`params.${start + offset}=${values.join('|')}|b0`);
}

const out = args.includes('--out') ? args[args.indexOf('--out') + 1]
                                   : 'D:/Temp/pb_chain_settings';
writeFileSync(out, lines.join('\n') + '\n');

const counts = [...paramsByType.entries()].map(([t, p]) => `${t}=${p.length}`);
console.log(`wrote ${out}: ${numEffectTypes} effects, mode=${mode}`);
console.log(`  params per effect: ${counts.join(' ')}`);
