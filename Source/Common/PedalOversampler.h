#pragma once

#include <juce_dsp/juce_dsp.h>

#include "Common/PedalConfig.h"

#include <memory>

/*==============================================================================
  PedalBoard - oversampling wrapper.

  juce::dsp::Oversampling in JUCE 8 works on AudioBlock, and its constructor
  takes a number of half band STAGES, not a factor: 2 stages is 4 times
  oversampling. It also allocates in the constructor and in initProcessing(), so
  the only place it may be built is prepare time. That is why the oversampling
  ratio of the distortion pedals is fixed when the device starts instead of being
  a live knob: a live knob would have to reallocate the filter bank on the audio
  thread.

  Integer latency compensation is switched on so getLatencyInSamples() is a
  whole number of samples, which the engine can subtract from the dry path
  without fractional delay interpolation.
==============================================================================*/
namespace pedal
{

class Oversampler
{
public:
    /** @param numStages 1 = 2x, 2 = 4x, 3 = 8x. Allocates; call from prepare only. */
    void prepare (int numChannels, int blockSizeBeforeOversampling, int numStages)
    {
        stages = numStages;
        samplesPerBlock = blockSizeBeforeOversampling;
        factor = 1 << stages;

        unit = std::make_unique<juce::dsp::Oversampling<float>> (
            static_cast<size_t> (numChannels),
            static_cast<size_t> (numStages),
            juce::dsp::Oversampling<float>::FilterType::filterHalfBandPolyphaseIIR,
            true,   // maximum quality filters
            true);  // integer latency

        unit->initProcessing (static_cast<size_t> (blockSizeBeforeOversampling));
        reset();
    }

    bool isReady() const noexcept { return unit != nullptr; }

    int getFactor() const noexcept            { return factor; }
    int getStages() const noexcept            { return stages; }
    double oversampledRate (double baseRate) const noexcept { return baseRate * factor; }
    int oversampledSamples (int baseSamples) const noexcept { return baseSamples * factor; }

    int getLatencySamples() const noexcept
    {
        return unit != nullptr ? static_cast<int> (unit->getLatencyInSamples() + 0.5f) : 0;
    }

    /** Up samples and hands back the oversampled block. The returned block is
        only valid until the matching down sample call.

        numSamples is the caller's block length, and the view handed to JUCE is
        trimmed to it. Without that trim the block covers the whole scratch buffer
        - which is sized for maxBlockSize, not for this block - so everything past
        the current block is stale, and because down() writes it back into the same
        buffer it becomes the input of the next block: a feedback path through the
        half-band IIR filters that is invisible at sane settings and enormous at
        ones that put energy near Nyquist (the bitcrusher's sample and hold reached
        5e12 on the bench this way). */
    juce::dsp::AudioBlock<float> up (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        juce::dsp::AudioBlock<float> view { buffer.getArrayOfWritePointers(),
                                                  static_cast<size_t> (buffer.getNumChannels()),
                                                  0, static_cast<size_t> (numSamples) };

        return unit->processSamplesUp (view);
    }

    /** Filters the oversampled data back down into the first numSamples of buffer. */
    void down (juce::AudioBuffer<float>& buffer, int numSamples) noexcept
    {
        juce::dsp::AudioBlock<float> view { buffer.getArrayOfWritePointers(),
                                                  static_cast<size_t> (buffer.getNumChannels()),
                                                  0, static_cast<size_t> (numSamples) };

        unit->processSamplesDown (view);
    }

    void reset() noexcept
    {
        if (unit != nullptr)
            unit->reset();
    }

private:
    std::unique_ptr<juce::dsp::Oversampling<float>> unit;
    int stages = 1, factor = 2, samplesPerBlock = 0;
};

} // namespace pedal
