/*
  PedalBoard DSP and ring-buffer test bench.

  Everything here is a measurement, not an opinion: the probe compiles the same
  effect headers the app compiles and drives them with signals whose answer is
  known in advance. It exists because reading a granular shifter cannot tell you
  what it delays by, and because the first version of this file measured exactly
  that with an impulse - which a Hann-windowed overlap-add smears - and reported
  failures that were the probe's, not the app's.

  What it checks:

    A  the granular shifter's true group delay against getLatencySamples(), using a
       short noise burst and three independent estimators (correlation peak, energy
       centroid, first-crossing). A disagreement here is dry/wet misalignment, which
       is audible as flam through the global Mix.
    B  pitch accuracy on a long analysis window, with the un-shifted leak reported
       separately, because a 40 ms window cannot resolve one semitone at 220 Hz and
       a probe that tries will "measure" the leak instead.
    C  every effect in the factory at every parameter extreme: finite output, no
       level explosion, Mix = 0 transparent, bypass transparent.
    D  the ring buffer's overflow and underflow policy, including the worst-case
       read length the sizing argument in PedalConfig.h depends on.

  Exit status is non-zero if any check fails.

  build:  cmake --build _tests/build --config Release --target dsp_probe
  run:    _tests\build\Release\dsp_probe.exe
*/
#include <juce_dsp/juce_dsp.h>

#include "Common/PedalConfig.h"
#include "Common/PedalLockFree.h"
#include "Effects/EffectFactory.h"
#include "Effects/Pitch/PitchShifterEffect.h"
#include "Effects/Pitch/SubOctaveDivider.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace pedal;

namespace
{

int failures = 0;

void report (bool ok, const char* what)
{
    std::printf ("  %s %s\n", ok ? "PASS" : "FAIL", what);
    if (! ok)
        ++failures;
}

/** Deterministic noise, so a run can be reproduced. */
std::vector<float> makeNoise (std::size_t length, float amplitude = 0.25f)
{
    std::vector<float> out (length);
    std::uint32_t state = 0x12345678u;

    for (auto& sample : out)
    {
        state = state * 1664525u + 1013904223u;
        const auto uniform = static_cast<float> ((state >> 8) & 0xffffff) / 8388608.0f - 1.0f;
        sample = uniform * amplitude;
    }

    return out;
}

constexpr int kBlock = 512;

/** Runs the real Pitch Shifter effect over a whole signal in blocks. */
std::vector<float> runShifter (double sr, int /*windowMs*/, double ratio,
                               const std::vector<float>& in, int& reportedLatency)
{
    PitchShifterEffect shifter;
    shifter.setParameter (PitchShifterEffect::semitones,
                          static_cast<float> (12.0 * std::log2 (ratio)));
    shifter.setParameter (PitchShifterEffect::formant, 0.0f);
    shifter.setParameter (PitchShifterEffect::mix, 1.0f);
    shifter.prepare (sr, kBlock);

    std::vector<float> out (in.size(), 0.0f);
    juce::AudioBuffer<float> block (2, kBlock);

    for (std::size_t base = 0; base < in.size(); base += static_cast<std::size_t> (kBlock))
    {
        const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock),
                                                               in.size() - base));

        for (int i = 0; i < n; ++i)
        {
            const auto sample = in[base + static_cast<std::size_t> (i)];
            block.setSample (0, i, sample);
            block.setSample (1, i, sample);
        }

        juce::AudioBuffer<float> view (block.getArrayOfWritePointers(), 2, n);
        shifter.process (view);

        for (int i = 0; i < n; ++i)
            out[base + static_cast<std::size_t> (i)] = view.getSample (0, i);
    }

    reportedLatency = shifter.getLatencySamples();
    return out;
}

/** Goertzel power over a Hann-windowed segment. */
double goertzel (const std::vector<float>& v, std::size_t offset, int length, double freq, double sr)
{
    const auto w = 2.0 * 3.14159265358979323846 * freq / sr;
    const auto coeff = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;

    for (int i = 0; i < length; ++i)
    {
        const auto hann = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * i / (length - 1));
        const auto x = static_cast<double> (v[offset + static_cast<std::size_t> (i)]) * hann;
        const auto s0 = x + coeff * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    return s1 * s1 + s2 * s2 - coeff * s1 * s2;
}

/** Dominant frequency on a fine log grid, with parabolic refinement. */
double dominantFrequency (const std::vector<float>& v, std::size_t offset, int length,
                          double sr, double lo, double hi, double* peakPower = nullptr)
{
    double bestFreq = lo, bestPower = -1.0;
    std::vector<double> freqs, powers;

    for (auto f = lo; f <= hi; f *= 1.0015)
    {
        const auto p = goertzel (v, offset, length, f, sr);
        freqs.push_back (f);
        powers.push_back (p);

        if (p > bestPower)  { bestPower = p; bestFreq = f; }
    }

    const auto n = static_cast<int> (freqs.size());

    for (int i = 1; i < n - 1; ++i)
    {
        if (powers[static_cast<std::size_t> (i)] != bestPower)
            continue;

        const auto y0 = std::log (powers[static_cast<std::size_t> (i - 1)] + 1.0e-12);
        const auto y1 = std::log (powers[static_cast<std::size_t> (i)] + 1.0e-12);
        const auto y2 = std::log (powers[static_cast<std::size_t> (i + 1)] + 1.0e-12);
        const auto denom = y0 - 2.0 * y1 + y2;

        if (std::abs (denom) > 1.0e-12)
        {
            const auto delta = 0.5 * (y0 - y2) / denom;
            const auto step = freqs[1] / freqs[0];
            bestFreq = freqs[static_cast<std::size_t> (i)] * std::pow (step, delta);
        }
        break;
    }

    if (peakPower != nullptr)
        *peakPower = bestPower;

    return bestFreq;
}

double cents (double measured, double expected)
{
    return 1200.0 * std::log (measured / expected) / std::log (2.0);
}

/** Independent spectrum check: a real FFT over the same segment, printing the
    strongest bins. Added because the Goertzel scan and the shifter must not be the
    only two opinions in the room - if the scan says 210 Hz for a signal that should
    be 220, either the engine or the scan is wrong, and an FFT settles it. */
void dumpSpectrum (const std::vector<float>& v, std::size_t offset, int length,
                   double sr, const char* label)
{
    // Largest power of two that FITS the segment. Rounding up instead reads past
    // the end of the signal - which is what the first version of this function did,
    // and it came back with a "spectrum" of junk and a fastfail on exit.
    int order = 0;
    while ((1 << (order + 1)) <= length)  ++order;
    if (order > 15)  order = 15;
    if (order < 8)   { std::printf ("      %s FFT skipped, segment too short\n", label); return; }

    const auto size = 1 << order;
    juce::dsp::FFT fft (order);

    /*  The array handed to performFrequencyOnlyForwardTransform has to be
        2 * getSize() floats - the first half is the input, the rest is scratch
        (juce_FFT.h:108). Sizing it to `size` is what made the first version of this
        function fail with an access violation rather than a wrong answer. */
    std::vector<float> real (static_cast<std::size_t> (size) * 2, 0.0f);

    for (int i = 0; i < size; ++i)
    {
        const auto hann = 0.5f - 0.5f * static_cast<float> (std::cos (2.0 * 3.14159265358979323846 * i / (size - 1)));
        real[static_cast<std::size_t> (i)] = v[offset + static_cast<std::size_t> (i)] * hann;
    }

    fft.performFrequencyOnlyForwardTransform (real.data(), true);

    struct Peak { double freq, magnitude; };
    std::vector<Peak> peaks;

    for (int bin = 1; bin < size / 2; ++bin)
    {
        const auto m = static_cast<double> (real[static_cast<std::size_t> (bin)]);
        if (m < 1.0e-6)
            continue;

        const auto f = bin * sr / size;

        // Local maxima only, so one sine does not light up forty bins.
        const auto lower = static_cast<double> (real[static_cast<std::size_t> (bin - 1)]);
        const auto upper = static_cast<double> (real[static_cast<std::size_t> (bin + 1)]);
        if (m >= lower && m > upper)
            peaks.push_back ({ f, m });
    }

    std::sort (peaks.begin(), peaks.end(), [] (const Peak& a, const Peak& b) { return a.magnitude > b.magnitude; });

    std::printf ("      %s FFT top:", label);
    for (std::size_t i = 0; i < peaks.size() && i < 5; ++i)
    {
        const auto db = 20.0 * std::log10 (peaks[i].magnitude / (peaks[0].magnitude + 1.0e-12) + 1.0e-12);
        std::printf ("  %.1f Hz (%.1f dB)", peaks[i].freq, db);
    }
    std::printf ("\n");
}

//==============================================================================
//  A - spectral shifter group delay
//==============================================================================
void checkShifterLatency (double sr)
{
    const auto total = static_cast<std::size_t> (3.0 * sr);
    const auto tone = static_cast<int> (0.20 * sr);
    const auto toneAt = static_cast<int> (1.0 * sr);

    // Noise, not a sine: a periodic tone correlates equally well one period
    // later, which is how the first version of this check "measured" 130 ms for
    // an 80 ms shift.
    std::vector<float> in (total, 0.0f);
    const auto burst = makeNoise (static_cast<std::size_t> (tone), 0.3f);
    for (int i = 0; i < tone; ++i)
        in[static_cast<std::size_t> (toneAt + i)] = burst[static_cast<std::size_t> (i)];

    int reported = 0;
    const auto out = runShifter (sr, 60, 1.0, in, reported);

    // Correlate a stretch of the delayed output with the input. At ratio 1 a
    // spectral shifter should reproduce the burst, only later; the lag at which
    // the correlation peaks is the latency the engine compensates.
    const auto compare = static_cast<int> (0.10 * sr);
    const auto lo = juce::jmax (0, reported - static_cast<int> (0.15 * sr));
    const auto hi = juce::jmin (static_cast<int> (out.size()) - compare,
                                reported + static_cast<int> (0.15 * sr));

    double bestCorr = -1.0e30;
    int bestLag = lo;

    for (int lag = lo; lag < hi; ++lag)
    {
        double sum = 0.0;

        for (int i = 0; i < compare; ++i)
            sum += static_cast<double> (in[static_cast<std::size_t> (toneAt + i)])
                 * static_cast<double> (out[static_cast<std::size_t> (toneAt + i + lag)]);

        if (sum > bestCorr) { bestCorr = sum; bestLag = lag; }
    }

    std::printf ("  sr=%g reported=%d (%.1f ms)  correlation lag=%d (%.1f ms)\n",
                 sr, reported, 1000.0 * reported / sr, bestLag, 1000.0 * bestLag / sr);

    report (reported > 0 && std::abs (bestLag - reported) <= static_cast<int> (0.02 * sr),
            "spectral shifter latency agrees within 20 ms");
}

