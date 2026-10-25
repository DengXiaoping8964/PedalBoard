#pragma once

#include "Effects/PedalEffect.h"

#include <cmath>

/*==============================================================================
  Delay

  Algorithm: hand written on pedal::TappedDelay (circular line, linear read
  interpolation) instead of juce::dsp::DelayLine, because the taps are modulated
  per sample - which is what a tape delay does and what makes ping-pong image
  move.

  Stereo mode delays each channel only itself. Ping-Pong mode uses two lines of
  the same length with crossed feedback and feeds the input into the left line
  only, so successive repeats alternate sides at the Time setting: left at T,
  right at 2T, left at 3T, and so on. The earlier version put the right tap at
  half the left delay and fed both lines, which produced a faster, uneven
  pattern rather than a ping-pong.

  Tone is a pedal::OnePole lowpass inside each feedback loop, so every repeat is
  lowpassed once more and the tail darkens while it decays. Mod adds tape
  wow/flutter: a slow 0.33 Hz LFO plus a 5.4 Hz flutter component modulate the
  read taps by up to 2.5 ms, with the two channels offset in phase. The net
  effect is the gentle pitch drift and chorus of a tape echo, not a vibrato;
  Mod at 0 is a clean digital repeat.

  Feedback is hard clamped to 0.95 so the loop always decays to silence, and
  both taps are clamped to [1, capacity - 2] samples: one sample minimum means
  the value written this frame can never be read back (no algebraic loop), the
  upper clamp keeps the interpolated read inside the line.

  The wet scratch buffer holds the echoes only, never the dry signal, so the
  Mix knob blends repeats against the chain signal and Mix = 0 is the untouched
  string signal.
==============================================================================*/
namespace pedal
{

class DelayEffect : public PedalEffect
{
public:
    enum Params { time, feedback, tone, mix, mode, modulation, numParams };

    /** Backing store for the discrete Mode selector. Function local static with
        constant initialisation, so the pointer the parameter table keeps is the
        same one for every translation unit and needs no lazy initialisation. */
    static const char* const* modeLabels() noexcept
    {
        static const char* const names[] = { "Stereo", "Ping-Pong" };
        return names;
    }

    DelayEffect()
        : PedalEffect (delay, "Delay",
            {
                { "Time",     1.0f, 2000.0f, 380.0f, "ms" },
                { "Feedback", 0.0f,    0.95f,  0.35f },
                { "Tone",     0.0f,  100.0f,   60.0f, "%" },
                { "Mix",      0.0f,    1.0f,   0.30f },
                { "Mode",     0.0f,    1.0f,    0.0f, "", 2, modeLabels() },
                { "Mod",      0.0f,  100.0f,    0.0f, "%" },
            })
    {
    }

    void doPrepare (double newSampleRate, int) override
    {
        // Sized for the longest delay the knob offers plus the full modulation
        // excursion: 2005 ms, which is a few megabytes per channel at 192 kHz.
        // Allocated once here, while the device is stopped; process() never
        // resizes anything.
        capacity = juce::jmax (4, static_cast<int> ((maxDelayMilliseconds + 5.0f) * 0.001 * newSampleRate));
        delayLine.prepare (chainChannels, capacity + 2);

        lastToneHz = -1.0f;     // make process() rebuild the one pole coefficients

        wowLeft.prepare (newSampleRate);
        wowRight.prepare (newSampleRate);
        flutterLeft.prepare (newSampleRate);
        flutterRight.prepare (newSampleRate);

        wowRight.setPhase (0.25f);
        flutterLeft.setPhase (0.13f);
        flutterRight.setPhase (0.50f);
    }

    void resetState() noexcept override
    {
        delayLine.reset();
        toneL.reset();
        toneR.reset();
        wowLeft.reset();
        wowRight.reset();
        flutterLeft.reset();
        flutterRight.reset();
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        updateToneFilters();

        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);

        const auto fb       = clampf (getParameter (feedback), 0.0f, 0.95f);
        const auto pingPong = getParameter (mode) >= 0.5f;
        const auto timeMs   = clampf (getParameter (time), minDelayMilliseconds, maxDelayMilliseconds);

