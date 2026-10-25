#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/*==============================================================================
  PedalBoard - small hand written DSP primitives.

  These exist instead of juce::dsp equivalents for one specific reason: several
  of the filters in this app have coefficients that change every audio block
  (auto-wah, envelope driven filters, phaser all-pass cascade). Building a
  juce::dsp::IIR::Coefficients object allocates a reference counted object, so
  the coefficient maths is done in place on a plain struct instead. Everything
  else keeps using juce::dsp where that class is the right tool.

  Filter designs: the static EQ sections are the standard second order RBJ audio
  EQ cookbook, the swept wah filters use the topology preserving transform state
  variable filter, and the clippers use first order antiderivative anti-aliasing.
  Delay lines are circular buffers with linear interpolation.
==============================================================================*/
namespace pedal
{

inline float clampf (float v, float lo, float hi) noexcept { return v < lo ? lo : (v > hi ? hi : v); }
inline float clamp01 (float v) noexcept { return clampf (v, 0.0f, 1.0f); }
inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
inline float limit1 (float v) noexcept { return clampf (v, -1.0f, 1.0f); }

/** One pole coefficient for a time constant given in milliseconds. */
inline float timeCoefficient (double sampleRate, float milliseconds) noexcept
{
    const auto seconds = std::max (0.0001f, milliseconds) * 0.001f;
    return 1.0f - std::exp (-1.0f / (static_cast<float> (sampleRate) * seconds));
}

/*==============================================================================
  Biquad - direct form I, coefficients pre-normalised by a0.
==============================================================================*/
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float x1 = 0.0f, x2 = 0.0f, y1 = 0.0f, y2 = 0.0f;

    /*  Target taps for glide(). A swept band-pass that jumps its coefficients
        every N samples rings at high Q - the state is kept, the poles are not -
        so the wah filters walk the taps over a millisecond instead. */
    float tb0 = 1.0f, tb1 = 0.0f, tb2 = 0.0f, ta1 = 0.0f, ta2 = 0.0f;

    void reset() noexcept { x1 = x2 = y1 = y2 = 0.0f; }

    void setPassthrough() noexcept
    {
        b0 = 1.0f; b1 = b2 = a1 = a2 = 0.0f;
        tb0 = 1.0f; tb1 = tb2 = ta1 = ta2 = 0.0f;
    }

    /** Design a band-pass into the targets; glide() walks towards it. */
    void setBandPassTarget (double sampleRate, double freqHz, double q) noexcept
    {
        double values[5];
        if (! compute (sampleRate, freqHz, q, 0.0, Mode::bandpass, values))
            { tb0 = 1.0f; tb1 = tb2 = ta1 = ta2 = 0.0f; return; }

        tb0 = static_cast<float> (values[0]);
        tb1 = static_cast<float> (values[1]);
        tb2 = static_cast<float> (values[2]);
        ta1 = static_cast<float> (values[3]);
        ta2 = static_cast<float> (values[4]);
    }

    void setPassthroughTarget() noexcept { tb0 = 1.0f; tb1 = tb2 = ta1 = ta2 = 0.0f; }

    void setTargetFromCurrent() noexcept
    {
        tb0 = b0; tb1 = b1; tb2 = b2; ta1 = a1; ta2 = a2;
    }

    /** Jump straight to the target, for the first block after a prepare. */
    void settle() noexcept
    {
        b0 = tb0; b1 = tb1; b2 = tb2; a1 = ta1; a2 = ta2;
    }

    void glide (float coefficient) noexcept
    {
        b0 += (tb0 - b0) * coefficient;
        b1 += (tb1 - b1) * coefficient;
        b2 += (tb2 - b2) * coefficient;
        a1 += (ta1 - a1) * coefficient;
        a2 += (ta2 - a2) * coefficient;
    }

    float process (float in) noexcept
    {
        const auto out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = in;
        y2 = y1; y1 = out;
        return out;
    }

    /** @returns false when the request was nonsensical and the section was left
        as a passthrough instead. Never allocates. */
    bool makeLowShelf (double sampleRate, double freqHz, double gainDb, double slope = 1.0) noexcept
    {
        return setShelf (sampleRate, freqHz, gainDb, slope, false);
    }

    bool makeHighShelf (double sampleRate, double freqHz, double gainDb, double slope = 1.0) noexcept
    {
        return setShelf (sampleRate, freqHz, gainDb, slope, true);
    }