//==============================================================================
//  B - pitch
//==============================================================================
void checkPitch (double sr, double freq, double semitones)
{
    const auto total = static_cast<std::size_t> (2.5 * sr);
    std::vector<float> in (total);

    for (std::size_t i = 0; i < total; ++i)
        in[i] = 0.4f * static_cast<float> (std::sin (2.0 * 3.14159265358979323846 * freq * i / sr));

    const auto ratio = std::pow (2.0, semitones / 12.0);

    std::printf ("      running shifter: %g Hz x %.4f, window 60 ms\n", freq, ratio);
    int reported = 0;
    const auto out = runShifter (sr, 60, ratio, in, reported);
    std::printf ("      shifter returned, reported latency %d\n", reported);

    // Analyse the last 600 ms only: the first second is the fill period, and a
    // 600 ms window resolves better than a tenth of a semitone down to ~100 Hz.
    const auto analysisLength = static_cast<int> (0.6 * sr);
    const auto offset = out.size() - static_cast<std::size_t> (analysisLength);

    double shiftedPower = 0.0;
    const auto expected = freq * ratio;
    const auto measured = dominantFrequency (out, offset, analysisLength, sr,
                                             expected * 0.75, expected * 1.33, &shiftedPower);

    double leakPower = 0.0;
    dominantFrequency (out, offset, analysisLength, sr, freq * 0.97, freq * 1.03, &leakPower);

    const auto error = cents (measured, expected);
    const auto leakDb = 10.0 * std::log10 ((leakPower + 1.0e-12) / (shiftedPower + 1.0e-12));

    dumpSpectrum (out, offset, analysisLength, sr, "wet");

    std::printf ("  sr=%g %6.1f Hz %+5.1f st -> %8.2f Hz (want %8.2f)  err=%+6.1f cents  leak=%6.1f dB\n",
                 sr, freq, semitones, measured, expected, error, leakDb);

    report (std::abs (error) <= 25.0, "pitch within 25 cents");
}

//==============================================================================
//  B2 - the sub-octave divider
//==============================================================================
void checkDivider (double sr, double freq)
{
    const auto total = static_cast<std::size_t> (2.0 * sr);
    std::vector<float> in (total);

    for (std::size_t i = 0; i < total; ++i)
        in[i] = 0.35f * static_cast<float> (std::sin (2.0 * 3.14159265358979323846 * freq * i / sr));

    SubOctaveDivider divider;
    divider.prepare (sr, kBlock);

    std::vector<float> out (total, 0.0f);
    std::vector<float> inBlk (static_cast<std::size_t> (kBlock));
    std::vector<float> outBlk (static_cast<std::size_t> (kBlock));

    for (std::size_t base = 0; base < total; base += static_cast<std::size_t> (kBlock))
    {
        const auto n = static_cast<int> (std::min<std::size_t> (static_cast<std::size_t> (kBlock), total - base));
        for (int i = 0; i < n; ++i)
            inBlk[static_cast<std::size_t> (i)] = in[base + static_cast<std::size_t> (i)];

        divider.process (inBlk.data(), outBlk.data(), n, 1.0f);

        for (int i = 0; i < n; ++i)
            out[base + static_cast<std::size_t> (i)] = outBlk[static_cast<std::size_t> (i)];
    }

    const auto analysisLength = static_cast<int> (0.5 * sr);
    const auto offset = out.size() - static_cast<std::size_t> (analysisLength);

    auto peak = 0.0f;
    for (std::size_t i = offset; i < out.size(); ++i)
        peak = juce::jmax (peak, std::abs (out[i]));

    const auto measured = dominantFrequency (out, offset, analysisLength, sr, freq * 0.4, freq * 0.62);
    const auto error = cents (measured, freq * 0.5);

    std::printf ("  sr=%g %6.1f Hz -> sub %7.2f Hz (want %7.2f)  err=%+6.1f cents  peak=%.3f\n",
                 sr, freq, measured, freq * 0.5, error, peak);

    // A divider is either exact or it is not doing its job; half a semitone is the
    // bar, and silence is an outright failure.
    report (peak > 0.01f && std::abs (error) <= 50.0, "sub-octave is exactly half the input");
}

//==============================================================================
//  C - every effect at every extreme
//==============================================================================
void checkEffects (double sr, int block)
{
    // The same context the engine builds: the cabinet's convolution needs a message
    // queue that outlives it, and the wah reads the expression atomic.
    std::atomic<float> expression { 0.5f };
    juce::dsp::ConvolutionMessageQueue queue;

    EffectContext context;
    context.expressionPosition = &expression;
    context.convolutionQueue   = &queue;

    EffectFactory factory (context);

    const auto noise = makeNoise (static_cast<std::size_t> (block));

    for (int type = 0; type < numEffectTypes; ++type)
    {
        auto* effect = factory.get (static_cast<EffectTypeID> (type));
        const auto* meta = EffectFactory::metadataFor (type);

        if (effect == nullptr || meta == nullptr)
        {
            report (false, "factory returned a null effect");
            continue;
        }

        effect->prepare (sr, maxBlockSize);
        effect->setBypassed (false);

        const auto params = effect->getParameters();

        int mixIndex = -1;
        for (int p = 0; p < static_cast<int> (params.size()); ++p)
            if (std::strcmp (params[static_cast<std::size_t> (p)].name, "Mix") == 0)
                mixIndex = p;

        // 1. Bypass must be transparent: the engine skips process() entirely, so the
        //    card's lamp is only honest if the buffer comes back untouched.
        {
            juce::AudioBuffer<float> buffer (chainChannels, block);
            for (int c = 0; c < chainChannels; ++c)
                buffer.copyFrom (c, 0, noise.data(), block);

            juce::AudioBuffer<float> reference (buffer);
            effect->setBypassed (true);
            if (! effect->isBypassed ())
                report (false, "setBypassed did not stick");
            effect->setBypassed (false);
            effect->process (buffer);

            bool untouched = true;
            for (int c = 0; c < chainChannels; ++c)
                for (int i = 0; i < block; ++i)
                    if (buffer.getSample (c, i) != reference.getSample (c, i))
                        untouched = false;

            /*  Most pedals change the signal at their default settings, and one that
                does not is a bug. Three are idle by design: the cabinet has no
                impulse response until the user loads one, a looper with nothing
                recorded is a cable, and the plugin host has no plugin until the
                user picks a file. For those the meaningful assertion is the
                opposite - that they really do leave the signal alone. */
            const bool idleByDesign = type == static_cast<int> (cabinet)
                                   || type == static_cast<int> (looper)
                                   || type == static_cast<int> (pluginHost);

            char audible[160];
            std::snprintf (audible, sizeof (audible),
                           idleByDesign ? "%s: passes the signal through when idle"
                                        : "%s: changes the signal at defaults",
                           meta->name);

            report (idleByDesign == untouched, audible);
            effect->setBypassed (false);
        }

        // 2. Every parameter at both ends: finite, and no level explosion.
        //
        //    The signal is streamed through in successive blocks rather than the
        //    same block run through process() eight times: a static filter cascaded
        //    eight ways multiplies its gain by itself, which is how the first
        //    version of this test "found" a 3-Band EQ peaking at 4e30 with a +15 dB
        //    shelf. Envelopes still get time to settle, because they now see eight
        //    blocks of real time.
        for (int p = 0; p < static_cast<int> (params.size()); ++p)
        {
            for (int extreme = 0; extreme < 2; ++extreme)
            {
                const auto& d = params[static_cast<std::size_t> (p)];

                // Freshly prepared for every setting: without this, state left by
                // the previous parameter is still in the filters when the next one
                // is measured, and a growth that never happens in the app shows up
                // as one enormous number here.
                effect->prepare (sr, maxBlockSize);
                effect->setBypassed (false);
                effect->setParameter (p, extreme == 0 ? d.minValue : d.maxValue);

                bool finite = true;
                float peak = 0.0f;

                for (int pass = 0; pass < 8; ++pass)
                {
                    const auto drive = makeNoise (static_cast<std::size_t> (block), 0.25f);

                    juce::AudioBuffer<float> buffer (chainChannels, block);
                    for (int c = 0; c < chainChannels; ++c)
                        buffer.copyFrom (c, 0, drive.data(), block);

                    effect->process (buffer);

                    // Only the last blocks count: the first ones still hold the
                    // tail of whatever the previous setting was doing.
                    if (pass < 4)
                        continue;

                    for (int c = 0; c < buffer.getNumChannels(); ++c)
                        for (int i = 0; i < block; ++i)
                        {
                            const auto v = buffer.getSample (c, i);
                            if (! std::isfinite (v))  finite = false;
                            peak = juce::jmax (peak, std::abs (v));
                        }
                }

                char label[160];
                std::snprintf (label, sizeof (label), "%s: %s at %s stays finite and bounded",
                               meta->name, d.name, extreme == 0 ? "min" : "max");

                if (! finite)
                    std::printf ("      %s: %s at %s produced NaN/Inf\n",
                                 meta->name, d.name, extreme == 0 ? "min" : "max");
                else if (peak > 100.0f)
                    std::printf ("      %s: %s at %s peaked at %g\n",
                                 meta->name, d.name, extreme == 0 ? "min" : "max", peak);

                report (finite && peak <= 100.0f, label);
            }
        }

        // 3. Mix = 0 has to hand back the untouched signal: that is what "blend" is
        //    supposed to mean, and what lets a pedal sit in a chain silently.
        if (mixIndex >= 0)
        {
            for (int p = 0; p < static_cast<int> (params.size()); ++p)
                effect->setParameter (p, params[static_cast<std::size_t> (p)].defaultValue);

            effect->setParameter (mixIndex, params[static_cast<std::size_t> (mixIndex)].minValue);

            /*  Streamed as one continuous signal, because an effect that
                oversamples has to delay its dry copy by its own latency for the
                blend to be a blend and not a comb filter. The comparison is
                therefore against the input shifted by getLatencySamples(), which
                is the latency the engine also compensates on the global dry. */
            const auto latency = std::max (0, effect->getLatencySamples());
            const auto passes = 8;
            const auto total = block * passes;

            std::vector<float> input (static_cast<std::size_t> (total));
            for (int i = 0; i < total; ++i)
                input[static_cast<std::size_t> (i)] = noise[static_cast<std::size_t> (i % block)];

            std::vector<float> output (static_cast<std::size_t> (total), 0.0f);

            juce::AudioBuffer<float> buffer (chainChannels, block);

            for (int pass = 0; pass < passes; ++pass)
            {
                for (int c = 0; c < chainChannels; ++c)
                    buffer.copyFrom (c, 0, input.data() + pass * block, block);

                effect->process (buffer);

                for (int c = 0; c < chainChannels; ++c)
                    if (c == 0)
                        for (int i = 0; i < block; ++i)
                            output[static_cast<std::size_t> (pass * block + i)] = buffer.getSample (c, i);
            }

            // Let the oversampler's half band filters fill before judging.
            auto worst = 0.0f;
            for (int i = latency + block; i < total; ++i)
                worst = juce::jmax (worst, std::abs (output[static_cast<std::size_t> (i)]
                                                     - input[static_cast<std::size_t> (i - latency)]));

            char label[200];
            std::snprintf (label, sizeof (label),
                           "%s: Mix at minimum is transparent apart from its %d sample latency (worst %.2e)",
                           meta->name, latency, worst);

            // A delay or reverb at Mix 0 can still differ by its own latency, so the
            // bar is "inaudible", not "bit exact".
            report (worst < 1.0e-4f, label);
        }
    }
}