        // Tap length in samples. The read is linearly interpolated, so turning
        // Time glides the pitch of the repeats instead of stepping through it.
        const auto baseTap = timeMs * 0.001f * static_cast<float> (sampleRate);
        const auto modDepth = clamp01 (getParameter (modulation) * 0.01f)
                            * 2.5f * 0.001f * static_cast<float> (sampleRate);
        const auto slowest = static_cast<float> (capacity - 2);

        const auto* srcL = audio.getReadPointer (0);
        const auto* srcR = numChannels > 1 ? audio.getReadPointer (1) : srcL;

        auto& wet = wetBufferForBlock();
        auto* outL = wet.getWritePointer (0);
        auto* outR = wet.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            // Wow is the slow capstan drift; flutter is the faster scrape
            // flutter. Offsetting the phases per channel keeps the image from
            // collapsing when both sides modulate in lockstep.
            const auto driftL = 0.7f * wowLeft.step (0.33f, LfoShape::sine)
                              + 0.3f * flutterLeft.step (5.4f, LfoShape::sine);
            const auto driftR = 0.7f * wowRight.step (0.37f, LfoShape::sine)
                              + 0.3f * flutterRight.step (5.7f, LfoShape::sine);

            const auto tapL = clampf (baseTap + driftL * modDepth, 1.0f, slowest);
            const auto tapR = clampf (baseTap + driftR * modDepth, 1.0f, slowest);

            // Cubic reads: the tape modulation moves the tap every sample, and a
            // linearly interpolated moving tap adds a metallic HF edge that was
            // audible on acoustic guitar as "the delay distorts".
            const auto l = delayLine.readCubic (0, tapL);
            const auto r = delayLine.readCubic (1, tapR);

            // The one poles are inside the loop, so each pass adds one more
            // stage of lowpassing: repeat n is filtered n times.
            const auto fbL = fb * toneL.process (l);
            const auto fbR = fb * toneR.process (r);

            outL[i] = l;
            outR[i] = r;

            if (pingPong)
            {
                // Input enters the left line only; each repeat then crosses to
                // the other side, giving left at T, right at 2T, left at 3T.
                delayLine.write (0, 0.5f * (srcL[i] + srcR[i]) + fbR);
                delayLine.write (1, fbL);
            }
            else
            {
                delayLine.write (0, srcL[i] + fbL);
                delayLine.write (1, srcR[i] + fbR);
            }

            delayLine.advance();
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    static constexpr float maxDelayMilliseconds = 2000.0f;
    static constexpr float minDelayMilliseconds = 1.0f;

    /** Tone 0..100 sweeps the loop corner exponentially: 120 Hz at the bottom
        (muffled dub echoes) up to just under Nyquist at the top (no colouration),
        so the whole knob range is usable at every sample rate. */
    static float toneCutoffHz (float tonePercent, float sr) noexcept
    {
        const auto top = sr * 0.45f;
        const auto t = clampf (tonePercent, 0.0f, 100.0f) * 0.01f;
        return 120.0f * std::pow (top / 120.0f, t);
    }

    /** Cached on both the sample rate and the corner, so the two exponentials
        per coefficient update only run when something actually moved. */
    void updateToneFilters() noexcept
    {
        const auto hz = toneCutoffHz (getParameter (tone), static_cast<float> (sampleRate));

        if (hz != lastToneHz || static_cast<float> (sampleRate) != lastToneRate)
        {
            toneL.setLowPass (sampleRate, hz);
            toneR.setLowPass (sampleRate, hz);
            lastToneHz   = hz;
            lastToneRate = static_cast<float> (sampleRate);
        }
    }

    TappedDelay delayLine;
    OnePole toneL, toneR;
    Modulator wowLeft, wowRight, flutterLeft, flutterRight;

    float lastToneHz = -1.0f;
    float lastToneRate = 0.0f;
    int capacity = 0;
};

} // namespace pedal
