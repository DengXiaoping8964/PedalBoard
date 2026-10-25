/*
  Checks that every pedal::tr / pedal::trf literal in Source has an entry in the
  Chinese table in Source/Common/PedalLanguage.cpp, and vice versa.

  A missing entry is silent at runtime: juce::translate returns the key unchanged,
  so the string simply stays English. This script is how that is caught without
  clicking through the whole UI in Chinese.

  usage:  node _toolchain_test/check_translations.mjs
*/
import { readdirSync, readFileSync, statSync } from 'node:fs';
import { join, relative } from 'node:path';

const root = process.argv[2] ?? 'D:/PedalBoard';
const sourceDir = join(root, 'Source');
const tablePath = join(sourceDir, 'Common', 'PedalLanguage.cpp');

function* walk (dir) {
    for (const entry of readdirSync(dir)) {
        const full = join(dir, entry);
        if (statSync(full).isDirectory()) yield* walk(full);
        else if (/\.cpp$|\.h$/.test(full)) yield full;
    }
}

/*  Reads the argument of a tr("a" "b") call: C++ lets a literal span several
    quoted pieces, so the key is the concatenation of every piece. */
function collectKeys (text) {
    const keys = [];
    const re = /\btrf?\s*\(\s*"/g;
    let m;

    while ((m = re.exec(text)) !== null) {
        let i = m.index + m[0].length - 1;   // sit on the opening quote
        let key = '';

        for (;;) {
            if (text[i] !== '"') break;
            ++i;
            let piece = '';
            while (i < text.length && text[i] !== '"') {
                if (text[i] === '\\') { piece += text[i + 1]; i += 2; continue; }
                piece += text[i];
                ++i;
            }
            key += piece;
            ++i;

            let j = i;
            while (j < text.length && /\s/.test(text[j])) ++j;
            if (text[j] !== '"') break;
            i = j;
        }

        keys.push(key);
    }

    return keys;
}

function collectTable (text) {
    const start = text.indexOf('R"ZH(');
    const end = text.indexOf(')ZH"', start);
    const body = text.slice(start + 5, end);
    const entries = [];

    for (const line of body.split('\n')) {
        const m = /^\s*"([^"]*)"\s*=\s*"([^"]*)"\s*$/.exec(line);
        if (m) entries.push({ key: m[1], value: m[2] });
        else if (/^\s*"/.test(line)) console.log(`  unparsed table line: ${line.trim()}`);
    }

    return entries;
}

/*  Every double-quoted literal in the sources. A table key can be reachable
    without sitting inside a tr() call: the effect names and parameter names are
    data (rows of EffectFactory's metadata table, ParamDescription initialisers)
    and reach the translator as a variable. Appearing anywhere is the honest test
    for "this string can actually be shown"; appearing in a tr() call is the test
    for "this string is translated right here". */
function collectAllLiterals (text) {
    const out = new Set();
    const re = /"((?:[^"\\\n]|\\.)*)"/g;
    let m;
    while ((m = re.exec(text)) !== null) out.add(m[1].replace(/\\"/g, '"'));
    return out;
}

const used = new Map();      // key -> [files], the direct tr()/trf() arguments
const present = new Set();   // every literal anywhere in Source

for (const file of walk(sourceDir)) {
    if (file === tablePath) continue;
    const text = readFileSync(file, 'utf8');
    const rel = relative(sourceDir, file);

    for (const key of collectKeys(text)) {
        if (! used.has(key)) used.set(key, []);
        used.get(key).push(rel);
    }

    for (const literal of collectAllLiterals(text)) present.add(literal);
}

const entries = collectTable(readFileSync(tablePath, 'utf8'));
const table = new Map(entries.map(e => [e.key, e.value]));

const dupes = entries.length - table.size;
const missing = [...used.keys()].filter(k => ! table.has(k)).sort();

// A key is reachable if it is a direct tr()/trf() argument or appears as a
// literal. The first test also covers the sentences written across several
// quoted pieces, which the single-line literal scan cannot see.
const unused = [...table.keys()].filter(k => ! used.has(k) && ! present.has(k)).sort();

const chinese = [...table.values()].filter(v => /[一-鿿]/.test(v)).length;

console.log(`keys in tr() calls  : ${used.size}`);
console.log(`literals in Source  : ${present.size}`);
console.log(`table entries       : ${entries.length}${dupes ? `  (${dupes} duplicate)` : ''}`);
console.log(`chinese values      : ${chinese}`);

if (missing.length) {
    console.log(`\nNO TRANSLATION (${missing.length}):`);
    for (const k of missing) console.log(`  "${k}"  <- ${[...new Set(used.get(k))].join(', ')}`);
}

if (unused.length) {
    console.log(`\nNOT USED ANYWHERE (${unused.length}):`);
    for (const k of unused) console.log(`  "${k}"`);
}

if (dupes) {
    const seen = new Set();
    console.log('\nDUPLICATE KEYS:');
    for (const e of entries) { if (seen.has(e.key)) console.log(`  "${e.key}"`); seen.add(e.key); }
}

process.exit(missing.length || unused.length || dupes ? 1 : 0);