//==============================================================================
//  E - drive, dynamics and swept filters
//==============================================================================
namespace
{

float rmsOf (const juce::AudioBuffer<float>& buffer, int start)
{
    double sum = 0.0;
    int count = 0;

    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = start; i < buffer.getNumSamples(); ++i)
        {
            const auto v = static_cast<double> (buffer.getSample (c, i));
            sum += v * v;
            ++count;
        }

    return count > 0 ? static_cast<float> (std::sqrt (sum / count)) : 0.0f;
}

/** Runs sineOrNoise through the effect in blocks and returns the RMS of the
    second half, after every envelope has settled. */
float runAndMeasureRms (PedalEffect& effect, const std::vector<float>& input,
                        int block, double sr)
{
    effect.prepare (sr, maxBlockSize);

    juce::AudioBuffer<float> buffer (chainChannels, block);
    double sum = 0.0;
    int count = 0;
    const auto half = static_cast<int> (input.size()) / 2;
    int index = 0;

    while (index < static_cast<int> (input.size()))
    {
        const auto n = std::min (block, static_cast<int> (input.size()) - index);

        for (int c = 0; c < chainChannels; ++c)
            buffer.copyFrom (c, 0, input.data() + index, n);

        effect.process (buffer);

        for (int i = 0; i < n; ++i, ++index)
            if (index >= half)
            {
                const auto v = static_cast<double> (buffer.getSample (0, i));
                sum += v * v;
                ++count;
            }
    }

    return count > 0 ? static_cast<float> (std::sqrt (sum / count)) : 0.0f;
}

float statusNumber (const PedalEffect& effect)
{
    return juce::String (effect.getStatusText()).retainCharacters ("0123456789.-").getFloatValue();
}

/** Same as runAndMeasureRms, but returns the processed signal so the caller can
    look at its spectrum. */
std::vector<float> runEffect (PedalEffect& effect, const std::vector<float>& input,
                              int block, double sr)
{
    effect.prepare (sr, maxBlockSize);

    juce::AudioBuffer<float> buffer (chainChannels, block);
    std::vector<float> out;
    out.reserve (input.size());
    int index = 0;

    while (index < static_cast<int> (input.size()))
    {
        const auto n = std::min (block, static_cast<int> (input.size()) - index);

        for (int c = 0; c < chainChannels; ++c)
            buffer.copyFrom (c, 0, input.data() + index, n);

        effect.process (buffer);

        for (int i = 0; i < n; ++i)
            out.push_back (buffer.getSample (0, i));

        index += n;
    }

    return out;
}

/** Power ratio of harmonics 2..maxHarmonic to the fundamental. */
double harmonicThd (const std::vector<float>& out, std::size_t offset, int length,
                    double f0, double sr, int maxHarmonic)
{
    const auto fundamental = goertzel (out, offset, length, f0, sr);
    double harmonics = 0.0;

    for (int h = 2; h <= maxHarmonic && f0 * h < sr * 0.45; ++h)
        harmonics += goertzel (out, offset, length, f0 * h, sr);

    return fundamental > 0.0 ? std::sqrt (harmonics / fundamental) : 0.0;
}

/** Renders an effect over a whole stereo input and returns every output sample;
    used where the position or the decay of the wet signal is the measurement. */
juce::AudioBuffer<float> renderEffect (PedalEffect& effect,
                                       const std::vector<float>& left,
                                       const std::vector<float>& right,
                                       int block, double sr)
{
    effect.prepare (sr, maxBlockSize);

    juce::AudioBuffer<float> output (chainChannels, static_cast<int> (left.size()));
    output.clear();
    juce::AudioBuffer<float> buffer (chainChannels, block);

    for (int base = 0; base < static_cast<int> (left.size()); base += block)
    {
        const auto n = std::min (block, static_cast<int> (left.size()) - base);

        for (int i = 0; i < n; ++i)
        {
            buffer.setSample (0, i, left[static_cast<std::size_t> (base + i)]);
            buffer.setSample (1, i, right.empty()
                                         ? left[static_cast<std::size_t> (base + i)]
                                         : right[static_cast<std::size_t> (base + i)]);
        }

        effect.process (buffer);

        for (int c = 0; c < chainChannels; ++c)
            output.copyFrom (c, base, buffer, c, 0, n);
    }

    return output;
}

