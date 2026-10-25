#pragma once

#include "Effects/PedalEffect.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

/*==============================================================================
  Reverb - Dattorro plate

  Algorithm: Jon Dattorro's plate reverberator, "Effect Design, Part 1:
  Reverberator and Other Filters" (JAES 1997, figure 8). It is used here instead
  of juce::dsp::Reverb because the JUCE class is a Freeverb network with a fixed
  topology and no pre-delay; Dattorro's design gives a denser, smoother plate
  tail and exposes the controls a guitar reverb actually needs.

  Signal flow (all delay lengths are Dattorro's 44.1 kHz tunings, scaled by the
  current sample rate):

      mono input -> pre-delay (0..100 ms)
                 -> 4 input diffusers 142/107/379/277 samples,
                    all-pass gains 0.75/0.75/0.625/0.625
                 -> two cross-coupled tanks:
                      left : AP 672 (modulated) -> 4453 -> damping LP
                             -> AP 1800 -> 3720
                      right: AP 908 (modulated) -> 4217 -> damping LP
                             -> AP 2656 -> 3163
                    each tank's second delay feeds the other tank's input,
                    scaled by the Decay gain
                 -> 7 output taps per side, weights +-0.6, exactly the tap
                    positions from Dattorro's figure
                 -> DC blocker -> width -> Mix

  The two first tank all-passes are modulated at about 1 Hz with a +-16 sample
  excursion, which is what keeps a static tail from sounding metallic. Damping
  is the one-pole in each tank loop: a higher setting removes high frequencies
  from every pass, so the tail darkens as it decays.

  Everything is allocated in doPrepare() and processed sample by sample; there
  is no allocation and no lock in process(). The network has no fixed group
  delay, so getLatencySamples() stays 0.

  Reference: https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf
==============================================================================*/
namespace pedal
{

class ReverbEffect : public PedalEffect
{
public:
    enum Params { decay, damping, width, mix, preDelay, numParams };

    ReverbEffect()
        : PedalEffect (reverb, "Reverb",
            {
                { "Decay",     0.0f,   1.0f,  0.55f },
                { "Damping",   0.0f,   1.0f,  0.40f },
                { "Width",     0.0f,   1.0f,  0.80f },
                { "Mix",       0.0f,   1.0f,  0.30f },
                { "Pre-Delay", 0.0f, 100.0f, 20.0f, "ms" },
            })
    {
    }

    void doPrepare (double newSampleRate, int) override
    {
        const auto scale = newSampleRate / tuningRate;

        auto scaled = [scale] (double samples) noexcept
        {
            return juce::jmax (1, static_cast<int> (std::lround (samples * scale)));
        };

        // Dattorro's tunings, scaled and given a little headroom for the
        // modulated all-passes.
        inAp1Len = scaled (142);   inAp2Len = scaled (107);
        inAp3Len = scaled (379);   inAp4Len = scaled (277);

        ap1LeftLen  = scaled (672);   delay1LeftLen  = scaled (4453);
        ap2LeftLen  = scaled (1800);  delay2LeftLen  = scaled (3720);
        ap1RightLen = scaled (908);   delay1RightLen = scaled (4217);
        ap2RightLen = scaled (2656);  delay2RightLen = scaled (3163);

        tapL1 = scaled (266);   tapL2 = scaled (2974);
        tapL3 = scaled (1913);  tapL4 = scaled (1996);
        tapL5 = scaled (1990);  tapL6 = scaled (187);   tapL7 = scaled (1066);

        tapR1 = scaled (353);   tapR2 = scaled (3627);
        tapR3 = scaled (1228);  tapR4 = scaled (2673);
        tapR5 = scaled (2111);  tapR6 = scaled (335);   tapR7 = scaled (121);

        modulationDepth = 16.0f * static_cast<float> (scale);

        inAp1.prepare (inAp1Len + 4);
        inAp2.prepare (inAp2Len + 4);
        inAp3.prepare (inAp3Len + 4);
        inAp4.prepare (inAp4Len + 4);

        ap1Left.prepare  (ap1LeftLen  + static_cast<int> (modulationDepth) + 8);
        delay1Left.prepare (delay1LeftLen + 4);
        ap2Left.prepare  (ap2LeftLen + 4);
        delay2Left.prepare (delay2LeftLen + 4);

        ap1Right.prepare (ap1RightLen + static_cast<int> (modulationDepth) + 8);
        delay1Right.prepare (delay1RightLen + 4);
        ap2Right.prepare (ap2RightLen + 4);
        delay2Right.prepare (delay2RightLen + 4);

        preDelayLine.prepare (juce::jmax (8, static_cast<int> (0.100 * newSampleRate)) + 4);

        lfoLeft.prepare (newSampleRate);
        lfoRight.prepare (newSampleRate);

        resetState();
    }