    bool makePeak (double sampleRate, double freqHz, double q, double gainDb) noexcept
    {
        double values[5];
        if (! compute (sampleRate, freqHz, q, gainDb, Mode::peak, values))
            { setPassthrough(); return false; }
        assign (values);
        return true;
    }

    bool makeBandPass (double sampleRate, double freqHz, double q) noexcept
    {
        double values[5];
        if (! compute (sampleRate, freqHz, q, 0.0, Mode::bandpass, values))
            { setPassthrough(); return false; }
        assign (values);
        return true;
    }

    /** Second order all-pass, used by the phaser cascade. */
    bool makeAllPass (double sampleRate, double freqHz, double q) noexcept
    {
        double values[5];
        if (! compute (sampleRate, freqHz, q, 0.0, Mode::allpass, values))
            { setPassthrough(); return false; }
        assign (values);
        return true;
    }

    bool makeLowPass (double sampleRate, double freqHz, double q = 0.7071) noexcept
    {
        double values[5];
        if (! compute (sampleRate, freqHz, q, 0.0, Mode::lowpass, values))
            { setPassthrough(); return false; }
        assign (values);
        return true;
    }

    bool makeHighPass (double sampleRate, double freqHz, double q = 0.7071) noexcept
    {
        double values[5];
        if (! compute (sampleRate, freqHz, q, 0.0, Mode::highpass, values))
            { setPassthrough(); return false; }
        assign (values);
        return true;
    }

private:
    enum class Mode { peak, bandpass, allpass, lowpass, highpass, lowShelf, highShelf };

    void assign (const double* v) noexcept
    {
        b0 = static_cast<float> (v[0]); b1 = static_cast<float> (v[1]); b2 = static_cast<float> (v[2]);
        a1 = static_cast<float> (v[3]); a2 = static_cast<float> (v[4]);
        tb0 = b0; tb1 = b1; tb2 = b2; ta1 = a1; ta2 = a2;
    }

    bool setShelf (double sampleRate, double freqHz, double gainDb, double slope, bool high) noexcept
    {
        Mode m = high ? Mode::highShelf : Mode::lowShelf;
        double values[5];
        if (! compute (sampleRate, freqHz, slope, gainDb, m, values))
            { setPassthrough(); return false; }
        assign (values);
        return true;
    }

    static bool valid (double sampleRate, double freqHz, double q) noexcept
    {
        return sampleRate > 0.0 && q > 0.05
            && freqHz > 1.0 && freqHz < sampleRate * 0.48;
    }

    static bool compute (double sampleRate, double freqHz, double qOrSlope, double gainDb,
                         Mode mode, double* out) noexcept
    {
        if (! valid (sampleRate, freqHz, qOrSlope))
            return false;

        const auto w0  = 2.0 * pi() * freqHz / sampleRate;
        const auto cw  = std::cos (w0);
        const auto sw  = std::sin (w0);
        const auto a   = std::pow (10.0, gainDb / 40.0);

        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

        switch (mode)
        {
            case Mode::peak:
            {
                const auto alpha = sw / (2.0 * qOrSlope);
                b0 = 1.0 + alpha * a;   b1 = -2.0 * cw;   b2 = 1.0 - alpha * a;
                a0 = 1.0 + alpha / a;   a1 = -2.0 * cw;   a2 = 1.0 - alpha / a;
                break;
            }
            case Mode::bandpass:
            {
                const auto alpha = sw / (2.0 * qOrSlope);
                b0 = alpha;   b1 = 0.0;   b2 = -alpha;
                a0 = 1.0 + alpha;  a1 = -2.0 * cw;  a2 = 1.0 - alpha;
                break;
            }
            case Mode::allpass:
            {
                const auto alpha = sw / (2.0 * qOrSlope);
                a0 = 1.0 + alpha;  a1 = -2.0 * cw;  a2 = 1.0 - alpha;
                b0 = a2;   b1 = a1;   b2 = a0;
                break;
            }
            case Mode::lowpass:
            {
                const auto alpha = sw / (2.0 * qOrSlope);
                b0 = (1.0 - cw) / 2.0;  b1 = 1.0 - cw;  b2 = (1.0 - cw) / 2.0;
                a0 = 1.0 + alpha;  a1 = -2.0 * cw;  a2 = 1.0 - alpha;
                break;
            }
            case Mode::highpass:
            {
                const auto alpha = sw / (2.0 * qOrSlope);
                b0 = (1.0 + cw) / 2.0;  b1 = -(1.0 + cw);  b2 = (1.0 + cw) / 2.0;
                a0 = 1.0 + alpha;  a1 = -2.0 * cw;  a2 = 1.0 - alpha;
                break;
            }
            case Mode::lowShelf:
            case Mode::highShelf:
            {
                // qOrSlope is the shelf slope S here; alpha as defined by the
                // cookbook for a shelf with slope S at the corner frequency.
                const auto alpha = (sw / 2.0)
                    * std::sqrt ((a + 1.0 / a) * (1.0 / qOrSlope - 1.0) + 2.0 * a);
                const auto twoSA = 2.0 * std::sqrt (a) * alpha;

                if (mode == Mode::lowShelf)
                {
                    b0 =      a * ((a + 1.0) - (a - 1.0) * cw + twoSA);
                    b1 =  2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
                    b2 =      a * ((a + 1.0) - (a - 1.0) * cw - twoSA);
                    a0 =          (a + 1.0) + (a - 1.0) * cw + twoSA;
                    a1 =     -2.0 * ((a - 1.0) + (a + 1.0) * cw);
                    a2 =          (a + 1.0) + (a - 1.0) * cw - twoSA;
                }
                else
                {
                    b0 =      a * ((a + 1.0) + (a - 1.0) * cw + twoSA);
                    b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cw);
                    b2 =      a * ((a + 1.0) + (a - 1.0) * cw - twoSA);
                    a0 =          (a + 1.0) - (a - 1.0) * cw + twoSA;
                    a1 =      2.0 * ((a - 1.0) - (a + 1.0) * cw);
                    a2 =          (a + 1.0) - (a - 1.0) * cw - twoSA;
                }
                break;
            }
        }