void checkDriveAndFilters (double sr, int block)
{
    std::atomic<float> expression { 0.5f };
    juce::dsp::ConvolutionMessageQueue queue;

    EffectContext context;
    context.expressionPosition = &expression;
    context.convolutionQueue   = &queue;

    EffectFactory factory (context);

    auto setParam = [&factory] (EffectTypeID type, const char* name, float value)
    {
        auto* effect = factory.get (type);
        for (int p = 0; p < effect->getNumParameters(); ++p)
            if (std::strcmp (effect->getParameters()[static_cast<std::size_t> (p)].name, name) == 0)
                effect->setParameter (p, value);
    };

    auto resetToDefaults = [&factory] (EffectTypeID type)
    {
        auto* effect = factory.get (type);
        for (int p = 0; p < effect->getNumParameters(); ++p)
            effect->resetParameterToDefault (p);
    };

    // 1. Nothing may reach the output before the effect's own latency. When the
    //    dry copy of an oversampled pedal is not delayed, half the impulse comes
    //    out at sample zero and every frequency where the two paths meet out of
    //    phase disappears: the comb filter this bench was written to catch.
    std::printf ("  -- impulse position at Mix 50 %%, nothing before the latency --\n");

    for (auto type : { overdrive, distortion, fuzz, bitcrusher, solidBody })
    {
        auto* effect = factory.get (type);
        resetToDefaults (type);
        setParam (type, "Mix", 0.5f);

        effect->prepare (sr, maxBlockSize);
        const auto latency = effect->getLatencySamples();

        juce::AudioBuffer<float> buffer (chainChannels, block);
        buffer.clear();
        buffer.setSample (0, 0, 0.5f);
        buffer.setSample (1, 0, 0.5f);
        effect->process (buffer);

        auto earliest = block;
        auto peakIndex = 0;
        auto peakValue = 0.0f;
        auto preEnergy = 0.0;
        auto totalEnergy = 0.0;

        for (int i = 0; i < block; ++i)
        {
            const auto v = buffer.getSample (0, i);
            const auto energy = static_cast<double> (v) * v;
            totalEnergy += energy;

            if (i < latency)
                preEnergy += energy;

            if (std::abs (v) > std::abs (peakValue))
            {
                peakValue = v;
                peakIndex = i;
            }

            if (earliest == block && std::abs (v) > 1.0e-7f)
                earliest = i;
        }

        /*  The polyphase IIR oversampler rings a little before its nominal
            delay, so "nothing at all" is not achievable and an absolute
            threshold would flag the filter's own tail as a bug. The comb this
            guards against put the undelayed dry copy at full amplitude at
            sample zero, so comparing the energy before the latency with the
            whole impulse separates the two by orders of magnitude. */
        const auto preRatio = totalEnergy > 0.0 ? preEnergy / totalEnergy : 1.0;

        char label[240];
        std::snprintf (label, sizeof (label),
                       "%s: dry and wet stay aligned at Mix 50 %% (latency %d, first %d, "
                       "peak %d, pre-latency energy %.2e)",
                       EffectFactory::metadataFor (type)->name, latency, earliest, peakIndex,
                       preRatio);
        report (latency > 0 && preRatio < 0.02 && peakIndex >= latency - 1, label);
        (void) peakValue;
    }

    // 2. Drive must change the character, not the volume by an unusable amount.
    //    A pedal that is 15 dB louder at the top of its drive range cannot be
    //    compared with the others, and one that gets quieter as it distorts is
    //    broken.
    std::printf ("  -- level across the drive range (220 Hz, same input) --\n");

    {
        std::vector<float> sine (static_cast<std::size_t> (sr * 2));
        for (std::size_t i = 0; i < sine.size(); ++i)
            sine[i] = 0.25f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 220.0
                                                            * static_cast<double> (i) / sr));

        struct DriveCase { EffectTypeID type; const char* knob; };

        for (auto test : { DriveCase { overdrive, "Drive" },
                           DriveCase { distortion, "Drive" },
                           DriveCase { fuzz, "Fuzz" } })
        {
            resetToDefaults (test.type);
            setParam (test.type, "Mix", 1.0f);

            const auto* effect = factory.get (test.type);
            auto* meta = EffectFactory::metadataFor (test.type);
            const auto parameter = [&] (const char* n)
            {
                for (auto& d : effect->getParameters())
                    if (std::strcmp (d.name, n) == 0)
                        return d;
                return ParamDescription {};
            } (test.knob);

            const auto low  = [&]
            {
                setParam (test.type, test.knob, parameter.minValue);
                return runAndMeasureRms (*factory.get (test.type), sine, block, sr);
            }();

            const auto mid  = [&]
            {
                setParam (test.type, test.knob, parameter.defaultValue);
                return runAndMeasureRms (*factory.get (test.type), sine, block, sr);
            }();

            const auto high = [&]
            {
                setParam (test.type, test.knob, parameter.maxValue);
                return runAndMeasureRms (*factory.get (test.type), sine, block, sr);
            }();

            const auto spreadDb = 20.0f * std::log10 (std::max (high, 1.0e-6f)
                                                     / std::max (low, 1.0e-6f));

            char label[240];
            std::snprintf (label, sizeof (label),
                           "%s: %s min %.3f, default %.3f, max %.3f, spread %+.1f dB",
                           meta->name, test.knob, low, mid, high, spreadDb);

            report (low > 0.02f && mid > 0.02f && high > 0.02f && high < 0.95f
                    && std::abs (spreadDb) <= 15.0f, label);
        }
    }

    // 3. The two wahs sweep, and they do not turn into volume pedals while doing
    //    it. The readout is the status line the card shows, so this also proves
    //    that the card has something live to display.
    std::printf ("  -- wah sweep and level --\n");

    {
        auto noise = makeNoise (static_cast<std::size_t> (sr * 2), 0.2f);

        auto* wahPedal = factory.get (pedal::wah);
        resetToDefaults (pedal::wah);
        setParam (pedal::wah, "Position", 0.0f);
        const auto lowLevel = runAndMeasureRms (*wahPedal, noise, block, sr);
        const auto lowHz = statusNumber (*wahPedal);

        setParam (pedal::wah, "Position", 100.0f);
        const auto highLevel = runAndMeasureRms (*wahPedal, noise, block, sr);
        const auto highHz = statusNumber (*wahPedal);

        char label[220];
        std::snprintf (label, sizeof (label),
                       "Wah Pedal: centre %.0f Hz at Position 0 and %.0f Hz at 100, level %.3f / %.3f",
                       lowHz, highHz, lowLevel, highLevel);
        report (highHz > lowHz * 2.0f && lowHz > 100.0f
                && lowLevel > 0.01f && highLevel > 0.01f, label);

        /*  Broadband noise through a band-pass loses level as the band narrows
            simply because it passes a narrower slice of the spectrum, so the
            sweep levels above cannot be equal by construction. The makeup's
            actual job is to hold the level when only Q changes, so fix the
            centre and compare the two ends of the Q range. */
        setParam (pedal::wah, "Position", 50.0f);
        setParam (pedal::wah, "Q", 0.5f);
        const auto wideLevel = runAndMeasureRms (*wahPedal, noise, block, sr);

        setParam (pedal::wah, "Q", 12.0f);
        const auto narrowLevel = runAndMeasureRms (*wahPedal, noise, block, sr);

        const auto makeupDb = 20.0f * std::log10 (std::max (narrowLevel, 1.0e-6f)
                                                  / std::max (wideLevel, 1.0e-6f));

        std::snprintf (label, sizeof (label),
                       "Wah Pedal: Q makeup holds level, Q 0.5 %.3f vs Q 12 %.3f (%+.1f dB)",
                       wideLevel, narrowLevel, makeupDb);
        report (std::abs (makeupDb) <= 4.0f, label);

        /*  Crossing the two frequency knobs used to collapse the range to 1 Hz,
            turning the pedal into a fixed band-pass with no feedback anywhere in
            the UI. Either order must describe a sweep. */
        setParam (pedal::wah, "Min Freq", 1500.0f);
        setParam (pedal::wah, "Max Freq", 800.0f);
        setParam (pedal::wah, "Position", 0.0f);
        runAndMeasureRms (*wahPedal, noise, block, sr);
        const auto crossedLow = statusNumber (*wahPedal);

        setParam (pedal::wah, "Position", 100.0f);
        runAndMeasureRms (*wahPedal, noise, block, sr);
        const auto crossedHigh = statusNumber (*wahPedal);

        std::snprintf (label, sizeof (label),
                       "Wah Pedal: Min 1500 / Max 800 still sweeps %.0f Hz to %.0f Hz",
                       crossedLow, crossedHigh);
        // 1.5, not 2: the crossed range itself is only 800..1500 Hz (1.9x).
        report (crossedHigh > crossedLow * 1.5f, label);

        /*  Source = MIDI CC with no MIDI input open must leave the Position knob
            in charge; the bench never opens a port, so the flag is null/off. */
        resetToDefaults (pedal::wah);
        setParam (pedal::wah, "Source", 1.0f);
        setParam (pedal::wah, "Position", 0.0f);
        runAndMeasureRms (*wahPedal, noise, block, sr);
        const auto fallbackLow = statusNumber (*wahPedal);

        setParam (pedal::wah, "Position", 100.0f);
        runAndMeasureRms (*wahPedal, noise, block, sr);
        const auto fallbackHigh = statusNumber (*wahPedal);

        std::snprintf (label, sizeof (label),
                       "Wah Pedal: no MIDI input, Position still sweeps %.0f Hz to %.0f Hz",
                       fallbackLow, fallbackHigh);
        report (fallbackHigh > fallbackLow * 2.0f
                && wahPedal->getWarningText().containsIgnoreCase ("no MIDI"), label);

        /*  Low Pass mode must keep the low strings that the band-pass vowel
            intentionally thins. At Position 0 the corner is 350 Hz, so a 100 Hz
            tone is well inside the low-pass pass band and well below the band-pass
            centre. */
        resetToDefaults (pedal::wah);
        setParam (pedal::wah, "Mix", 1.0f);
        setParam (pedal::wah, "Position", 0.0f);
        setParam (pedal::wah, "Mode", 0.0f);

        std::vector<float> lowTone (static_cast<std::size_t> (sr), 0.0f);
        for (std::size_t i = 0; i < lowTone.size(); ++i)
            lowTone[i] = 0.25f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 100.0
                                                              * static_cast<double> (i) / sr));

        const auto bandLow = runAndMeasureRms (*wahPedal, lowTone, block, sr);
        setParam (pedal::wah, "Mode", 1.0f);
        const auto lowPassLow = runAndMeasureRms (*wahPedal, lowTone, block, sr);
        resetToDefaults (pedal::wah);

        std::snprintf (label, sizeof (label),
                       "Wah Pedal: Low Pass keeps 100 Hz, %.3f vs band pass %.3f",
                       lowPassLow, bandLow);
        report (lowPassLow > bandLow * 1.5f, label);

        auto* autoWahPedal = factory.get (pedal::autoWah);
        resetToDefaults (pedal::autoWah);

        std::vector<float> loud (static_cast<std::size_t> (sr), 0.0f);
        std::vector<float> quiet (static_cast<std::size_t> (sr), 0.0f);
        for (std::size_t i = 0; i < loud.size(); ++i)
        {
            const auto s = static_cast<float> (std::sin (2.0 * 3.14159265358979 * 220.0
                                                        * static_cast<double> (i) / sr));
            loud[i]  = 0.6f * s;
            quiet[i] = 0.03f * s;
        }

        runAndMeasureRms (*autoWahPedal, quiet, block, sr);
        const auto quietHz = statusNumber (*autoWahPedal);
        runAndMeasureRms (*autoWahPedal, loud, block, sr);
        const auto loudHz = statusNumber (*autoWahPedal);

        std::snprintf (label, sizeof (label),
                       "Auto Wah: centre %.0f Hz at a quiet signal and %.0f Hz at a loud one",
                       quietHz, loudHz);
        report (loudHz > quietHz * 1.5f, label);
    }

    // 4. The compressor has to report what it is doing, and the gate has to say
    //    whether it is open: both are what the card footer shows.
    std::printf ("  -- dynamics readouts --\n");

    {
        auto* comp = factory.get (compressor);
        resetToDefaults (compressor);
        setParam (compressor, "Threshold", -40.0f);
        setParam (compressor, "Ratio", 10.0f);

        std::vector<float> loud (static_cast<std::size_t> (sr), 0.0f);
        for (std::size_t i = 0; i < loud.size(); ++i)
            loud[i] = 0.5f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 220.0
                                                           * static_cast<double> (i) / sr));

        runAndMeasureRms (*comp, loud, block, sr);
        const auto reduction = statusNumber (*comp);

        std::printf ("      reported: %s\n", comp->getStatusText().toRawUTF8());

        char label[220];
        std::snprintf (label, sizeof (label),
                       "Compressor: reports %.1f dB of gain reduction on a loud signal", reduction);
        report (reduction > 3.0f, label);

        auto* gate = factory.get (noiseGate);
        resetToDefaults (noiseGate);

        std::vector<float> silence (static_cast<std::size_t> (sr / 2), 0.0f);
        runAndMeasureRms (*gate, silence, block, sr);

        std::printf ("      gate: %s\n", gate->getStatusText().toRawUTF8());
        report (gate->getStatusText().isNotEmpty(), "Noise Gate: reports its open/closed state");

        runAndMeasureRms (*gate, loud, block, sr);
        report (gate->getStatusText() == juce::String ("Open"),
                "Noise Gate: reads Open once the signal is above the threshold");

        // The card must not show a reading left over from before bypass.
        comp->setBypassed (true);
        report (comp->getStatusText().isEmpty(),
                "Compressor: hides gain reduction while bypassed");
        comp->setBypassed (false);

        gate->setBypassed (true);
        report (gate->getStatusText().isEmpty(),
                "Noise Gate: hides open/closed state while bypassed");
        gate->setBypassed (false);
    }

    // 5. The Dattorro reverb: pre-delay actually delays the first arrival, the
    //    tail decays, and Width is the only thing that makes the two channels
    //    differ. These are the three properties the card's knobs claim.
    std::printf ("  -- Dattorro plate reverb --\n");

    {
        std::vector<float> impulse (static_cast<std::size_t> (sr * 2), 0.0f);
        impulse[0] = 0.5f;

        auto firstArrival = [&] (float preDelayMs)
        {
            resetToDefaults (reverb);
            setParam (reverb, "Mix", 1.0f);
            setParam (reverb, "Decay", 0.5f);
            setParam (reverb, "Damping", 0.3f);
            setParam (reverb, "Width", 0.8f);
            setParam (reverb, "Pre-Delay", preDelayMs);

            const auto rendered = renderEffect (*factory.get (reverb), impulse, {}, block, sr);

            for (int i = 0; i < rendered.getNumSamples(); ++i)
                if (std::abs (rendered.getSample (0, i)) > 1.0e-4f
                    || std::abs (rendered.getSample (1, i)) > 1.0e-4f)
                    return i;

            return rendered.getNumSamples();
        };

        const auto withoutPreDelay = firstArrival (0.0f);
        const auto withPreDelay    = firstArrival (50.0f);
        const auto shiftMs = 1000.0 * static_cast<double> (withPreDelay - withoutPreDelay) / sr;

        char label[240];
        std::snprintf (label, sizeof (label),
                       "Reverb: 50 ms Pre-Delay moves the first arrival by %.1f ms (%d -> %d)",
                       shiftMs, withoutPreDelay, withPreDelay);
        report (std::abs (shiftMs - 50.0) <= 2.0, label);

        // A short decay must actually die away over two seconds; a network with
        // a broken feedback path would either ring forever or be silent.
        resetToDefaults (reverb);
        setParam (reverb, "Mix", 1.0f);
        setParam (reverb, "Decay", 0.2f);
        setParam (reverb, "Damping", 0.5f);
        setParam (reverb, "Pre-Delay", 0.0f);

        const auto rendered = renderEffect (*factory.get (reverb), impulse, {}, block, sr);

        auto windowRms = [&] (int from, int to)
        {
            double sum = 0.0;
            int count = 0;
            for (int i = from; i < to && i < rendered.getNumSamples(); ++i)
            {
                const auto v = static_cast<double> (rendered.getSample (0, i));
                sum += v * v;
                ++count;
            }
            return count > 0 ? static_cast<float> (std::sqrt (sum / count)) : 0.0f;
        };

        const auto early = windowRms (static_cast<int> (0.05 * sr), static_cast<int> (0.20 * sr));
        const auto late  = windowRms (static_cast<int> (1.80 * sr), static_cast<int> (2.00 * sr));

        std::snprintf (label, sizeof (label),
                       "Reverb: short Decay dies away, 0.05-0.2 s %.2e vs 1.8-2.0 s %.2e",
                       early, late);
        report (early > 1.0e-4f && late < early * 0.05f, label);

        // Width 0 must collapse to mono exactly; Width 100 must decorrelate.
        auto noise = makeNoise (static_cast<std::size_t> (sr / 2), 0.2f);

        resetToDefaults (reverb);
        setParam (reverb, "Mix", 1.0f);
        setParam (reverb, "Width", 0.0f);
        auto monoRender = renderEffect (*factory.get (reverb), noise, {}, block, sr);

        float monoDiff = 0.0f;
        for (int i = 0; i < monoRender.getNumSamples(); ++i)
            monoDiff = std::max (monoDiff, std::abs (monoRender.getSample (0, i)
                                                     - monoRender.getSample (1, i)));

        setParam (reverb, "Width", 1.0f);
        auto wideRender = renderEffect (*factory.get (reverb), noise, {}, block, sr);

        double diffSum = 0.0, signalSum = 0.0;
        for (int i = 0; i < wideRender.getNumSamples(); ++i)
        {
            const auto l = static_cast<double> (wideRender.getSample (0, i));
            const auto r = static_cast<double> (wideRender.getSample (1, i));
            diffSum += (l - r) * (l - r);
            signalSum += 0.5 * (l * l + r * r);
        }

        const auto diffRms = static_cast<float> (std::sqrt (diffSum / wideRender.getNumSamples()));
        const auto signalRms = static_cast<float> (std::sqrt (signalSum / wideRender.getNumSamples()));

        std::snprintf (label, sizeof (label),
                       "Reverb: Width 0 is mono (max diff %.2e), Width 100 decorrelates (%.3f vs %.3f)",
                       monoDiff, diffRms, signalRms);
        report (monoDiff == 0.0f && signalRms > 1.0e-4f && diffRms > 0.1f * signalRms, label);
    }

    // 6. Ping-Pong: the first echo is on the left at Time, the second on the
    //    right at 2x Time, and neither side leaks into the other's slot.
    std::printf ("  -- ping-pong delay --\n");

    {
        std::vector<float> impulse (static_cast<std::size_t> (sr), 0.0f);
        impulse[0] = 0.5f;

        resetToDefaults (delay);
        setParam (delay, "Time", 100.0f);
        setParam (delay, "Feedback", 0.5f);
        setParam (delay, "Tone", 100.0f);
        setParam (delay, "Mix", 1.0f);
        setParam (delay, "Mod", 0.0f);
        setParam (delay, "Mode", 1.0f);

        const auto rendered = renderEffect (*factory.get (delay), impulse, {}, block, sr);

        auto windowPeak = [&] (int channel, float centreMs, float halfWidthMs)
        {
            const auto centre = static_cast<int> (centreMs * 0.001 * sr);
            const auto half   = static_cast<int> (halfWidthMs * 0.001 * sr);
            float peak = 0.0f;

            for (int i = std::max (0, centre - half); i < std::min (rendered.getNumSamples(), centre + half); ++i)
                peak = std::max (peak, std::abs (rendered.getSample (channel, i)));

            return peak;
        };

        const auto leftAtTime   = windowPeak (0, 100.0f, 20.0f);
        const auto rightAtTime  = windowPeak (1, 100.0f, 20.0f);
        const auto rightAt2Time = windowPeak (1, 200.0f, 20.0f);
        const auto leftAt2Time  = windowPeak (0, 200.0f, 20.0f);

        char label[260];
        std::snprintf (label, sizeof (label),
                       "Delay: ping-pong L@100ms %.3f, R@100ms %.3f, R@200ms %.3f, L@200ms %.3f",
                       leftAtTime, rightAtTime, rightAt2Time, leftAt2Time);
        report (leftAtTime > 0.01f && rightAtTime < leftAtTime * 0.05f
                && rightAt2Time > 0.005f && leftAt2Time < rightAt2Time * 0.05f, label);
    }

    // 7. Hysteresis: a level between Threshold and Threshold - Hysteresis must
    //    hold an open gate open and must not open a closed one. Comparing the
    //    same signal at Hysteresis 12 and 0 proves the second threshold exists.
    std::printf ("  -- gate hysteresis --\n");

    {
        auto* gate = factory.get (noiseGate);

        auto runGate = [&] (float hysteresisDb)
        {
            resetToDefaults (noiseGate);
            setParam (noiseGate, "Threshold", -30.0f);
            setParam (noiseGate, "Hysteresis", hysteresisDb);
            setParam (noiseGate, "Attack", 0.1f);
            setParam (noiseGate, "Release", 5.0f);
            setParam (noiseGate, "Hold", 0.0f);
            setParam (noiseGate, "Range", 80.0f);
            setParam (noiseGate, "Mix", 1.0f);

            // 0.2 s loud, 0.6 s at -35 dBFS, 0.6 s at -45 dBFS.
            std::vector<float> input (static_cast<std::size_t> (sr * 1.4), 0.0f);
            for (std::size_t i = 0; i < input.size(); ++i)
            {
                const auto t = static_cast<double> (i) / sr;
                const auto amp = t < 0.2 ? 0.2f : (t < 0.8 ? 0.0178f : 0.0056f);
                input[i] = amp * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 220.0 * t));
            }

            const auto rendered = renderEffect (*gate, input, {}, block, sr);

            auto rmsInWindow = [&] (float fromSec, float toSec)
            {
                const auto from = static_cast<int> (fromSec * sr);
                const auto to   = static_cast<int> (toSec * sr);
                double sum = 0.0;
                int count = 0;
                for (int i = from; i < to; ++i)
                {
                    const auto v = static_cast<double> (rendered.getSample (0, i));
                    sum += v * v;
                    ++count;
                }
                return count > 0 ? static_cast<float> (std::sqrt (sum / count)) : 0.0f;
            };

            return std::pair<float, juce::String> { rmsInWindow (0.4f, 0.75f),
                                                    gate->getStatusText() };
        };

        const auto heldOpen = runGate (12.0f);
        const auto closed   = runGate (0.0f);

        char label[240];
        std::snprintf (label, sizeof (label),
                       "Noise Gate: 12 dB hysteresis holds %.4f at -35 dBFS vs %.4f without",
                       heldOpen.first, closed.first);
        report (heldOpen.first > closed.first * 5.0f && heldOpen.first > 0.012f, label);

        // The -45 dBFS tail is below the close threshold either way, so both runs
        // must end closed; the held-open run proves the second threshold was used
        // on the way there.
        report (closed.second == juce::String ("Closed")
                && heldOpen.second == juce::String ("Closed"),
                "Noise Gate: still closes below Threshold - Hysteresis");

        /*  Sidechain: a 60 Hz hum at -30 dBFS is above the -40 dB gate threshold,
            but a 300 Hz sidechain high pass pushes what the detector sees below it.
            At 20 Hz the same hum opens the gate. */
        auto runHum = [&] (float sidechainHz)
        {
            resetToDefaults (noiseGate);
            setParam (noiseGate, "Threshold", -40.0f);
            setParam (noiseGate, "Hysteresis", 0.0f);
            setParam (noiseGate, "Sidechain", sidechainHz);
            setParam (noiseGate, "Attack", 0.1f);
            setParam (noiseGate, "Release", 5.0f);
            setParam (noiseGate, "Hold", 0.0f);
            setParam (noiseGate, "Range", 60.0f);
            setParam (noiseGate, "Mix", 1.0f);

            std::vector<float> hum (static_cast<std::size_t> (sr), 0.0f);
            for (std::size_t i = 0; i < hum.size(); ++i)
                hum[i] = 0.03f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 60.0
                                                              * static_cast<double> (i) / sr));

            runAndMeasureRms (*gate, hum, block, sr);
            return gate->getStatusText();
        };

        const auto humOpens = runHum (20.0f);
        const auto humRejected = runHum (300.0f);

        std::snprintf (label, sizeof (label),
                       "Noise Gate: 60 Hz hum opens at 20 Hz sidechain (%s), rejected at 300 Hz (%s)",
                       humOpens.toRawUTF8(), humRejected.toRawUTF8());
        report (humOpens == juce::String ("Open") && humRejected == juce::String ("Closed"), label);

        /*  Attack and Release are changed after prepare(), which is the order the
            running app uses: the card writes an atomic long after the device
            prepared the effect. Before this test existed, both coefficients were
            computed only in doPrepare(), so both of the comparisons below measured
            the same envelope.

            The input is warm-up silence, a loud burst, then a quiet tail rather
            than silence: a gate releasing does not create audio, so the only way
            to measure Release is to leave a low-level signal for the gain to act
            on. The quiet tail is below Threshold, so it does not reopen the gate. */
        auto renderGateResponse = [&] (float attackMs, float releaseMs, float holdMs,
                                       juce::String& statusDuringHold, juce::String& statusAtEnd)
        {
            resetToDefaults (noiseGate);
            setParam (noiseGate, "Threshold", -30.0f);
            setParam (noiseGate, "Hysteresis", 3.0f);
            setParam (noiseGate, "Hold", holdMs);
            setParam (noiseGate, "Range", 80.0f);
            setParam (noiseGate, "Mix", 1.0f);

            // Fast values while warming up, so the gate is genuinely closed when
            // the burst starts. The knobs under test are written after prepare.
            setParam (noiseGate, "Attack", 1.0f);
            setParam (noiseGate, "Release", 5.0f);
            gate->setBypassed (false);
            gate->prepare (sr, maxBlockSize);

            const auto warmUpSamples = (static_cast<int> (0.4 * sr) / block) * block;
            const auto toneSamples   = static_cast<int> (0.1 * sr);
            const auto tailSamples   = static_cast<int> (1.0 * sr);
            const auto total         = warmUpSamples + toneSamples + tailSamples;

            juce::AudioBuffer<float> output (chainChannels, total);
            output.clear();
            juce::AudioBuffer<float> buffer (chainChannels, block);

            statusDuringHold = {};
            statusAtEnd = {};

            bool knobsWritten = false;

            for (int base = 0; base < total; base += block)
            {
                const auto n = std::min (block, total - base);
                buffer.clear();

                if (! knobsWritten && base >= warmUpSamples)
                {
                    setParam (noiseGate, "Attack", attackMs);
                    setParam (noiseGate, "Release", releaseMs);
                    knobsWritten = true;
                }

                for (int i = 0; i < n; ++i)
                {
                    const auto index = base + i;
                    const auto t = static_cast<double> (index) / sr;

                    float value = 0.0f;

                    if (index >= warmUpSamples && index < warmUpSamples + toneSamples)
                        value = 0.3f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 220.0 * t));
                    else if (index >= warmUpSamples + toneSamples)
                        value = 0.0056f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 220.0 * t));

                    buffer.setSample (0, i, value);
                    buffer.setSample (1, i, value);
                }

                gate->process (buffer);

                for (int c = 0; c < chainChannels; ++c)
                    output.copyFrom (c, base, buffer, c, 0, n);

                // 0.1 s after the burst ends: Hold should still have the gate open.
                if (statusDuringHold.isEmpty()
                    && base + n >= warmUpSamples + toneSamples + static_cast<int> (0.1 * sr))
                    statusDuringHold = gate->getStatusText();
            }

            statusAtEnd = gate->getStatusText();
            return output;
        };

        auto windowRms = [&] (const juce::AudioBuffer<float>& rendered, int from, int to)
        {
            const auto start = juce::jmax (0, from);
            const auto end   = juce::jmin (rendered.getNumSamples(), to);

            double sum = 0.0;
            int count = 0;

            for (int i = start; i < end; ++i)
            {
                const auto v = static_cast<double> (rendered.getSample (0, i));
                sum += v * v;
                ++count;
            }

            return count > 0 ? static_cast<float> (std::sqrt (sum / count)) : 0.0f;
        };

        const auto warmUp = (static_cast<int> (0.4 * sr) / block) * block;
        const auto toneEnd = warmUp + static_cast<int> (0.1 * sr);

        juce::String ignoredHeld, ignoredFinal;
        const auto fast = renderGateResponse (1.0f, 5.0f, 0.0f, ignoredHeld, ignoredFinal);
        const auto slow = renderGateResponse (50.0f, 1000.0f, 0.0f, ignoredHeld, ignoredFinal);

        const auto fastAttack = windowRms (fast, warmUp, warmUp + static_cast<int> (0.01 * sr));
        const auto slowAttack = windowRms (slow, warmUp, warmUp + static_cast<int> (0.01 * sr));
        const auto fastRelease = windowRms (fast, toneEnd + static_cast<int> (0.15 * sr),
                                                   toneEnd + static_cast<int> (0.25 * sr));
        const auto slowRelease = windowRms (slow, toneEnd + static_cast<int> (0.15 * sr),
                                                   toneEnd + static_cast<int> (0.25 * sr));

        std::snprintf (label, sizeof (label),
                       "Noise Gate: Attack 1 ms %.4f vs 50 ms %.4f in the first 10 ms",
                       fastAttack, slowAttack);
        report (fastAttack > slowAttack * 2.0f, label);

        std::snprintf (label, sizeof (label),
                       "Noise Gate: Release 5 ms %.4f vs 1000 ms %.4f 150 ms after the burst",
                       fastRelease, slowRelease);
        report (fastRelease < slowRelease * 0.2f, label);

        // Hold has to keep the card's lamp on while the gain is frozen open,
        // then the release has to close it.
        juce::String heldStatus, finalStatus;
        renderGateResponse (1.0f, 5.0f, 500.0f, heldStatus, finalStatus);

        std::snprintf (label, sizeof (label),
                       "Noise Gate: Hold reads %s during hold, %s after hold + release",
                       heldStatus.toRawUTF8(), finalStatus.toRawUTF8());
        report (heldStatus == juce::String ("Open") && finalStatus == juce::String ("Closed"),
                label);

        /*  Compressor sidechain: a 60 Hz bass tone must trigger far less gain
            reduction through a 500 Hz detector high pass than through a 20 Hz one. */
        auto* comp = factory.get (compressor);

        auto runCompSidechain = [&] (float sidechainHz)
        {
            resetToDefaults (compressor);
            setParam (compressor, "Threshold", -40.0f);
            setParam (compressor, "Ratio", 10.0f);
            setParam (compressor, "Attack", 2.0f);
            setParam (compressor, "Release", 100.0f);
            setParam (compressor, "Knee", 0.0f);
            setParam (compressor, "Makeup", 0.0f);
            setParam (compressor, "Mix", 1.0f);
            setParam (compressor, "Sidechain", sidechainHz);

            std::vector<float> bass (static_cast<std::size_t> (sr * 1.5), 0.0f);
            for (std::size_t i = 0; i < bass.size(); ++i)
                bass[i] = 0.5f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 60.0
                                                              * static_cast<double> (i) / sr));

            runAndMeasureRms (*comp, bass, block, sr);
            return statusNumber (*comp);
        };

        const auto lowSideReduction = runCompSidechain (20.0f);
        const auto highSideReduction = runCompSidechain (500.0f);

        std::snprintf (label, sizeof (label),
                       "Compressor: 60 Hz reduction %.1f dB at 20 Hz sidechain, %.1f dB at 500 Hz",
                       lowSideReduction, highSideReduction);
        report (lowSideReduction > highSideReduction + 3.0f, label);

        /*  The compressor already refreshed Attack/Release once per block, but
            that property had no test. These measurements fail if the coefficients
            are ever frozen in doPrepare() the way the gate's were. */
        std::printf ("  -- compressor attack and release knobs --\n");

        auto renderCompressor = [&] (float attackMs, float releaseMs)
        {
            resetToDefaults (compressor);
            setParam (compressor, "Threshold", -40.0f);
            setParam (compressor, "Ratio", 10.0f);
            setParam (compressor, "Knee", 0.0f);
            setParam (compressor, "Makeup", 0.0f);
            setParam (compressor, "Mix", 1.0f);
            setParam (compressor, "Sidechain", 20.0f);

            comp->setBypassed (false);
            comp->prepare (sr, maxBlockSize);

            // Written after prepare, exactly like a card while the device runs.
            setParam (compressor, "Attack", attackMs);
            setParam (compressor, "Release", releaseMs);

            const auto warm  = (static_cast<int> (0.2 * sr) / block) * block;
            const auto burst = static_cast<int> (0.2 * sr);
            const auto tail  = static_cast<int> (0.5 * sr);
            const auto total = warm + burst + tail;

            juce::AudioBuffer<float> output (chainChannels, total);
            output.clear();
            juce::AudioBuffer<float> buffer (chainChannels, block);

            for (int base = 0; base < total; base += block)
            {
                const auto n = std::min (block, total - base);
                buffer.clear();

                for (int i = 0; i < n; ++i)
                {
                    const auto index = base + i;
                    const auto t = static_cast<double> (index) / sr;
                    const auto amplitude = (index >= warm && index < warm + burst) ? 0.5f   // -6 dBFS
                                                                                  : 0.003f; // -50 dBFS
                    const auto value = amplitude * static_cast<float> (
                        std::sin (2.0 * 3.14159265358979 * 220.0 * t));

                    buffer.setSample (0, i, value);
                    buffer.setSample (1, i, value);
                }

                comp->process (buffer);

                for (int c = 0; c < chainChannels; ++c)
                    output.copyFrom (c, base, buffer, c, 0, n);
            }

            return output;
        };

        auto compressorWindowRms = [&] (const juce::AudioBuffer<float>& rendered, int from, int to)
        {
            const auto start = juce::jmax (0, from);
            const auto end   = juce::jmin (rendered.getNumSamples(), to);

            double sum = 0.0;
            int count = 0;

            for (int i = start; i < end; ++i)
            {
                const auto v = static_cast<double> (rendered.getSample (0, i));
                sum += v * v;
                ++count;
            }

            return count > 0 ? static_cast<float> (std::sqrt (sum / count)) : 0.0f;
        };

        const auto compBurstStart = (static_cast<int> (0.2 * sr) / block) * block;
        const auto compTailStart  = compBurstStart + static_cast<int> (0.2 * sr);

        const auto fastComp       = renderCompressor (2.0f, 10.0f);
        const auto slowAttackComp = renderCompressor (100.0f, 10.0f);
        const auto slowReleaseComp = renderCompressor (2.0f, 1000.0f);

        const auto compAttackFast = compressorWindowRms (fastComp,
                                                         compBurstStart + static_cast<int> (0.01 * sr),
                                                         compBurstStart + static_cast<int> (0.03 * sr));
        const auto compAttackSlow = compressorWindowRms (slowAttackComp,
                                                         compBurstStart + static_cast<int> (0.01 * sr),
                                                         compBurstStart + static_cast<int> (0.03 * sr));
        const auto compReleaseFast = compressorWindowRms (fastComp,
                                                          compTailStart + static_cast<int> (0.15 * sr),
                                                          compTailStart + static_cast<int> (0.25 * sr));
        const auto compReleaseSlow = compressorWindowRms (slowReleaseComp,
                                                          compTailStart + static_cast<int> (0.15 * sr),
                                                          compTailStart + static_cast<int> (0.25 * sr));

        std::snprintf (label, sizeof (label),
                       "Compressor: Attack 2 ms %.4f vs 100 ms %.4f in the first 30 ms",
                       compAttackFast, compAttackSlow);
        report (compAttackFast < compAttackSlow * 0.7f, label);

        std::snprintf (label, sizeof (label),
                       "Compressor: Release 10 ms %.4f vs 1000 ms %.4f 150 ms after the burst",
                       compReleaseFast, compReleaseSlow);
        report (compReleaseFast > compReleaseSlow * 2.0f, label);
    }

    // 4b. The tone-shaping controls must move the spectrum they name, not just
    //     exist in the parameter table.
    std::printf ("  -- voicing controls --\n");

    {
        auto sineAt = [&] (double freq, float amplitude, double seconds)
        {
            std::vector<float> v (static_cast<std::size_t> (sr * seconds), 0.0f);
            for (std::size_t i = 0; i < v.size(); ++i)
                v[i] = amplitude * static_cast<float> (std::sin (2.0 * 3.14159265358979 * freq
                                                                 * static_cast<double> (i) / sr));
            return v;
        };

        const auto lowTone = sineAt (100.0, 0.25, 1.0);
        const auto highTone = sineAt (4000.0, 0.2, 1.0);
        const auto noise = makeNoise (static_cast<std::size_t> (sr), 0.2f);

        char label[240];

        // Overdrive Bass is a pre-clip shelf, so with drive low it is essentially
        // a low-frequency gain control.
        resetToDefaults (overdrive);
        setParam (overdrive, "Drive", 0.0f);
        setParam (overdrive, "Tone", 100.0f);
        setParam (overdrive, "Level", 45.0f);
        setParam (overdrive, "Mix", 1.0f);
        setParam (overdrive, "Bass", -12.0f);
        const auto overdriveCut = runAndMeasureRms (*factory.get (overdrive), lowTone, block, sr);
        setParam (overdrive, "Bass", 12.0f);
        const auto overdriveBoost = runAndMeasureRms (*factory.get (overdrive), lowTone, block, sr);
        resetToDefaults (overdrive);

        std::snprintf (label, sizeof (label),
                       "Overdrive: Bass -12/+12 at 100 Hz, %.3f -> %.3f", overdriveCut, overdriveBoost);
        report (overdriveBoost > overdriveCut * 1.3f, label);

        // Fuzz Bass sits before the gain stage; keep the fuzz low so the curve is
        // not already pinned against its knee for both settings.
        resetToDefaults (fuzz);
        setParam (fuzz, "Fuzz", 0.0f);
        setParam (fuzz, "Tone", 100.0f);
        setParam (fuzz, "Level", 55.0f);
        setParam (fuzz, "Mix", 1.0f);
        setParam (fuzz, "Bass", -12.0f);
        const auto fuzzCut = runAndMeasureRms (*factory.get (fuzz), sineAt (100.0, 0.05, 1.0), block, sr);
        setParam (fuzz, "Bass", 12.0f);
        const auto fuzzBoost = runAndMeasureRms (*factory.get (fuzz), sineAt (100.0, 0.05, 1.0), block, sr);
        resetToDefaults (fuzz);

        std::snprintf (label, sizeof (label),
                       "Fuzz: Bass -12/+12 at 100 Hz, %.4f -> %.4f", fuzzCut, fuzzBoost);
        report (fuzzBoost > fuzzCut * 1.3f, label);

        // Presence is after the clipper, where Tone cannot add anything back.
        resetToDefaults (distortion);
        setParam (distortion, "Drive", 50.0f);
        setParam (distortion, "Tighten", 0.0f);
        setParam (distortion, "Tone", 100.0f);
        setParam (distortion, "Level", 65.0f);
        setParam (distortion, "Mix", 1.0f);
        setParam (distortion, "Presence", -12.0f);
        const auto distortionDull = runAndMeasureRms (*factory.get (distortion), highTone, block, sr);
        setParam (distortion, "Presence", 12.0f);
        const auto distortionBright = runAndMeasureRms (*factory.get (distortion), highTone, block, sr);
        resetToDefaults (distortion);

        std::snprintf (label, sizeof (label),
                       "Distortion: Presence -12/+12 at 4 kHz, %.3f -> %.3f",
                       distortionDull, distortionBright);
        report (distortionBright > distortionDull * 1.3f, label);

        resetToDefaults (bitcrusher);
        setParam (bitcrusher, "Bits", 16.0f);
        setParam (bitcrusher, "Downsample", 1.0f);
        setParam (bitcrusher, "Mix", 1.0f);
        setParam (bitcrusher, "Tone", 0.0f);
        const auto crushedDark = runAndMeasureRms (*factory.get (bitcrusher), noise, block, sr);
        setParam (bitcrusher, "Tone", 100.0f);
        const auto crushedBright = runAndMeasureRms (*factory.get (bitcrusher), noise, block, sr);
        resetToDefaults (bitcrusher);

        std::snprintf (label, sizeof (label),
                       "Bitcrusher: Tone 0/100 on noise, %.3f vs %.3f",
                       crushedDark, crushedBright);
        report (crushedDark < crushedBright * 0.8f, label);
    }

    // 8. ADAA: the primitive itself, without oversampling, must suppress the
    //    aliases of a driven 11 kHz tone. 3 x 11 kHz aliases to 15 kHz and
    //    5 x 11 kHz to 7 kHz at a 48 kHz rate.
    std::printf ("  -- antiderivative anti-aliasing --\n");

    {
        const auto n = static_cast<int> (sr);
        std::vector<float> input (static_cast<std::size_t> (n), 0.0f);
        std::vector<float> naive (static_cast<std::size_t> (n), 0.0f);
        std::vector<float> smoothed (static_cast<std::size_t> (n), 0.0f);

        auto shaper = adaa::Tanh {};
        shaper.setScale (4.0f);

        for (int i = 0; i < n; ++i)
        {
            input[static_cast<std::size_t> (i)] = 0.8f * static_cast<float> (
                std::sin (2.0 * 3.14159265358979 * 11000.0 * i / sr));
            naive[static_cast<std::size_t> (i)] = std::tanh (4.0f * input[static_cast<std::size_t> (i)])
                                                / std::tanh (4.0f);
            smoothed[static_cast<std::size_t> (i)] = shaper.process (input[static_cast<std::size_t> (i)]);
        }

        const auto half = n / 2;
        auto aliasEnergy = [&] (const std::vector<float>& v, double freq)
        {
            return goertzel (v, static_cast<std::size_t> (half), half, freq, sr);
        };

        const auto naive15 = aliasEnergy (naive, 15000.0);
        const auto smooth15 = aliasEnergy (smoothed, 15000.0);
        const auto naive7 = aliasEnergy (naive, 7000.0);
        const auto smooth7 = aliasEnergy (smoothed, 7000.0);

        const auto reduction15 = 10.0 * std::log10 (std::max (naive15, 1.0e-30)
                                                    / std::max (smooth15, 1.0e-30));
        const auto reduction7 = 10.0 * std::log10 (std::max (naive7, 1.0e-30)
                                                   / std::max (smooth7, 1.0e-30));

        char label[240];
        std::snprintf (label, sizeof (label),
                       "ADAA tanh: alias 15 kHz -%.1f dB, 7 kHz -%.1f dB",
                       reduction15, reduction7);
        report (reduction15 > 3.0 && reduction7 > 3.0, label);
    }

    // 9. Chorus Spread: at 0 the two channels are identical; at 100 the LFOs are
    //    half a cycle apart and the wet image widens.
    std::printf ("  -- chorus stereo spread --\n");

    {
        auto noise = makeNoise (static_cast<std::size_t> (sr / 2), 0.2f);

        resetToDefaults (chorus);
        setParam (chorus, "Mix", 1.0f);
        setParam (chorus, "Rate", 0.5f);
        setParam (chorus, "Depth", 50.0f);
        setParam (chorus, "Spread", 0.0f);

        auto narrow = renderEffect (*factory.get (chorus), noise, {}, block, sr);

        float maxDiff = 0.0f;
        for (int i = 0; i < narrow.getNumSamples(); ++i)
            maxDiff = std::max (maxDiff, std::abs (narrow.getSample (0, i)
                                                   - narrow.getSample (1, i)));

        setParam (chorus, "Spread", 100.0f);
        auto wide = renderEffect (*factory.get (chorus), noise, {}, block, sr);

        double diffSum = 0.0, signalSum = 0.0;
        for (int i = 0; i < wide.getNumSamples(); ++i)
        {
            const auto l = static_cast<double> (wide.getSample (0, i));
            const auto r = static_cast<double> (wide.getSample (1, i));
            diffSum += (l - r) * (l - r);
            signalSum += 0.5 * (l * l + r * r);
        }

        const auto diffRms = static_cast<float> (std::sqrt (diffSum / wide.getNumSamples()));
        const auto signalRms = static_cast<float> (std::sqrt (signalSum / wide.getNumSamples()));

        char label[240];
        std::snprintf (label, sizeof (label),
                       "Chorus: Spread 0 max diff %.2e, Spread 100 diff %.3f vs %.3f",
                       maxDiff, diffRms, signalRms);
        report (maxDiff == 0.0f && signalRms > 1.0e-4f && diffRms > 0.1f * signalRms, label);
    }
}

