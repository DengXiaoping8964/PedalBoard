#pragma once

#include "Effects/PedalEffect.h"

#include <algorithm>
#include <cmath>

/*==============================================================================
  Auto Wah

  Algorithm: envelope driven resonant band-pass built on pedal::TptSvf, the same
  topology preserving state variable filter as the wah pedal. Nothing in
  juce::dsp does this - dsp::Oscillator would give an LFO, and an auto wah has
  to follow the playing dynamics instead, so the sweep source is the input
  envelope, not a clock.

  pedal::EnvelopeFollower tracks the peak magnitude of both channels, and its
  value maps exponentially onto the frequency range:
      freq = minFreq * pow (maxFreq / minFreq, position)
  Higher sensitivity pushes the same playing level further along that curve.
  Direction reverses the mapping, which is the "down" mode on a Mu-Tron III and
  gives the nasal, ducking filter sound rather than the usual quack.

  The TPT filter is designed per sample from a log-domain smoothed cutoff, so an
  envelope that jumps on a pick attack moves the corner quickly without the
  state-variable coefficients being invalidated mid-sweep. Q is smoothed the
  same way because a resonance step is a click.

  Output is scaled by 1/sqrt(Q) relative to the SVF's Q-times band gain, leaving
  a net sqrt(Q) lift: a band-pass lets less of a broadband guitar through as it
  narrows, and an auto wah that drops 6 dB when it opens up is not usable in a
  chain.

  The envelope maps to the sweep through a square root, not linearly. A linear
  amplitude map spends most of the knob's travel in the top of the range, where
  every note is already loud; the square root spreads the quieter half of the
  picking dynamics across the lower part of the sweep where it can be heard.
==============================================================================*/
namespace pedal
{

class AutoWahEffect : public PedalEffect
{
public:
    enum Params { sensitivity, minFreq, maxFreq, q, attack, release, mix, direction, numParams };

    static const char* const* directionLabels() noexcept
    {
        static const char* const names[] = { "Up Sweep", "Down Sweep" };
        return names;
    }

    AutoWahEffect()
        : PedalEffect (autoWah, "Auto Wah",
            {
                { "Sensitivity",   0.0f,  100.0f,   60.0f },
                { "Min Freq",    100.0f, 1500.0f,  320.0f, "Hz" },
                { "Max Freq",    800.0f, 8000.0f, 3600.0f, "Hz" },
                { "Q",             0.5f,   12.0f,    3.0f },
                { "Attack",        0.5f,   50.0f,    8.0f, "ms" },
                { "Release",      10.0f, 1000.0f,  120.0f, "ms" },
                { "Mix",           0.0f,    1.0f,    1.0f },
                { "Direction",     0.0f,    1.0f,    0.0f, "", 2, AutoWahEffect::directionLabels() },
            })
    {
    }

    void doPrepare (double, int) override
    {
        follower.setTimes (sampleRate, getParameter (attack), getParameter (release));
        cachedAttack  = getParameter (attack);
        cachedRelease = getParameter (release);
        coefficientRate = static_cast<float> (sampleRate);
        glideCoeff = timeCoefficient (sampleRate, 4.0f);

        currentHz  = targetFor (0.0f);
        currentQ   = std::max (0.5f, getParameter (Params::q));
        logCurrent = std::log (currentHz);

        for (auto& section : band)
            section.reset();

        centreHz.store (currentHz, std::memory_order_relaxed);
        retuneCounter = retuneInterval;
        reportCounter = reportInterval;
    }

    void resetState() noexcept override
    {
        follower.reset();

        for (auto& section : band)
            section.reset();

        currentHz  = targetFor (0.0f);
        currentQ   = std::max (0.5f, getParameter (Params::q));
        logCurrent = std::log (currentHz);
    }

    juce::String getStatusText() const override
    {
        return juce::String (centreHz.load (std::memory_order_relaxed), 0) + " Hz";
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        if (getParameter (attack) != cachedAttack
            || getParameter (release) != cachedRelease
            || sampleRate != coefficientRate)
        {
            follower.setTimes (sampleRate, getParameter (attack), getParameter (release));
            cachedAttack  = getParameter (attack);
            cachedRelease = getParameter (release);
            coefficientRate = static_cast<float> (sampleRate);
        }

        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);
        auto& wet = wetBufferForBlock();

        const auto targetQ  = std::max (0.5f, getParameter (Params::q));
        const auto downward = getParameter (direction) >= 0.5f;

        auto logTarget = std::log (currentHz);

        for (int i = 0; i < numSamples; ++i)
        {
            float level = 0.0f;
            for (int c = 0; c < numChannels; ++c)
                level = std::max (level, std::abs (audio.getReadPointer (c)[i]));

            // the follower itself must run per sample or the sweep loses pick attack
            const auto env = follower.process (level);

            // Recomputing pow() per sample buys nothing: the filter is smoothed
            // anyway, and the envelope is already the fast part of the sweep.
            if (--retuneCounter <= 0)
            {
                retuneCounter = retuneInterval;

                const auto position = clamp01 (std::sqrt (clamp01 (env))
                                               * getParameter (sensitivity) * 0.02f);
                logTarget = std::log (targetFor (downward ? 1.0f - position : position));
            }

            logCurrent += (logTarget - logCurrent) * glideCoeff;
            currentHz   = std::exp (logCurrent);
            currentQ   += (targetQ - currentQ) * glideCoeff;

            const auto makeup = 1.0f / std::sqrt (std::max (0.5f, currentQ));

            for (int c = 0; c < numChannels; ++c)
            {
                auto& section = band[c];
                section.setCutoffQ (sampleRate, currentHz, currentQ);
                const auto out = section.process (audio.getReadPointer (c)[i]);
                wet.getWritePointer (c)[i] = out.band * makeup;
            }

            if (--reportCounter <= 0)
            {
                reportCounter = reportInterval;
                centreHz.store (currentHz, std::memory_order_relaxed);
            }
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    /** Maps a 0..1 sweep position onto the frequency range. The two knobs are
        independent, so either can be turned past the other; taking them in
        order makes that a wide sweep in the other direction instead of
        collapsing the range to 1 Hz. */
    float targetFor (float position) const noexcept
    {
        const auto lowKnob  = getParameter (minFreq);
        const auto highKnob = getParameter (maxFreq);
        const auto minimum  = std::max (20.0f, std::min (lowKnob, highKnob));
        const auto maximum  = std::max (minimum + 1.0f, std::max (lowKnob, highKnob));

        return minimum * std::pow (maximum / minimum, clamp01 (position));
    }

    static constexpr int retuneInterval = 8;
    static constexpr int reportInterval = 16;

    EnvelopeFollower follower;
    std::atomic<float> centreHz { 320.0f };
    TptSvf band[chainChannels];
    float glideCoeff = 0.004f;
    float currentHz = 320.0f, currentQ = 3.0f, logCurrent = 0.0f;
    float cachedAttack = -1.0f, cachedRelease = -1.0f;
    float coefficientRate = 0.0f;
    int retuneCounter = retuneInterval, reportCounter = reportInterval;
};

} // namespace pedal