        out[0] = b0 / a0; out[1] = b1 / a0; out[2] = b2 / a0;
        out[3] = a1 / a0; out[4] = a2 / a0;
        return true;
    }

    static double pi() noexcept { return 3.14159265358979323846; }
};

/*==============================================================================
  TptSvf - topology preserving transform state variable filter.

  The RBJ cookbook sections above are designed for a fixed corner. A swept
  filter changes its coefficients while it runs, and direct form II transposed
  (what the cookbook sections normalise to) can ring or jump when the poles
  move quickly. The TPT/ZDF state variable filter integrates the analogue
  topology itself, so its state stays meaningful while cutoff and Q move; it is
  the structure Zavalishin's "Art of VA Filter Design" and Cytomic's SVF are
  built on, and it is what the wah pedals and the envelope filter now use.

  Coefficients (Cytomic form):
      g  = tan (pi * fc / fs)
      k  = 1 / Q
      a1 = 1 / (1 + g * (g + k)),  a2 = g * a1,  a3 = g * a2
  The band output has a peak gain of Q, so a caller that wants unity peak
  multiplies by k.
==============================================================================*/
struct SvfOutputs
{
    float low = 0.0f, band = 0.0f, high = 0.0f;
};

struct TptSvf
{
    float ic1eq = 0.0f, ic2eq = 0.0f;
    float k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

    void reset() noexcept { ic1eq = ic2eq = 0.0f; }

    void setCutoffQ (double sr, double freqHz, double q) noexcept
    {
        const auto nyquist = sr * 0.5;
        const auto hz = std::max (10.0, std::min (freqHz, nyquist * 0.98));
        const auto qq = std::max (0.15, q);
        const auto g  = static_cast<float> (std::tan (3.14159265358979323846 * hz / sr));

        k  = static_cast<float> (1.0 / qq);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    /** One sample through the analogue prototype. State is kept in the
        integrators, so moving cutoff/Q between samples is safe. */
    SvfOutputs process (float v0) noexcept
    {
        const auto v3 = v0 - ic2eq;
        const auto v1 = a1 * ic1eq + a2 * v3;
        const auto v2 = ic2eq + a2 * ic1eq + a3 * v3;

        ic1eq = 2.0f * v1 - ic1eq;
        ic2eq = 2.0f * v2 - ic2eq;

        return { v2, v1, v0 - k * v1 - v2 };
    }
};

/*==============================================================================
  OnePole - single pole lowpass / highpass used for cheap "tone" controls.
==============================================================================*/
struct OnePole
{
    float coefficient = 0.0f, feedback = 1.0f, state = 0.0f, lastInput = 0.0f;
    bool highPass = false;

    void setLowPass (double sampleRate, double freqHz) noexcept
    {
        highPass = false;
        coefficient = static_cast<float> (1.0 - std::exp (-2.0 * 3.14159265358979 * freqHz / sampleRate));
        feedback = 1.0f - coefficient;
    }