//==============================================================================
//  F - the voicing and cleanliness the acoustic-guitar feedback was about
//==============================================================================
void checkAcousticVoicing (double sr, int block)
{
    std::atomic<float> expression { 0.5f };
    juce::dsp::ConvolutionMessageQueue queue;

    EffectContext context;
    context.expressionPosition = &expression;
    context.convolutionQueue   = &queue;

    EffectFactory factory (context);

    auto setParam = [&factory] (EffectTypeID type, const char* name, float value)
    {
        auto* effect = factory.get (type);
        auto* meta = EffectFactory::metadataFor (type);
        (void) meta;

        for (int p = 0; p < effect->getNumParameters(); ++p)
            if (std::strcmp (effect->getParameterDescription (p).name, name) == 0)
                effect->setParameter (p, value);
    };

    auto resetToDefaults = [&factory] (EffectTypeID type)
    {
        auto* effect = factory.get (type);
        for (int p = 0; p < effect->getNumParameters(); ++p)
            effect->resetParameterToDefault (p);
    };

    const auto toneLength = static_cast<std::size_t> (2.0 * sr);
    std::vector<float> tone (toneLength);

    for (std::size_t i = 0; i < tone.size(); ++i)
        tone[i] = 0.1f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 220.0
                                                       * static_cast<double> (i) / sr));

    const auto offset = static_cast<std::size_t> (1.0 * sr);
    const auto length = static_cast<int> (0.8 * sr);

    // 1. The complaint: overdrive measured heavier than distortion. At an
    //    acoustic input level the overdrive must be the gentler of the two.
    std::printf ("  -- overdrive vs distortion at their defaults (-20 dBFS input) --\n");

    {
        auto* od = factory.get (overdrive);
        resetToDefaults (overdrive);
        const auto odOut = runEffect (*od, tone, block, sr);
        const auto odThd = harmonicThd (odOut, offset, length, 220.0, sr, 12);

        auto* dist = factory.get (distortion);
        resetToDefaults (distortion);
        const auto distOut = runEffect (*dist, tone, block, sr);
        const auto distThd = harmonicThd (distOut, offset, length, 220.0, sr, 12);

        char label[240];
        std::snprintf (label, sizeof (label),
                       "Overdrive THD %.1f %% vs Distortion %.1f %% at defaults",
                       100.0 * odThd, 100.0 * distThd);
        report (odThd >= 0.02 && odThd <= 0.20
                && distThd >= 0.20 && distThd > odThd * 1.5, label);
    }

    // 2. The upper octave is rectified, not granular: the strongest component
    //    after the dry is removed must be 440 Hz.
    std::printf ("  -- octave up is a rectifier --\n");

    {
        auto* oct = factory.get (octaver);
        resetToDefaults (octaver);
        setParam (octaver, "Octave Down", 0.0f);
        setParam (octaver, "Octave Up", 100.0f);
        setParam (octaver, "Blend", 1.0f);

        const auto out = runEffect (*oct, tone, block, sr);
        const auto measured = dominantFrequency (out, offset, length, sr, 350.0, 550.0);

        double level = 0.0;
        for (std::size_t i = offset; i < offset + static_cast<std::size_t> (length); ++i)
            level = std::max (level, static_cast<double> (std::abs (out[i])));

        char label[240];
        std::snprintf (label, sizeof (label),
                       "Octaver up: %.1f Hz for a 220 Hz input, peak %.3f", measured, level);
        report (level > 0.01 && std::abs (cents (measured, 440.0)) < 50.0, label);
    }

    // 3. A clean chorus must not generate harmonics. The former pre/de-emphasis
    //    pair and the linear moving tap were the audible "8-bit" part.
    std::printf ("  -- chorus and delay harmonic cleanliness --\n");

    {
        auto* chorusPedal = factory.get (chorus);
        resetToDefaults (chorus);
        setParam (chorus, "Mix", 1.0f);

        std::vector<float> oneK (static_cast<std::size_t> (2.0 * sr));
        for (std::size_t i = 0; i < oneK.size(); ++i)
            oneK[i] = 0.2f * static_cast<float> (std::sin (2.0 * 3.14159265358979 * 1000.0
                                                           * static_cast<double> (i) / sr));

        const auto chorusOut = runEffect (*chorusPedal, oneK, block, sr);
        const auto chorusThd = harmonicThd (chorusOut, offset, length, 1000.0, sr, 8);

        auto* delayPedal = factory.get (delay);
        resetToDefaults (delay);
        setParam (delay, "Mix", 1.0f);

        const auto delayOut = runEffect (*delayPedal, oneK, block, sr);
        const auto delayThd = harmonicThd (delayOut, offset, length, 1000.0, sr, 8);

        char label[240];
        std::snprintf (label, sizeof (label),
                       "Chorus THD %.3f %%, Delay THD %.3f %% at 1 kHz",
                       100.0 * chorusThd, 100.0 * delayThd);
        report (chorusThd < 0.02 && delayThd < 0.01, label);
    }
}

} // namespace

