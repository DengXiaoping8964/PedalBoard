#pragma once

#include "Effects/PedalEffect.h"

#include <algorithm>
#include <cmath>
#include <atomic>

/*==============================================================================
  Wah (Pedal)

  Algorithm: a topology preserving transform state variable filter (pedal::TptSvf)
  swept exponentially across the Min/Max range, driven by the Position knob or by
  a MIDI expression controller.

  Why TPT rather than the RBJ cookbook section the earlier build used: a wah
  changes its corner every sample while it sweeps, and direct-form biquads with
  moving poles can ring or jump. The TPT structure (Zavalishin, "The Art of VA
  Filter Design"; Cytomic's SVF) integrates the analogue topology, so its state
  stays valid while cutoff and Q move - which is also why it needs no tap-glide
  trick to stay click free. Both cutoff and Q are smoothed per sample, so a MIDI
  pedal jump cannot step the corner.

  Frequency maps exponentially across the range, matching a voice box wah:
      freq = minFreq * pow (maxFreq / minFreq, position)
  The band output of the SVF has a peak gain of Q, so it is scaled by 1/sqrt(Q):
  the net level rises with sqrt(Q), which keeps a sharp vowel audible without
  turning the sweep into a volume pedal.

  Mode selects the filter response: Band Pass is the classic CryBaby vowel;
  Low Pass is the darker, wider Morley-style sweep that keeps the low strings
  under the sweep instead of thinning them. The two are crossfaded over a few
  milliseconds when the switch moves, so changing mode never clicks.

  The swept position comes either from the Position knob or, when Source is set
  to MIDI CC, from the expression controller value the engine keeps in the
  shared EffectContext atomic. That atomic is written by the MIDI callback, so
  it is read with a relaxed load and never from the message thread. The selector
  only takes effect while a MIDI input is open (the context carries that flag
  too); otherwise the pedal falls back to the knob and the card says so, rather
  than freezing because nobody is sending CC 11.
==============================================================================*/
namespace pedal
{

class WahEffect : public PedalEffect
{
public:
    enum Params { position, minFreq, maxFreq, q, source, mix, mode, numParams };

    /** Enum like knob: the description carries the step count and the label
        table so the card renders a selector instead of a continuous knob. */
    static const char* const* sourceLabels() noexcept
    {
        static const char* const names[] = { "Slider", "MIDI CC" };
        return names;
    }

    static const char* const* modeLabels() noexcept
    {
        static const char* const names[] = { "Band Pass", "Low Pass" };
        return names;
    }

    explicit WahEffect (const EffectContext& context)
        : PedalEffect (wah, "Wah Pedal",
            {
                { "Position",    0.0f,  100.0f,  50.0f, "%" },
                { "Min Freq",  200.0f, 1500.0f, 350.0f, "Hz" },
                { "Max Freq",  800.0f, 8000.0f, 3200.0f, "Hz" },
                { "Q",           0.5f,   12.0f,   3.5f },
                { "Source",     0.0f,    1.0f,    0.0f, "", 2, WahEffect::sourceLabels() },
                { "Mix",        0.0f,    1.0f,    1.0f },
                { "Mode",       0.0f,    1.0f,    0.0f, "", 2, WahEffect::modeLabels() },
            }),
          expression (context.expressionPosition),
          midiActive (context.expressionMidiActive)
    {
    }

    void doPrepare (double, int) override
    {
        glideCoeff = timeCoefficient (sampleRate, 4.0f);
        modeGlide  = timeCoefficient (sampleRate, 6.0f);

        const auto target = targetCentre();
        currentHz  = target;
        currentQ   = std::max (0.5f, getParameter (Params::q));
        logCurrent = std::log (currentHz);
        modeMix    = getParameter (mode) >= 0.5f ? 1.0f : 0.0f;

        for (auto& section : band)
            section.reset();

        centreHz.store (target, std::memory_order_relaxed);
        reportCounter = reportInterval;
    }

    void resetState() noexcept override
    {
        for (auto& section : band)
            section.reset();

        const auto target = targetCentre();
        currentHz  = target;
        currentQ   = std::max (0.5f, getParameter (Params::q));
        logCurrent = std::log (currentHz);
        modeMix    = getParameter (mode) >= 0.5f ? 1.0f : 0.0f;
    }