    void setHighPass (double sampleRate, double freqHz) noexcept
    {
        highPass = true;
        coefficient = static_cast<float> (std::exp (-2.0 * 3.14159265358979 * freqHz / sampleRate));
        feedback = coefficient;
    }

    void reset() noexcept { state = 0.0f; lastInput = 0.0f; }

    float process (float in) noexcept
    {
        if (highPass)
        {
            const auto out = feedback * (state + in - lastInput);
            state = out; lastInput = in;
            return out;
        }

        state = in * coefficient + feedback * state;
        return state;
    }
};

/*==============================================================================
  EnvelopeFollower - peak detector with independent attack and release.
==============================================================================*/
struct EnvelopeFollower
{
    float attack = 0.5f, release = 0.05f, value = 0.0f;

    void setTimes (double sampleRate, float attackMs, float releaseMs) noexcept;
    void reset() noexcept { value = 0.0f; }

    float process (float absInput) noexcept
    {
        const auto coeff = absInput > value ? attack : release;
        value += (absInput - value) * coeff;
        return value;
    }
};

inline void EnvelopeFollower::setTimes (double sampleRate, float attackMs, float releaseMs) noexcept
{
    attack  = timeCoefficient (sampleRate, attackMs);
    release = timeCoefficient (sampleRate, releaseMs);
}

/*==============================================================================
  TappedDelay - circular multi channel delay with linear read interpolation.

  Prepared once with a fixed capacity; reading and writing are per sample so the
  delay time can be modulated every sample (flanger, vibrato, ping-pong delay,
  dry path latency compensation).
==============================================================================*/
/**
    Per-sample usage, which is what allows modulation of the delay time:

        delay.write (channel, sample);
        const auto out = delay.read (channel, delayInSamples);
        delay.advance();          // once per sample, after all channels are written
*/
class TappedDelay
{
public:
    void prepare (int numChannels, int capacitySamples) noexcept
    {
        auto size = 1;
        while (size < capacitySamples + 4)
            size <<= 1;

        mask = size - 1;
        stride = size;
        channels = numChannels;
        maximumDelay = capacitySamples;
        storage.assign (static_cast<std::size_t> (numChannels) * static_cast<std::size_t> (size), 0.0f);
        position = 0;
    }

    void reset() noexcept { std::fill (storage.begin(), storage.end(), 0.0f); }

    int getCapacity() const noexcept { return maximumDelay; }
    int getNumChannels() const noexcept { return channels; }

    void write (int channel, float sample) noexcept
    {
        storage[static_cast<std::size_t> (channel) * stride
                + static_cast<std::size_t> (position & mask)] = sample;
    }

    /** Advances the shared write head. Call once per sample. */
    void advance() noexcept { ++position; position &= mask; }

    float read (int channel, float delaySamples) const noexcept
    {
        const auto d = clampf (delaySamples, 0.0f, static_cast<float> (maximumDelay));
        const auto whole = static_cast<int> (d);
        const auto frac = d - static_cast<float> (whole);

        const auto* base = storage.data() + static_cast<std::size_t> (channel) * stride;
        const auto s0 = base[static_cast<std::size_t> ((position - whole) & mask)];

        if (frac <= 0.0f)
            return s0;

        const auto s1 = base[static_cast<std::size_t> ((position - whole - 1) & mask)];
        return s0 + (s1 - s0) * frac;
    }

    /** Catmull-Rom read for modulated taps. Linear interpolation between two
        samples is a lowpass whose corner moves with the tap, and on a moving
        tap that produces audible HF grit at every LFO turnaround; the four
        point kernel keeps a fractional-delay read close to transparent.
        Needs two samples of history and one of look-behind, so the delay is
        clamped to [2, capacity - 2]. */
    float readCubic (int channel, float delaySamples) const noexcept
    {
        const auto d = clampf (delaySamples, 2.0f, static_cast<float> (maximumDelay - 1));
        const auto whole = static_cast<int> (d);
        const auto t = d - static_cast<float> (whole);

        const auto* base = storage.data() + static_cast<std::size_t> (channel) * stride;
        const auto x0 = base[static_cast<std::size_t> ((position - whole + 1) & mask)];
        const auto x1 = base[static_cast<std::size_t> ((position - whole) & mask)];
        const auto x2 = base[static_cast<std::size_t> ((position - whole - 1) & mask)];
        const auto x3 = base[static_cast<std::size_t> ((position - whole - 2) & mask)];

        const auto c0 = x1;
        const auto c1 = 0.5f * (x2 - x0);
        const auto c2 = x0 - 2.5f * x1 + 2.0f * x2 - 0.5f * x3;
        const auto c3 = 0.5f * (x3 - x0) + 1.5f * (x1 - x2);

        return ((c3 * t + c2) * t + c1) * t + c0;
    }

private:
    std::vector<float> storage;
    int stride = 0, mask = 0, channels = 0, maximumDelay = 0, position = 0;
};

/*==============================================================================
  Modulator - phase continuous LFO for tremolo / vibrato / phaser / ring mod.
==============================================================================*/
enum class LfoShape : int { sine = 0, triangle = 1, square = 2, sawtooth = 3 };

class Modulator
{
public:
    void prepare (double sampleRate) noexcept { sr = static_cast<float> (sampleRate); }