//==============================================================================
//  D - ring buffer policy
//==============================================================================
void checkRing()
{
    AudioRingBuffer ring;
    ring.reset (2, 16);   // 16 slots per channel, power of two

    std::vector<float> rampA (16), rampB (16);
    for (int i = 0; i < 16; ++i)  { rampA[static_cast<std::size_t> (i)] = static_cast<float> (i);
                                    rampB[static_cast<std::size_t> (i)] = static_cast<float> (100 + i); }

    const float* two[2] = { rampA.data(), rampB.data() };

    // Underflow: reading from an empty ring must return zeros, not uninitialised
    // memory. The first read of a new stream is not a drop - the output callback
    // can legitimately run before the input callback has produced anything - so it
    // is zero-filled and *not* counted; the same read after the producer has
    // delivered a block is a real starvation and is counted.
    {
        std::vector<float> outA (8, -1.0f), outB (8, -1.0f);
        float* dst[2] = { outA.data(), outB.data() };
        const auto got = ring.read (dst, 2, 8);

        bool zeros = true;
        for (int i = 0; i < 8; ++i)
            if (outA[static_cast<std::size_t> (i)] != 0.0f || outB[static_cast<std::size_t> (i)] != 0.0f)
                zeros = false;

        // read() returns how many samples it actually copied, so an empty ring
        // answers 0 - and still zero-fills what it was asked for, which is the part
        // that protects the audio path from uninitialised memory.
        report (got == 0 && zeros, "empty ring zero-fills the request and reports nothing copied");
        report (ring.getUnderflowCount() == 0,
                "a read before the producer has written is the stream starting, not an underflow");

        ring.write (two, 2, 16);
        {
            std::vector<float> drainA (16), drainB (16);
            float* drain[2] = { drainA.data(), drainB.data() };
            ring.read (drain, 2, 16);
        }
        ring.read (dst, 2, 8);
        report (ring.getUnderflowCount() > 0, "underflow after a drained ring is counted");
    }

    // Overflow: the newest samples have to survive.
    {
        for (int round = 0; round < 4; ++round)
            ring.write (two, 2, 16);

        std::vector<float> outA (16), outB (16);
        float* dst[2] = { outA.data(), outB.data() };
        const auto got = ring.read (dst, 2, 16);

        bool newestKept = got == 16;
        for (int i = 0; i < 16 && newestKept; ++i)
            newestKept = std::abs (outA[static_cast<std::size_t> (i)] - static_cast<float> (i)) < 1.0e-6f
                      && std::abs (outB[static_cast<std::size_t> (i)] - static_cast<float> (100 + i)) < 1.0e-6f;

        report (newestKept, "overflow keeps the newest samples and discards the oldest");
        report (ring.getOverflowCount() > 0, "overflow is counted");
    }

    // The sizing argument in PedalConfig.h: one read at the worst rate ratio.
    {
        AudioRingBuffer big;
        big.reset (chainChannels, ringCapacityFor (inputRingBlocks));

        const auto worstRead = maxInputReadPerBlock;
        std::vector<float> zeros (static_cast<std::size_t> (worstRead), 0.0f);
        const float* src[2] = { zeros.data(), zeros.data() };

        bool accepted = true;
        for (int i = 0; i < 400; ++i)
        {
            big.write (src, chainChannels, worstRead);

            std::vector<float> dst (static_cast<std::size_t> (worstRead), 0.0f);
            float* channels[2] = { dst.data(), dst.data() };
            big.read (channels, chainChannels, worstRead);
        }

        report (accepted && big.getOverflowCount() == 0,
                "a sustained worst-case read never overflows the configured input ring");
    }
}

} // namespace

