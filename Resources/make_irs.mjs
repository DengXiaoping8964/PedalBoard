/*
  PedalBoard - built-in impulse response generator.

  Why this exists as a script and not as three committed .wav files: the cabinet
  simulator needs three responses to be useful out of the box (1x12 V30 closed,
  2x12 open back, 4x12 high gain), they have to be real audio the convolution can
  actually load, and a synthetic response is honest about what it is. These are
  not measured microphone-on-cabinet captures. They are filter-shaped noise
  bursts with a decaying envelope, a direct arrival and a few early reflections,
  which is the same construction a simple cabinet model uses: it gives the right
  band balance and the right short time response, and it is audibly different from
  its neighbours. For real cabinet tones, load a measured IR from the card.

  Run:
      node Resources/make_irs.mjs

  Writes into Resources/IRs/, which is the folder juce_add_binary_data embeds.
  No dependencies: Node's own Buffer writes the RIFF header and the samples.
*/

import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const sampleRate = 44100;
const here = dirname(fileURLToPath(import.meta.url));
const outDir = join(here, 'IRs');

/* ---------------------------------------------------------------- utilities */

/** mulberry32: a deterministic PRNG, so regenerating the files twice produces
    byte-identical WAVs and the build never sees a spurious change. */
function makeRandom (seed)
{
    let a = seed >>> 0;
    return () =>
    {
        a = (a + 0x6D2B79F5) >>> 0;
        let t = Math.imul (a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul (t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

/** RBJ cookbook biquads, the same set the C++ side uses in PedalDsp.h, so the
    tone-shaping here matches the filters the live equaliser runs. */
function biquad (kind, f0, qOrGain, sr = sampleRate)
{
    const w0 = 2 * Math.PI * Math.min (Math.max (f0, 10), sr * 0.45) / sr;
    const cosW = Math.cos (w0);

    // 'peak' takes dB of gain and a fixed Q; the slopes take Q directly.
    const alpha = Math.sin (w0) / (2 * (kind === 'peak' ? 1.6 : qOrGain));

    let b0, b1, b2, a0, a1, a2;

    if (kind === 'lowpass')
    {
        b0 = (1 - cosW) / 2; b1 = 1 - cosW; b2 = b0;
        a0 = 1 + alpha; a1 = -2 * cosW; a2 = 1 - alpha;
    }
    else if (kind === 'highpass')
    {
        b0 = (1 + cosW) / 2; b1 = -(1 + cosW); b2 = b0;
        a0 = 1 + alpha; a1 = -2 * cosW; a2 = 1 - alpha;
    }
    else
    {
        const a = Math.pow (10, qOrGain / 40);   // RBJ: A = 10 ^ (dB / 40)
        b0 = 1 + alpha * a; b1 = -2 * cosW; b2 = 1 - alpha * a;
        a0 = 1 + alpha / a; a1 = -2 * cosW; a2 = 1 - alpha / a;
    }

    return { b: [b0 / a0, b1 / a0, b2 / a0], a: [a1 / a0, a2 / a0] };
}

function filter (samples, coeffs)
{
    const [b0, b1, b2] = coeffs.b;
    const [a1, a2] = coeffs.a;

    let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    const out = new Float32Array (samples.length);

    for (let i = 0; i < samples.length; ++i)
    {
        const x0 = samples[i];
        const y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        out[i] = y0;
        x2 = x1; x1 = x0;
        y2 = y1; y1 = y0;
    }

    return out;
}

/* ------------------------------------------------------------ one response */

function generate (spec)
{
    const length = Math.round (spec.seconds * sampleRate);
    const channels = [];

    // Two independent noise seeds make a genuine stereo image; a single response
    // copied to both sides would collapse every stereo effect downstream to mono.
    for (let c = 0; c < 2; ++c)
    {
        const random = makeRandom (spec.seed + c * 7919);
        const drive = new Float32Array (length);

        for (let i = 0; i < length; ++i)
            drive[i] = random () * 2 - 1;

        let shaped = drive;

        for (const stage of spec.stages)
            shaped = filter (shaped, biquad (stage.kind, stage.hz, stage.value));

        // Exponential decay plus a soft end, so trimming never leaves a step.
        const tau = Math.max (1, spec.decaySeconds * sampleRate);

        for (let i = 0; i < length; ++i)
        {
            const envelope = Math.exp (-i / tau);
            const fadeOut = i > length - 64 ? (length - i) / 64 : 1;
            shaped[i] *= envelope * fadeOut;
        }

        // Early reflections: the thing that separates an open back from a closed
        // one at the same decay time. Applied as sparse taps so the comb stays
        // short and sounds like a box, not like a delay pedal.
        for (const tap of spec.reflections)
        {
            const delay = Math.round (tap.ms * sampleRate / 1000);

            for (let i = length - 1; i >= delay; --i)
                shaped[i] += shaped[i - delay] * tap.gain * (c === 0 ? 1 : tap.otherSideGain);
        }

        // The direct arrival. Convolution trimming looks for this, and without it
        // the cabinet has no attack, only a smear.
        const attack = Math.round (spec.attackMs * sampleRate / 1000);
        shaped[attack] += 1.0;
        if (attack + 1 < length)
            shaped[attack + 1] += 0.35;

        // The high-gain box keeps more of the pick transient, modelled as a
        // one-sample overshoot rather than a longer filter.
        if (spec.punch != null)
            shaped[attack] += spec.punch;

        channels.push (normalise (shaped));
    }

    return { channels, length };
}

function normalise (samples)
{
    let peak = 0;
    for (let i = 0; i < samples.length; ++i)
        peak = Math.max (peak, Math.abs (samples[i]));

    if (peak < 1.0e-9)
        return samples;

    const gain = 0.95 / peak;
    for (let i = 0; i < samples.length; ++i)
        samples[i] *= gain;

    return samples;
}

/** Guards the one failure mode that produces a file which still loads: silence.
    A filtered NaN runs would be coerced to zero by writeInt16LE, so a valid
    header and an empty response are indistinguishable without checking here.
    This caught exactly that on the first run of this script. */
function sanityCheck (label, { channels, length })
{
    let peak = 0, energy = 0, tailEnergy = 0, bad = 0;
    const tailStart = Math.floor (length * 0.9);

    for (const channel of channels)
        for (let i = 0; i < length; ++i)
        {
            const v = channel[i];

            if (! Number.isFinite (v))
            {
                ++bad;
                continue;
            }

            const square = v * v;
            energy += square;
            if (i >= tailStart)
                tailEnergy += square;
            peak = Math.max (peak, Math.abs (v));
        }

    const rms = Math.sqrt (energy / (length * channels.length));

    if (bad > 0)
        throw new Error (`${label}: ${bad} non-finite samples - a filter stage is unstable.`);

    if (peak < 0.5)
        throw new Error (`${label}: peak ${peak.toFixed (4)} - the response is effectively silent.`);

    if (rms < 0.005)
        throw new Error (`${label}: rms ${rms.toFixed (5)} - no usable energy.`);

    if (tailEnergy > energy * 0.25)
        throw new Error (`${label}: the last 10% carries ${(tailEnergy / energy * 100).toFixed (0)}% of the energy - it is not decaying.`);

    return { peak, rms };
}

/* ------------------------------------------------------------ WAV writing */

function writeWav (path, { channels, length })
{
    const numChannels = channels.length;
    const bytesPerSample = 2;
    const blockAlign = numChannels * bytesPerSample;
    const dataSize = length * blockAlign;

    const buffer = Buffer.alloc (44 + dataSize);
    const putString = (offset, text) => buffer.write (text, offset, 'ascii');
    const put32 = (offset, value) => buffer.writeUInt32LE (value >>> 0, offset);
    const put16 = (offset, value) => buffer.writeUInt16LE (value & 0xffff, offset);

    putString (0, 'RIFF');
    put32 (4, 36 + dataSize);
    putString (8, 'WAVE');

    putString (12, 'fmt ');
    put32 (16, 16);
    put16 (20, 1);                 // PCM
    put16 (22, numChannels);
    put32 (24, sampleRate);
    put32 (28, sampleRate * blockAlign);
    put16 (32, blockAlign);
    put16 (34, 16);                // bits per sample

    putString (36, 'data');
    put32 (40, dataSize);

    let offset = 44;

    for (let i = 0; i < length; ++i)
        for (let c = 0; c < numChannels; ++c)
        {
            // Round-to-nearest, then clamp: a value outside the range would wrap
            // and put a full-scale click into the response.
            const scaled = Math.round (channels[c][i] * 32767);
            const clipped = Math.max (-32768, Math.min (32767, scaled));
            buffer.writeInt16LE (clipped, offset);
            offset += 2;
        }

    writeFileSync (path, buffer);
    return buffer.length;
}

/** Radix-2 in-place FFT, forward only, which is all the profile below needs.
    Written here rather than measured with a Goertzel at one frequency per band:
    the magnitude of a single noise realisation at a single frequency varies by
    tens of dB on its own, so a point probe cannot tell a filter effect from the
    realisation. Averaging the power over an octave band does. */
function fft (real, imag)
{
    const n = real.length;

    for (let i = 1, j = 0; i < n; ++i)
    {
        let bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j ^= bit;

        if (i < j)
        {
            let t = real[i]; real[i] = real[j]; real[j] = t;
            t = imag[i]; imag[i] = imag[j]; imag[j] = t;
        }
    }

    for (let len = 2; len <= n; len <<= 1)
    {
        const angle = -2 * Math.PI / len;
        const wRe = Math.cos (angle);
        const wIm = Math.sin (angle);

        for (let start = 0; start < n; start += len)
        {
            let curRe = 1, curIm = 0;

            for (let k = 0; k < len / 2; ++k)
            {
                const even = start + k;
                const odd = even + len / 2;
                const tRe = curRe * real[odd] - curIm * imag[odd];
                const tIm = curRe * imag[odd] + curIm * real[odd];

                real[odd] = real[even] - tRe;
                imag[odd] = imag[even] - tIm;
                real[even] += tRe;
                imag[even] += tIm;

                const nextRe = curRe * wRe - curIm * wIm;
                curIm = curRe * wIm + curIm * wRe;
                curRe = nextRe;
            }
        }
    }
}

const profileBands = [
    [80, 160], [160, 400], [400, 900], [900, 1800], [1800, 3600], [3600, 7000], [7000, 14000],
];

/** Mean power per octave band, returned in dB relative to the loudest band, so
    the table reads as a tone description: where the box lives and where it dies.
    Also returns two absolute numbers, because a shape normalised to its own
    loudest band cannot tell a dark box from a bright one of the same tilt. */
function bandProfile ({ channels, length })
{
    let size = 1024;
    while (size < length)
        size <<= 1;

    const real = new Float64Array (size);
    const imag = new Float64Array (size);

    // Summed to mono: the tone of the box is the same on both sides, and only the
    // image differs, so measuring one channel would put random realisation noise
    // in the table.
    for (let i = 0; i < length; ++i)
    {
        let sum = 0;
        for (const channel of channels)
            sum += channel[i];
        real[i] = sum / channels.length;
    }

    fft (real, imag);

    const powers = profileBands.map (([lo, hi]) =>
    {
        let total = 0, bins = 0;

        for (let k = 1; k < size / 2; ++k)
        {
            const hz = k * sampleRate / size;
            if (hz < lo || hz >= hi)
                continue;
            total += real[k] * real[k] + imag[k] * imag[k];
            ++bins;
        }

        return bins > 0 ? total / bins : 1.0e-18;
    });

    let weighted = 0, energy = 0;
    let topHz = 0;

    for (let k = 1; k < size / 2; ++k)
    {
        const hz = k * sampleRate / size;
        const power = real[k] * real[k] + imag[k] * imag[k];
        weighted += hz * power;
        energy += power;

        if (hz > topHz)
            topHz = hz;
    }

    const loudest = Math.max (...powers);
    const decibels = powers.map (p => 10 * Math.log10 ((p + 1.0e-18) / loudest));

    /*  The measured decay: window the squared signal into 5 ms blocks, find the
        loudest block, then the first later block that has fallen 20 dB. Synthetic
        presets specify a time constant, but what the ear follows is this number,
        so it is worth reporting rather than asserting. */
    const windowSamples = Math.round (sampleRate * 0.005);
    const windows = [];

    for (let start = 0; start + windowSamples <= length; start += windowSamples)
    {
        let sum = 0;
        for (let i = 0; i < windowSamples; ++i)
            sum += channels[0][start + i] * channels[0][start + i];
        windows.push (sum / windowSamples);
    }

    let decayMs = 0;

    if (windows.length > 2)
    {
        let peakIndex = 0;

        for (let i = 1; i < windows.length; ++i)
            if (windows[i] > windows[peakIndex])
                peakIndex = i;

        const threshold = windows[peakIndex] * 0.01;   // -20 dB

        for (let i = peakIndex + 1; i < windows.length; ++i)
        {
            if (windows[i] <= threshold)
            {
                decayMs = (i - peakIndex) * 5;
                break;
            }
        }

        if (decayMs === 0)
            decayMs = (windows.length - peakIndex) * 5;
    }

    return { shape: decibels, centroid: energy > 0 ? weighted / energy : 0, decayMs };
}

/* ------------------------------------------------------------ the three IRs */

const presets = [
    {
        /* 1x12 V30, closed back: dark and boxy, a strong 1150 Hz Voice-of-the-30
           peak, fast decay, no open-back reflections. The classic "record this
           through the wall" sound. */
        file: 'ir_1x12_v30_closed.wav',
        label: '1x12 V30 closed back',
        seed: 20261023,
        seconds: 0.13,
        decaySeconds: 0.033,
        attackMs: 0.08,
        stages: [
            { kind: 'highpass', hz: 88,  value: 0.71 },
            { kind: 'lowpass',  hz: 4300, value: 0.71 },
            { kind: 'lowpass',  hz: 4300, value: 0.71 },   // second pass = 4th order roll-off
            { kind: 'peak',     hz: 215,  value: 4.5 },    // closed-back body
            { kind: 'peak',     hz: 1150, value: 6.0 },    // V31/V30 voice peak
        ],
        reflections: [
            { ms: 0.35, gain: 0.16, otherSideGain: 0.13 },
            { ms: 1.10, gain: 0.09, otherSideGain: 0.11 },
        ],
    },
    {
        /* 2x12 open back: shorter and dryer, scooped upper-mids, a wider 900 Hz
           hump and noticeably more top-end leak. The reflection taps are stronger
           and the decay is shortest, which is what an open back does. */
        file: 'ir_2x12_open_back.wav',
        label: '2x12 open back',
        seed: 771113,
        seconds: 0.10,
        decaySeconds: 0.022,
        attackMs: 0.05,
        punch: 0.15,
        stages: [
            { kind: 'highpass', hz: 100,  value: 0.71 },
            { kind: 'lowpass',  hz: 5600, value: 0.71 },
            { kind: 'peak',     hz: 320,  value: -4.0 },   // the scoop
            { kind: 'peak',     hz: 900,  value: 4.0 },
            { kind: 'peak',     hz: 3400, value: 3.0 },    // open-back top end
        ],
        reflections: [
            { ms: 0.28, gain: 0.26, otherSideGain: 0.20 },
            { ms: 0.74, gain: 0.18, otherSideGain: 0.24 },
            { ms: 1.60, gain: 0.11, otherSideGain: 0.08 },
        ],
    },
    {
        /* 4x12 high gain: the longest and loudest box. More low-end thump, a
           tight mid punch and a fizz peak near 4 kHz, which is what makes it
           take distortion the way the other two do not. */
        file: 'ir_4x12_high_gain.wav',
        label: '4x12 high gain',
        seed: 4120412,
        seconds: 0.19,
        decaySeconds: 0.048,
        attackMs: 0.03,
        punch: 0.4,
        stages: [
            { kind: 'highpass', hz: 72,  value: 0.71 },
            { kind: 'lowpass',  hz: 6400, value: 0.71 },
            { kind: 'peak',     hz: 140,  value: 5.5 },    // four twelves of thump
            { kind: 'peak',     hz: 430,  value: 4.0 },    // the body the fizz used to eat
            { kind: 'peak',     hz: 900,  value: 2.5 },
            { kind: 'peak',     hz: 2600, value: 4.0 },
            { kind: 'peak',     hz: 4200, value: 3.5 },    // high-gain fizz, kept under the mids
        ],
        reflections: [
            { ms: 0.42, gain: 0.20, otherSideGain: 0.17 },
            { ms: 1.25, gain: 0.13, otherSideGain: 0.15 },
            { ms: 2.80, gain: 0.07, otherSideGain: 0.09 },
        ],
    },
];

mkdirSync (outDir, { recursive: true });

let totalBytes = 0;
const profiles = [];

console.log (`band Hz            ${profileBands.map (([lo, hi]) => `${lo}-${hi}`).join ('  ')}`);

for (const preset of presets)
{
    const ir = generate (preset);
    sanityCheck (preset.file, ir);
    const bytes = writeWav (join (outDir, preset.file), ir);
    totalBytes += bytes;

    const profile = bandProfile (ir);
    profiles.push ({ label: preset.label, ...profile });

    const milliseconds = (ir.length / sampleRate * 1000).toFixed (1);
    console.log (`${preset.label.padEnd (18)}${profile.shape.map (db => `${db.toFixed (1).padStart (7)}`).join ('  ')}` +
                 `   centroid ${profile.centroid.toFixed (0).padStart (5)} Hz  ` +
                 `-20 dB in ${String (profile.decayMs).padStart (4)} ms  (${milliseconds} ms long)`);
}

/*  The brief's demand is three cabinets that sound different, not one response
    copied three times, so the profiles are checked rather than eyeballed. Two
    boxes count as different when they are at least 4 dB apart in some octave band,
    or their spectral centroids are more than 10% apart, or their measured decays
    differ by over a quarter - the third is the open-back versus closed-back cue
    the shape numbers cannot express. */
for (let i = 0; i < profiles.length; ++i)
    for (let j = i + 1; j < profiles.length; ++j)
    {
        const a = profiles[i];
        const b = profiles[j];

        const spread = Math.max (...a.shape.map ((db, band) => Math.abs (db - b.shape[band])));
        const centroidGap = Math.abs (a.centroid - b.centroid) / Math.max (a.centroid, b.centroid) * 100;
        const decayGap = Math.abs (a.decayMs - b.decayMs) / Math.max (a.decayMs, b.decayMs) * 100;

        const reasons = [];
        if (spread >= 4.0)
            reasons.push (`${spread.toFixed (1)} dB band difference`);
        if (centroidGap >= 10.0)
            reasons.push (`${centroidGap.toFixed (0)}% centroid gap`);
        if (decayGap >= 25.0)
            reasons.push (`${decayGap.toFixed (0)}% decay gap`);

        if (reasons.length === 0)
            throw new Error (`"${a.label}" and "${b.label}" are too alike: ` +
                             `${spread.toFixed (1)} dB shape spread, ${centroidGap.toFixed (1)}% centroid, ` +
                             `${decayGap.toFixed (1)}% decay. Retune the presets.`);

        console.log (`${a.label} vs ${b.label}: ${reasons.join (', ')}`);
    }

console.log (`\nWrote ${presets.length} impulse responses to ${outDir} ` +
             `(${totalBytes} bytes total).`);
console.log ('These are modelled responses, not microphone captures. Load a measured IR ' +
             'from the Cabinet card for real cabinet tones.');