    void reset() noexcept { phase = 0.0f; }

    /** Advances by rateHz and returns the value in [-1, 1]. */
    float step (float rateHz, LfoShape shape) noexcept
    {
        phase += rateHz / sr;
        if (phase >= 1.0f)  phase -= 1.0f;
        if (phase < 0.0f)   phase += 1.0f;
        return value (phase, shape);
    }

    float valueAt (float p, LfoShape shape) const noexcept
    {
        p -= std::floor (p);
        return value (p, shape);
    }

    static float value (float p, LfoShape shape) noexcept
    {
        switch (shape)
        {
            case LfoShape::triangle: return 4.0f * std::abs (p - 0.5f) - 1.0f;
            case LfoShape::square:   return p < 0.5f ? 1.0f : -1.0f;
            case LfoShape::sawtooth: return 2.0f * p - 1.0f;
            case LfoShape::sine:
            default:                 return std::sin (2.0f * 3.14159265358979f * p);
        }
    }

    /** Free running phase for cases where the caller wants to read and set it. */
    float getPhase() const noexcept { return phase; }
    void  setPhase (float p) noexcept { phase = p - std::floor (p); }

private:
    float phase = 0.0f, sr = 44100.0f;
};

/** Soft clipping curves. Each is normalised so f(1) == 1. */
namespace shaping
{
    inline float softSaturation (float x, float drive) noexcept
    {
        const auto k = drive;
        return std::tanh (x * k) / std::tanh (k);
    }

    /** Piecewise linear break curve: transparent below the threshold, heavily
        squashed above it. Audibly harder than softSaturation(). */
    inline float hardClip (float x, float threshold, float residualSlope) noexcept
    {
        const auto t = std::max (0.02f, threshold);
        const auto a = std::abs (x);
        if (a <= t)
            return x;
        const auto over = a - t;
        const auto y = t + over * residualSlope / (1.0f + over * residualSlope);
        return x < 0.0f ? -y : y;
    }

    /** Asymmetric transfer curve: different break points for each polarity,
        which produces the even harmonics that give a fuzz its spitty edge. */
    inline float asymmetricFuzz (float x, float positiveKnee, float negativeKnee) noexcept
    {
        if (x >= 0.0f)
        {
            const auto k = std::max (0.02f, positiveKnee);
            return x / (k + std::abs (x)) * k;
        }

        const auto k = std::max (0.02f, negativeKnee);
        return -((0.0f - x) / (k + std::abs (x)) * k);
    }
}

/*==============================================================================
  First order antiderivative anti-aliasing (ADAA), following Chowdhury's
  "Practical Considerations for Antiderivative Anti-Aliasing".

  A waveshaper evaluated at the sample points folds the harmonics it generates
  above Nyquist straight back into the audible band. First order ADAA averages
  the nonlinearity over each sample interval instead:

      y[n] = (F(x[n]) - F(x[n-1])) / (x[n] - x[n-1])

  where F is the antiderivative of the curve. It costs one extra log/exp per
  sample and, together with the pedals' 4x oversampling, removes the residual
  aliasing that oversampling alone leaves near full-scale drive. The fallback
  at vanishing differences is the curve at the midpoint, which is the limit of
  the quotient and avoids dividing by zero.
==============================================================================*/
namespace adaa
{

inline float logCosh (float x) noexcept
{
    const auto a = std::abs (x);
    return a + std::log1p (std::exp (-2.0f * a)) - 0.6931471805599453f;
}

/** tanh(k*x)/tanh(k), with F(x) = log(cosh(k*x)) / (k*tanh(k)).
    setScale() is called when the drive-dependent hardness moves; the stored
    antiderivative is recomputed at the last input so the quotient stays
    consistent with the new curve instead of mixing two shapes for one sample. */
struct Tanh
{
    void reset() noexcept { primed = false; }

