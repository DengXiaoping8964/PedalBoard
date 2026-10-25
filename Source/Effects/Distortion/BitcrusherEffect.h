#pragma once

#include "Common/PedalOversampler.h"
#include "Effects/PedalEffect.h"

#include <array>
#include <cstdint>

/*==============================================================================
  Bitcrusher

  Two independent degradations, exactly as the spec asks:

    Bit Depth      quantises the sample value with a truncate toward zero, which
                   is the classic digital crunch (rounding would keep the signal
                   cleaner than a real crusher). Quantisation noise is broadband,
                   so it benefits from the oversampling: the noise is spread over
                   twice the band and the half band reconstruction filter removes
                   the part above Nyquist that would otherwise fold back through
                   the playback chain.

    Downsample     holds each sample for N blocks of samples. The hold interval is
                   multiplied by the oversampling ratio so the knob means the same
                   thing musically at 2x as it does at 1x. Because a hold is
                   decimation, a one pole tracks the new Nyquist ahead of it: the
                   remaining aliasing above roughly 4x is a characteristic of the
                   effect, further down it would swamp the crunch.

  The hold counter and the quantiser both run at the oversampled rate. The
  quantiser is dithered with one LSB of triangular noise, allocated nowhere.

  Tone is a post low-pass on the wet signal at the device rate, 500 Hz to 18 kHz.
  A crusher's quantisation noise is broadband and deliberately harsh; the tone
  control lets it sit behind the guitar instead of on top of it, and at 100 %
  the corner is high enough that it is effectively out of the way.
==============================================================================*/
namespace pedal
{

class BitcrusherEffect : public PedalEffect
{
public:
    enum Params { bitDepth, downsample, mix, tone, numParams };

    BitcrusherEffect()
        : PedalEffect (bitcrusher, "Bitcrusher",
            {
                { "Bits",      2.0f,  16.0f,  7.0f, "bit" },
                { "Downsample",  1.0f, 100.0f, 6.0f, "x" },
                { "Mix",         0.0f,   1.0f,  1.0f },
                { "Tone",        0.0f, 100.0f, 100.0f, "%" },
            })
    {
    }

    void doPrepare (double, int) override
    {
        oversampler.prepare (chainChannels, blockSize, 1);   // 2x
        resetState();
        prepareLatencyCompensatedDry (getLatencySamples());
    }

    void resetState() noexcept override
    {
        oversampler.reset();
        for (auto& held : heldSample) held = 0.0f;
        for (auto& filter : preFilter) filter.reset();
        for (auto& filter : toneFilter) filter.reset();
        holdCounter = 0;
        cachedToneHz = -1.0f;
    }

    int getLatencySamples() const noexcept override { return oversampler.getLatencySamples(); }

    void process (juce::AudioBuffer<float>& audio) override
    {
        auto& wet = wetBufferForBlock();

        for (int c = 0; c < chainChannels; ++c)
            wet.copyFrom (c, 0, audio, c, 0, audio.getNumSamples());

        const auto bits = juce::jmax (2.0f, getParameter (bitDepth));
        const auto steps = std::exp2 (bits - 1.0f);
        const auto inverseSteps = 1.0f / steps;

        // Hold interval expressed at the oversampled rate.
        const auto holdLength = juce::jmax (1, static_cast<int> (getParameter (downsample))
                                       * oversampler.getFactor());

        // A sample hold is decimation without a filter: everything above the new
        // Nyquist folds back into the band. Track that Nyquist with a one pole
        // ahead of the hold so the grit is the hold itself, not the aliasing.
        const auto oversampleRate = oversampler.oversampledRate (sampleRate);
        const auto preFilterHz = holdLength > 1
            ? oversampleRate * 0.5 / static_cast<double> (holdLength)
            : oversampleRate * 0.45;

        for (auto& filter : preFilter)
            filter.setLowPass (oversampleRate, preFilterHz);

        const auto block = oversampler.up (wet, audio.getNumSamples());
        const auto numSamples = static_cast<int> (block.getNumSamples());
        const auto numChannels = static_cast<int> (block.getNumChannels());

        // The pre filter has to run at its own rate, so it sees every oversampled
        // sample; only the hold latches the result.
        for (int i = 0; i < numSamples; ++i)
            for (int c = 0; c < numChannels; ++c)
                block.getChannelPointer (static_cast<size_t> (c))[i] =
                    preFilter[static_cast<std::size_t> (c)]
                        .process (block.getChannelPointer (static_cast<size_t> (c))[i]);

        for (int i = 0; i < numSamples; ++i)
        {
            if (holdCounter <= 0)
            {
                /*  Dither and quantise here, once per held step, and latch the
                    result. Doing it every sample re-randomised the same held
                    value, so the output was dither noise between latches rather
                    than the flat staircase a sample-and-hold is supposed to make
                    - which is also what the comment always claimed it did. */
                for (int c = 0; c < numChannels; ++c)
                {
                    // Triangular dither, one LSB peak to peak. Without it
                    // truncation gates quiet passages to zero and the crusher
                    // stutters instead of crunching.
                    const auto dither = (nextNoise() + nextNoise()) * 0.5f * inverseSteps;
                    const auto sampled = block.getChannelPointer (static_cast<size_t> (c))[i];

                    // Truncate toward zero rather than round: keeps the gritty,
                    // slightly noisy behaviour of a low resolution converter.
                    heldSample[static_cast<std::size_t> (c)] =
                        std::trunc ((sampled + dither) * steps) * inverseSteps;
                }

                holdCounter = holdLength;
            }

            --holdCounter;

            for (int c = 0; c < numChannels; ++c)
                block.getChannelPointer (static_cast<size_t> (c))[i]
                    = heldSample[static_cast<std::size_t> (c)];
        }

        oversampler.down (wet, audio.getNumSamples());

        // Post low-pass at the device rate: 500 Hz (dark, almost a bit reduce
        // with no fizz) to 18 kHz (out of the way).
        const auto toneCutoff = 500.0 * std::pow (36.0, clamp01 (getParameter (tone) * 0.01f));
        if (static_cast<float> (toneCutoff) != cachedToneHz)
        {
            for (auto& filter : toneFilter)
                filter.setLowPass (sampleRate, toneCutoff);
            cachedToneHz = static_cast<float> (toneCutoff);
        }

        for (int c = 0; c < chainChannels; ++c)
        {
            auto* data = wet.getWritePointer (c);
            for (int i = 0; i < audio.getNumSamples(); ++i)
                data[i] = toneFilter[static_cast<std::size_t> (c)].process (data[i]);
        }

        mixWithDelayedDry (audio, wet, getParameter (mix));
    }

private:
    /** xorshift32: allocation free, deterministic, plenty for dither. */
    float nextNoise() noexcept
    {
        noiseState ^= noiseState << 13;
        noiseState ^= noiseState >> 17;
        noiseState ^= noiseState << 5;
        return static_cast<float> (noiseState) * (2.0f / 4294967295.0f) - 1.0f;
    }

    Oversampler oversampler;
    std::array<float, chainChannels> heldSample {};
    std::array<OnePole, chainChannels> preFilter;
    std::array<OnePole, chainChannels> toneFilter;
    int holdCounter = 0;
    float cachedToneHz = -1.0f;
    std::uint32_t noiseState = 0x9e3779b9u;
};

} // namespace pedal
