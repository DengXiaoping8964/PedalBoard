#pragma once

#include "Effects/PedalEffect.h"

#include <array>

/*==============================================================================
  Flanger

  Algorithm: hand written on pedal::TappedDelay + pedal::Modulator. The JUCE
  chorus widget can be pushed into a flanger (short centre delay plus feedback),
  but its delay line is sized for a 100 ms chorus and its internal dry mix is
  fixed by its own DryWetMixer, so a hand written line is both cheaper and exact.

  What makes a flanger a flanger and not a chorus: the delay is swept through a
  very short range (roughly 1..10 ms here, and down to near zero at low settings)
  and the delayed output is fed back into the delay input. Feedback turns the
  comb into a resonator, which is the moving jet-plane peak a chorus, with its
  zero feedback, never produces.

  The feedback is crossed between the two channels - the left tap drives the right
  delay line and vice versa - so the two combs have slightly different histories
  and the stereo image opens and closes as the LFO sweeps. Spread offsets the
  right channel's LFO phase from 0 to 180 degrees: at 0 the two sides sweep
  together, at 100 % they move half a cycle apart and the jet sound crosses the
  image. A 180 Hz high pass in the feedback path keeps the low
  strings out of the resonance, which is what stops a deep flanger from turning
  the bottom end into mud.

  Everything is sized in doPrepare: the line holds 15 ms at the prepared rate,
  which covers the whole Centre x Depth range with headroom for the clamp.
==============================================================================*/
namespace pedal
{

class FlangerEffect : public PedalEffect
{
public:
    enum Params { rate, depth, centre, feedback, mix, spread, numParams };

    /** Depth 100 % swings the tap +/- 4 ms around Centre, which with Centre up to
        10 ms stays inside the 15 ms line. */
    static constexpr float maxDepthMs = 4.0f;

    FlangerEffect()
        : PedalEffect (flanger, "Flanger",
            {
                { "Rate",     0.02f,  2.00f,  0.40f, "Hz" },
                { "Depth",    0.0f, 100.0f,   85.0f, "%" },
                { "Centre",   0.5f,  10.0f,    1.5f,  "ms" },
                { "Feedback", 0.0f,   0.95f,   0.55f },
                { "Mix",      0.0f,   1.0f,    0.5f },
                { "Spread",   0.0f, 100.0f,   85.0f, "%" },
            })
    {
    }

    void doPrepare (double newSampleRate, int newBlockSize) override
    {
        static_cast<void> (newBlockSize);

        // 15 ms of history: Centre tops out at 10 ms and the deepest sweep still
        // needs headroom below the capacity - 2 clamp.
        delay.prepare (chainChannels, samplesFor (newSampleRate, 15.0f));
        modulator.prepare (newSampleRate);

        for (auto& filter : feedbackHighPass)
        {
            filter.setHighPass (newSampleRate, 180.0);
            filter.reset();
        }
    }

    void resetState() noexcept override
    {
        delay.reset();
        modulator.reset();

        for (auto& filter : feedbackHighPass)
            filter.reset();
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);

        const auto rateHz        = clampf (getParameter (rate), 0.01f, 20.0f);
        const auto depthSamples  = getParameter (depth) * 0.01f * maxDepthMs * 0.001f
                                 * static_cast<float> (sampleRate);
        const auto centreSamples = getParameter (centre) * 0.001f * static_cast<float> (sampleRate);
        const auto fb            = clamp01 (getParameter (feedback)) * 0.95f;
        const auto spreadPhase   = clamp01 (getParameter (Params::spread) * 0.01f) * 0.5f;
        const auto maxDelay      = static_cast<float> (juce::jmax (2, delay.getCapacity() - 2));

        const auto* inL = audio.getReadPointer (0);
        const auto* inR = numChannels > 1 ? audio.getReadPointer (1) : inL;

        auto& wet = wetBufferForBlock();
        auto* outL = wet.getWritePointer (0);
        auto* outR = wet.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto phase = modulator.step (rateHz, LfoShape::sine);
            const auto lfoL  = modulator.valueAt (phase, LfoShape::sine);
            const auto lfoR  = modulator.valueAt (phase + spreadPhase, LfoShape::sine);

            // The tap is clamped into [2, capacity - 2]. The cubic read needs one
            // sample either side; two samples is still effectively zero delay for
            // the jet, and it stops the tap reading the slot the write head is
            // about to fill (which is 15 ms old and would jump the sweep).
            const auto tapDelayL = clampf (centreSamples + lfoL * depthSamples, 2.0f, maxDelay);
            const auto tapDelayR = clampf (centreSamples + lfoR * depthSamples, 2.0f, maxDelay);

            // Taps first, writes second, so the crossed feedback loop can only ever
            // see the previous sample and never solves for itself.
            const auto tapL = delay.readCubic (0, tapDelayL);
            const auto tapR = delay.readCubic (1, tapDelayR);

            const auto fbL = feedbackHighPass[0].process (tapL);
            const auto fbR = feedbackHighPass[1].process (tapR);

            delay.write (0, inL[i] + fb * fbR);   // crossed feedback: R's tap feeds
            delay.write (1, inR[i] + fb * fbL);   // L's line and the other way round
            delay.advance();

            outL[i] = tapL;
            outR[i] = tapR;
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    static int samplesFor (double rate, float milliseconds) noexcept
    {
        return juce::jmax (8, static_cast<int> (milliseconds * 0.001 * rate));
    }

    TappedDelay delay;
    Modulator modulator;
    std::array<OnePole, chainChannels> feedbackHighPass;
};

} // namespace pedal