    void setScale (float k) noexcept
    {
        const auto newScale = std::max (1.0e-3f, k);

        if (newScale != scale)
        {
            scale = newScale;
            norm = std::tanh (scale);

            if (primed)
                fPrev = antiderivative (xPrev);
        }
    }

    float process (float x) noexcept
    {
        const auto f = curve (x);
        const auto F = antiderivative (x);

        if (! primed)
        {
            primed = true;
            xPrev = x;
            fPrev = F;
            return f;
        }

        const auto dx = x - xPrev;
        const auto y = std::abs (dx) > 1.0e-6f
                         ? (F - fPrev) / dx
                         : curve (0.5f * (x + xPrev));

        xPrev = x;
        fPrev = F;
        return y;
    }

private:
    float curve (float x) const noexcept
    {
        return std::tanh (scale * x) / norm;
    }

    float antiderivative (float x) const noexcept
    {
        return logCosh (scale * x) / (scale * norm);
    }

    bool primed = false;
    float xPrev = 0.0f, fPrev = 0.0f;
    float scale = 1.0f, norm = 0.7615942f;
};

/** shaping::hardClip with fixed break point and residual slope. */
struct HardClip
{
    float threshold = 0.55f, residual = 0.08f;

    void reset() noexcept { primed = false; }

    float process (float x) noexcept
    {
        const auto f = shaping::hardClip (x, threshold, residual);
        const auto F = antiderivative (x);

        if (! primed)
        {
            primed = true;
            xPrev = x;
            fPrev = F;
            return f;
        }

        const auto dx = x - xPrev;
        const auto y = std::abs (dx) > 1.0e-6f
                         ? (F - fPrev) / dx
                         : shaping::hardClip (0.5f * (x + xPrev), threshold, residual);

        xPrev = x;
        fPrev = F;
        return y;
    }

private:
    float antiderivative (float x) const noexcept
    {
        const auto t = std::max (0.02f, threshold);
        const auto s = std::max (0.0f, residual);
        const auto a = std::abs (x);

        float F = 0.5f * a * a;

        if (a > t)
        {
            const auto u = a - t;
            F = 0.5f * t * t + t * u + u
                - (s > 1.0e-6f ? std::log1p (s * u) / s : 0.0f);
        }

        // hardClip is odd, so its antiderivative is even: F(-x) == F(x).
        return F;
    }

    bool primed = false;
    float xPrev = 0.0f, fPrev = 0.0f;
};

/** shaping::asymmetricFuzz with per-polarity knees. Changing a knee restarts
    the history: the antiderivative changes shape, so the previous value is no
    longer comparable. One sample of difference, on a user gesture. */
struct AsymmetricFuzz
{
    void setKnees (float positive, float negative) noexcept
    {
        const auto p = std::max (0.02f, positive);
        const auto n = std::max (0.02f, negative);

        if (p != positiveKnee || n != negativeKnee)
        {
            positiveKnee = p;
            negativeKnee = n;
            primed = false;
        }
    }

    void reset() noexcept { primed = false; }

    float process (float x) noexcept
    {
        const auto f = shaping::asymmetricFuzz (x, positiveKnee, negativeKnee);
        const auto F = antiderivative (x);

        if (! primed)
        {
            primed = true;
            xPrev = x;
            fPrev = F;
            return f;
        }

        const auto dx = x - xPrev;
        const auto y = std::abs (dx) > 1.0e-6f
                         ? (F - fPrev) / dx
                         : shaping::asymmetricFuzz (0.5f * (x + xPrev),
                                                    positiveKnee, negativeKnee);

        xPrev = x;
        fPrev = F;
        return y;
    }

private:
    float antiderivative (float x) const noexcept
    {
        if (x >= 0.0f)
            return positiveKnee * (x - positiveKnee * std::log (positiveKnee + x))
                 + positiveKnee * positiveKnee * std::log (positiveKnee);

        return -negativeKnee * x
             - negativeKnee * negativeKnee * std::log (negativeKnee - x)
             + negativeKnee * negativeKnee * std::log (negativeKnee);
    }

    float positiveKnee = 0.3f, negativeKnee = 0.3f;
    bool primed = false;
    float xPrev = 0.0f, fPrev = 0.0f;
};

} // namespace adaa

} // namespace pedal