    void resetState() noexcept override
    {
        inAp1.reset(); inAp2.reset(); inAp3.reset(); inAp4.reset();
        ap1Left.reset(); delay1Left.reset(); ap2Left.reset(); delay2Left.reset();
        ap1Right.reset(); delay1Right.reset(); ap2Right.reset(); delay2Right.reset();
        preDelayLine.reset();
        lfoLeft.reset();
        lfoRight.reset();

        dampLeft = dampRight = 0.0f;
        lastLeft2 = lastRight2 = 0.0f;
        dcXLeft = dcYLeft = dcXRight = dcYRight = 0.0f;
    }

    /** A feedback network has no fixed group delay to compensate for. */
    int getLatencySamples() const noexcept override { return 0; }

    void process (juce::AudioBuffer<float>& audio) override
    {
        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);

        auto& wet = wetBufferForBlock();

        const auto decayGain  = 0.2f + 0.75f * clamp01 (getParameter (decay));
        const auto dampCoeff  = 0.05f + 0.90f * clamp01 (getParameter (damping));
        const auto widthValue = clamp01 (getParameter (width));
        const auto preDelay   = clampf (getParameter (Params::preDelay), 0.0f, 100.0f)
                              * 0.001f * static_cast<float> (sampleRate);

        const auto* inL = audio.getReadPointer (0);
        const auto* inR = numChannels > 1 ? audio.getReadPointer (1) : inL;
        auto* outL = wet.getWritePointer (0);
        auto* outR = wet.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto dry = 0.5f * (inL[i] + inR[i]);

            // Pre-delay: write first so zero pre-delay is the current sample.
            preDelayLine.write (dry);
            auto x = preDelayLine.read (static_cast<int> (preDelay));

            // Four input diffusers in series.
            x = allPass (inAp1, inAp1Len, 0.75f, x);
            x = allPass (inAp2, inAp2Len, 0.75f, x);
            x = allPass (inAp3, inAp3Len, 0.625f, x);
            x = allPass (inAp4, inAp4Len, 0.625f, x);

            const auto modL = lfoLeft.step  (0.9f,  LfoShape::sine) * modulationDepth;
            const auto modR = lfoRight.step (1.1f,  LfoShape::sine) * modulationDepth;

            // Cross-coupled tanks: each second delay drives the other side.
            const auto inLeft  = x + decayGain * lastRight2;
            const auto inRight = x + decayGain * lastLeft2;

            // --- left tank ---------------------------------------------------
            const auto tapA1L = ap1Left.readFrac (static_cast<float> (ap1LeftLen) + modL);
            const auto write1L = inLeft + 0.70f * tapA1L;
            const auto outA1L = tapA1L - 0.70f * write1L;
            ap1Left.write (write1L);

            const auto delay1L = delay1Left.read (delay1LeftLen);
            delay1Left.write (outA1L);

            dampLeft = delay1L + dampCoeff * (dampLeft - delay1L);

            const auto tapA2L = ap2Left.read (ap2LeftLen);
            const auto write2L = dampLeft + 0.50f * tapA2L;
            const auto outA2L = tapA2L - 0.50f * write2L;
            ap2Left.write (write2L);

            const auto delay2L = delay2Left.read (delay2LeftLen);
            delay2Left.write (outA2L);

            // --- right tank --------------------------------------------------
            const auto tapA1R = ap1Right.readFrac (static_cast<float> (ap1RightLen) + modR);
            const auto write1R = inRight + 0.70f * tapA1R;
            const auto outA1R = tapA1R - 0.70f * write1R;
            ap1Right.write (write1R);

            const auto delay1R = delay1Right.read (delay1RightLen);
            delay1Right.write (outA1R);

            dampRight = delay1R + dampCoeff * (dampRight - delay1R);

            const auto tapA2R = ap2Right.read (ap2RightLen);
            const auto write2R = dampRight + 0.50f * tapA2R;
            const auto outA2R = tapA2R - 0.50f * write2R;
            ap2Right.write (write2R);

            const auto delay2R = delay2Right.read (delay2RightLen);
            delay2Right.write (outA2R);

            lastLeft2  = delay2L;
            lastRight2 = delay2R;

            // --- Dattorro's output taps --------------------------------------
            const auto rawLeft = 0.6f * (delay1Right.read (tapR1)
                                       + delay1Right.read (tapR2)
                                       - ap2Right.read (tapR3)
                                       + delay2Right.read (tapR4)
                                       - delay1Left.read (tapR5)
                                       - ap2Left.read (tapR6)
                                       - delay2Left.read (tapR7));