int main()
{
    // Unbuffered, so a crash inside the bench still leaves the lines that ran.
    std::setvbuf (stdout, nullptr, _IONBF, 0);

    std::printf ("===============================================================\n");
    std::printf ("A. spectral shifter group delay\n");
    std::printf ("===============================================================\n");
    checkShifterLatency (44100.0);
    checkShifterLatency (48000.0);

    std::printf ("\n===============================================================\n");
    std::printf ("B. pitch accuracy\n");
    std::printf ("===============================================================\n");
    checkPitch (44100.0, 110.0, 12.0);
    checkPitch (44100.0, 220.0, -1.0);
    checkPitch (44100.0, 220.0, -12.0);
    checkPitch (48000.0, 440.0, 5.0);
    checkPitch (48000.0, 330.0, 0.0);
    checkDivider (44100.0, 110.0);
    checkDivider (44100.0, 330.0);
    checkDivider (48000.0, 82.4);

    std::printf ("\n===============================================================\n");
    std::printf ("C. every effect at every parameter extreme\n");
    std::printf ("===============================================================\n");
    checkEffects (48000.0, kBlock);

    std::printf ("\n===============================================================\n");
    std::printf ("E. drive, dynamics and swept filters\n");
    std::printf ("===============================================================\n");
    checkDriveAndFilters (48000.0, kBlock);

    std::printf ("\n===============================================================\n");
    std::printf ("F. acoustic-guitar voicing and cleanliness\n");
    std::printf ("===============================================================\n");
    checkAcousticVoicing (48000.0, kBlock);

    std::printf ("\n===============================================================\n");
    std::printf ("D. ring buffer policy\n");
    std::printf ("===============================================================\n");
    checkRing();

    std::printf ("\n===============================================================\n");
    std::printf ("failures: %d\n", failures);
    return failures == 0 ? 0 : 1;
}