    juce::String getStatusText() const override
    {
        const auto live = centreHz.load (std::memory_order_relaxed);
        return juce::String (live, 0) + " Hz";
    }

    void process (juce::AudioBuffer<float>& audio) override
    {
        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);
        auto& wet = wetBufferForBlock();

        const auto target     = targetCentre();
        const auto logTarget  = std::log (target);
        const auto targetQ    = std::max (0.5f, getParameter (Params::q));
        const auto targetMode = getParameter (mode) >= 0.5f ? 1.0f : 0.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            // Smooth in the log domain so an octave sweep moves at a steady
            // musical rate, then smooth Q as well: a jump in resonance is a
            // click even when the corner itself is glided.
            logCurrent += (logTarget - logCurrent) * glideCoeff;
            currentHz   = std::exp (logCurrent);
            currentQ   += (targetQ - currentQ) * glideCoeff;
            modeMix    += (targetMode - modeMix) * modeGlide;

            const auto makeup = 1.0f / std::sqrt (std::max (0.5f, currentQ));

            for (int c = 0; c < numChannels; ++c)
            {
                auto& section = band[c];
                section.setCutoffQ (sampleRate, currentHz, currentQ);
                const auto out = section.process (audio.getReadPointer (c)[i]);

                // Band pass is the vowel; low pass keeps the low strings in.
                // 0.5 sits the low-pass sweep at a comparable loudness, because
                // its whole pass band passes where the band-pass only passes Q.
                wet.getWritePointer (c)[i] = lerp (out.band * makeup,
                                                   out.low * 0.5f,
                                                   modeMix);
            }

            if (--reportCounter <= 0)
            {
                reportCounter = reportInterval;
                centreHz.store (std::exp (logCurrent), std::memory_order_relaxed);
            }
        }

        crossFade (audio, wet, getParameter (mix));
    }

    juce::String getWarningText() const override
    {
        if (getParameter (source) < 0.5f)
            return {};

        return midiDrives() ? tr ("Wah sweep follows MIDI CC 11")
                            : tr ("Source is MIDI CC but no MIDI input is open - "
                                  "the Position knob drives the sweep");
    }

private:
    /** True when the selector says MIDI and a MIDI input is actually open. The
        expression atomic always exists (the engine owns it), so the selector alone
        cannot answer this; without the flag the pedal would sit frozen at 50 %
        whenever no controller was connected. */
    bool midiDrives() const noexcept
    {
        return getParameter (source) >= 0.5f
            && expression != nullptr
            && midiActive != nullptr
            && midiActive->load (std::memory_order_relaxed);
    }

    /** 0..1 pedal travel from the knob or the incoming expression CC.
        The CC value is 0..1, i.e. the same travel the Position knob shows as
        0..100 %, so it is used unrescaled. */
    float sweptPosition() const noexcept
    {
        if (midiDrives())
            return clamp01 (expression->load (std::memory_order_relaxed));

        return clamp01 (getParameter (position) * 0.01f);
    }

    float targetCentre() const noexcept
    {
        /*  The two knobs are independent, so either can be turned past the other.
            Taking them in order makes that a wide sweep in the other direction
            instead of collapsing the range to 1 Hz and turning the pedal into a
            fixed band-pass with no feedback in the UI. */
        const auto lowKnob  = getParameter (minFreq);
        const auto highKnob = getParameter (maxFreq);
        const auto minimum  = std::max (20.0f, std::min (lowKnob, highKnob));
        const auto maximum  = std::max (minimum + 1.0f, std::max (lowKnob, highKnob));

        return minimum * std::pow (maximum / minimum, sweptPosition());
    }

    static constexpr int reportInterval = 16;

    std::atomic<float>* expression = nullptr;
    std::atomic<bool>*  midiActive = nullptr;
    std::atomic<float> centreHz { 350.0f };

    TptSvf band[chainChannels];
    float glideCoeff = 0.004f;
    float modeGlide = 0.004f, modeMix = 0.0f;
    float currentHz = 350.0f, currentQ = 3.5f, logCurrent = 0.0f;
    int reportCounter = reportInterval;
};

} // namespace pedal