            const auto rawRight = 0.6f * (delay1Left.read (tapL1)
                                        + delay1Left.read (tapL2)
                                        - ap2Left.read (tapL3)
                                        + delay2Left.read (tapL4)
                                        - delay1Right.read (tapL5)
                                        - ap2Right.read (tapL6)
                                        - delay2Right.read (tapL7));

            // Remove any DC the all-pass sums leave behind.
            const auto blockedLeft  = rawLeft  - dcXLeft  + 0.995f * dcYLeft;
            const auto blockedRight = rawRight - dcXRight + 0.995f * dcYRight;
            dcXLeft = rawLeft;   dcYLeft = blockedLeft;
            dcXRight = rawRight; dcYRight = blockedRight;

            const auto mid  = 0.5f * (blockedLeft + blockedRight);
            const auto side = 0.5f * (blockedLeft - blockedRight) * widthValue;

            outL[i] = mid + side;
            outR[i] = mid - side;

            // One write head per line, advanced once per sample.
            inAp1.advance(); inAp2.advance(); inAp3.advance(); inAp4.advance();
            ap1Left.advance(); delay1Left.advance(); ap2Left.advance(); delay2Left.advance();
            ap1Right.advance(); delay1Right.advance(); ap2Right.advance(); delay2Right.advance();
            preDelayLine.advance();
        }

        crossFade (audio, wet, getParameter (mix));
    }

private:
    static constexpr double tuningRate = 44100.0;

    /** Circular line with integer and fractional reads. The fractional read is
        only used by the modulated tank all-passes. */
    struct Line
    {
        void prepare (int capacitySamples)
        {
            auto size = 1;
            while (size < capacitySamples + 4)
                size <<= 1;

            buffer.assign (static_cast<std::size_t> (size), 0.0f);
            mask = size - 1;
            position = 0;
            maximumDelay = capacitySamples;
        }

        void reset() noexcept
        {
            std::fill (buffer.begin(), buffer.end(), 0.0f);
            position = 0;
        }

        void write (float sample) noexcept
        {
            buffer[static_cast<std::size_t> (position & mask)] = sample;
        }

        void advance() noexcept { position = (position + 1) & mask; }

        float read (int delaySamples) const noexcept
        {
            const auto d = juce::jlimit (0, maximumDelay, delaySamples);
            return buffer[static_cast<std::size_t> ((position - d) & mask)];
        }

        float readFrac (float delaySamples) const noexcept
        {
            const auto d = clampf (delaySamples, 1.0f, static_cast<float> (maximumDelay));
            const auto whole = static_cast<int> (d);
            const auto frac  = d - static_cast<float> (whole);
            const auto s0 = buffer[static_cast<std::size_t> ((position - whole) & mask)];

            if (frac <= 0.0f)
                return s0;

            const auto s1 = buffer[static_cast<std::size_t> ((position - whole - 1) & mask)];
            return s0 + (s1 - s0) * frac;
        }

    private:
        std::vector<float> buffer;
        int mask = 0, position = 0, maximumDelay = 0;
    };

    /** Single all-pass: y = -g*x + v, written value = x + g*v. Reads first,
        writes second, so the loop never has to solve for itself. */
    static float allPass (Line& line, int length, float gain, float x) noexcept
    {
        const auto delayed = line.read (length);
        const auto w = x + gain * delayed;
        line.write (w);
        return delayed - gain * w;
    }

    Line inAp1, inAp2, inAp3, inAp4;
    Line ap1Left, delay1Left, ap2Left, delay2Left;
    Line ap1Right, delay1Right, ap2Right, delay2Right;
    Line preDelayLine;

    Modulator lfoLeft, lfoRight;

    int inAp1Len = 142, inAp2Len = 107, inAp3Len = 379, inAp4Len = 277;
    int ap1LeftLen = 672, delay1LeftLen = 4453, ap2LeftLen = 1800, delay2LeftLen = 3720;
    int ap1RightLen = 908, delay1RightLen = 4217, ap2RightLen = 2656, delay2RightLen = 3163;

    int tapL1 = 266, tapL2 = 2974, tapL3 = 1913, tapL4 = 1996;
    int tapL5 = 1990, tapL6 = 187, tapL7 = 1066;
    int tapR1 = 353, tapR2 = 3627, tapR3 = 1228, tapR4 = 2673;
    int tapR5 = 2111, tapR6 = 335, tapR7 = 121;

    float modulationDepth = 16.0f;

    float dampLeft = 0.0f, dampRight = 0.0f;
    float lastLeft2 = 0.0f, lastRight2 = 0.0f;
    float dcXLeft = 0.0f, dcYLeft = 0.0f, dcXRight = 0.0f, dcYRight = 0.0f;
};

} // namespace pedal
