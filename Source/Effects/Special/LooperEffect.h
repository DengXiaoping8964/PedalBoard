#pragma once

#include <algorithm>

#include "Effects/PedalEffect.h"

#include <vector>
#include <array>
#include <atomic>

/*==============================================================================
  Looper

  A fixed 30 second (at the current sample rate) two channel tape, allocated once
  in doPrepare() and never resized again. The whole transport runs on the audio
  thread; the message thread only ever deposits a command in one atomic and reads
  status from other atomics. That is the entire synchronisation scheme, so there
  is no lock, no allocation and no file access in process().

  Overdub reads the tap before it writes the same position, which is what makes
  stacking passes additive rather than a feedback howl.

  Memory note: the tape is sized for the *device* sample rate, so 30 s stereo
  float costs about 11.5 MB at 44.1 kHz and about 46 MB at 192 kHz, times two
  because Undo keeps the previous take.
==============================================================================*/
namespace pedal
{

class LooperEffect : public PedalEffect
{
public:
    enum Params { level, tone, mix, numParams };

    enum Commands { cmdNone = 0, cmdRecord, cmdTogglePlay, cmdOverdub, cmdClear, cmdUndo };

    // The card buttons map one to one onto the audio-thread commands, so the
    // identifiers are declared from the same list rather than duplicated.
    enum Actions { record = cmdRecord, togglePlay = cmdTogglePlay, overdub = cmdOverdub,
                   clearTape = cmdClear, undo = cmdUndo };

    enum class State : int { stopped = 0, recording = 1, playing = 2, overdubbing = 3 };

    LooperEffect()
        : PedalEffect (looper, "Looper",
            {
                { "Level", -24.0f, 12.0f,  0.0f, "dB" },
                { "Tone",    800.0f, 20000.0f, 18000.0f, "Hz" },
                { "Mix",      0.0f,  1.0f,  1.0f },
            })
    {
    }

    //==============================================================================
    void doPrepare (double, int) override
    {
        const auto capacity = std::max (1, static_cast<int> (maxLoopSeconds * sampleRate));

        loop.setSize (chainChannels, capacity, false, true, true);
        undoBuffer.setSize (chainChannels, capacity, false, true, true);

        loop.clear();
        undoBuffer.clear();

        capacitySamples = capacity;
        rampSamples = std::max (1, static_cast<int> (0.008 * sampleRate));

        resetState();
    }

    void resetState() noexcept override
    {
        playhead = 0;
        writeHead = 0;
        loopLength = 0;
        gain = 0.0f;
        state = State::stopped;
        undoValid = false;
        stateOut.store (static_cast<int> (state), std::memory_order_relaxed);
        loopSecondsOut.store (0.0f, std::memory_order_relaxed);
    }

    //==============================================================================
    void process (juce::AudioBuffer<float>& audio) override
    {
        if (loop.getNumSamples() == 0)
            return;

        applyPendingCommand();

        auto& wet = wetBufferForBlock();
        wet.clear();

        const auto numSamples  = audio.getNumSamples();
        const auto numChannels = juce::jmin (audio.getNumChannels(), chainChannels);
        const auto playing = state == State::playing || state == State::overdubbing;

        const auto levelGain = dbToGain (getParameter (level));
        updateTone();

        for (int i = 0; i < numSamples; ++i)
        {
            // 1. Read the tape. The gain ramp is applied per sample so that a
            //    transport change in the middle of a block stays click free.
            if (playing && loopLength > 0)
            {
                const auto tap = playhead;
                const auto tapeGain = levelGain * gain;

                for (int c = 0; c < numChannels; ++c)
                    wet.getWritePointer (c)[i] = toneFilter[c].process (loop.getReadPointer (c)[tap]) * tapeGain;

                if (++playhead >= loopLength)
                    playhead = 0;
            }

            // 2. Write the incoming signal. At the same index the read above has
            //    already happened, so an overdub pass cannot feed itself.
            if (state == State::recording || state == State::overdubbing)
            {
                const auto tap = writeHead;

                for (int c = 0; c < numChannels; ++c)
                    loop.getWritePointer (c)[tap] = audio.getReadPointer (c)[i];

                if (writeHead + 1 > loopLength)
                    loopLength = writeHead + 1;

                if (++writeHead >= capacitySamples)
                {
                    // The tape is full: close the loop at its end and play.
                    writeHead = 0;
                    playhead  = 0;
                    state = State::playing;
                    publishState();
                }
            }

            advanceGainRamp();
        }

        // Mix blends the tape over whatever the rest of the chain produced. While
        // the first pass is only being captured there is nothing to blend, so the
        // dry signal stays whole instead of being attenuated by an empty loop.
        const auto blend = playing ? clamp01 (getParameter (mix)) : 0.0f;
        const auto dryAmount = 1.0f - blend;

        for (int c = 0; c < numChannels; ++c)
        {
            auto* dry  = audio.getWritePointer (c);
            const auto* src = wet.getReadPointer (c);

            for (int i = 0; i < numSamples; ++i)
                dry[i] = dry[i] * dryAmount + src[i] * blend;
        }
    }

    //==============================================================================
    std::vector<EffectAction> actions() const override
    {
        return { { record,      "Rec" },
                 { togglePlay,  "Play / Stop" },
                 { overdub,     "Overdub" },
                 { clearTape,   "Clear" },
                 { undo,        "Undo" } };
    }

    void handleAction (int id) override
    {
        command.store (id, std::memory_order_release);
    }

    juce::String getStatusText() const override
    {
        const auto seconds = loopSecondsOut.load (std::memory_order_relaxed);
        const auto label   = [&]
        {
            switch (static_cast<State> (stateOut.load (std::memory_order_relaxed)))
            {
                case State::recording:    return tr ("Recording");
                case State::playing:      return tr ("Playing");
                case State::overdubbing:  return tr ("Overdubbing");
                default:                  return tr ("Stopped");
            }
        }();

        // One template rather than a state word glued to a sentence: the order of
        // "Recording - loop 3.2 s / 30 s" is not the order Chinese uses.
        return trf ("%1  -  loop %2 s / %3 s", label,
                    juce::String (seconds, 1), juce::String (maxLoopSeconds, 0));
    }

    juce::String getWarningText() const override
    {
        if (stateOut.load (std::memory_order_relaxed) == static_cast<int> (State::stopped)
            && loopSecondsOut.load (std::memory_order_relaxed) <= 0.0f)
            return tr ("Rec starts on the next audio block; the tape is lost when the device changes.");

        return {};
    }

private:
    //==============================================================================
    void applyPendingCommand()
    {
        const auto cmd = command.exchange (cmdNone, std::memory_order_acq_rel);
        if (cmd == cmdNone)
            return;

        switch (cmd)
        {
            case cmdRecord:
                if (state != State::recording)
                {
                    captureForUndo();

                    /*  No loop.clear() here. Every read is bounded by loopLength,
                        which this resets to 0 and which only grows as new samples
                        are written, so the old take beyond it can never be heard.
                        Clearing 11.5 MB on the audio thread for samples nobody can
                        reach was the largest single block-time spike in the app. */
                    writeHead = playhead = 0;
                    loopLength = 0;
                    state = State::recording;
                }
                else
                {
                    finishRecording();
                }
                break;

            case cmdOverdub:
                if (state == State::overdubbing)
                {
                    finishRecording();
                }
                else if (loopLength > 0)
                {
                    captureForUndo();
                    writeHead = playhead;      // punch in where the listener is
                    state = State::overdubbing;
                }
                else
                {
                    state = State::recording;
                }
                break;

            case cmdTogglePlay:
                if (state == State::stopped)
                {
                    if (loopLength > 0)
                    {
                        playhead = 0;
                        state = State::playing;
                    }
                }
                else if (state == State::playing)
                {
                    state = State::stopped;
                }
                else
                {
                    finishRecording();         // stop punching in, keep playing
                    state = State::playing;
                }
                break;

            case cmdClear:
                captureForUndo();
                loopLength = 0;               // bounds every read; the bytes stay put
                writeHead = playhead = 0;
                state = State::stopped;
                break;

            case cmdUndo:
                if (undoValid && state != State::recording && state != State::overdubbing)
                    swapTapes();
                break;

            default:
                break;
        }

        publishState();
    }

    void finishRecording()
    {
        writeHead = 0;
        playhead  = 0;
        state = State::playing;
    }

    /** Undo keeps one previous take, which is why the tape is allocated twice.

        Only loopLength samples are copied, not capacitySamples: playback is bounded
        by loopLength, so copying the untouched remainder of a full 30 s tape would
        be 11.5 MB of memory traffic on the audio thread that no undo can reveal. */
    void captureForUndo()
    {
        if (state == State::stopped || loopLength <= 0)
        {
            undoValid = false;
            return;
        }

        for (int c = 0; c < chainChannels; ++c)
            undoBuffer.copyFrom (c, 0, loop, c, 0, loopLength);

        undoLength = loopLength;
        undoValid = true;
    }

    void swapTapes()
    {
        // The longer of the two takes is what has to move; each tape's reads are
        // bounded by its own length, so samples past that are irrelevant.
        const auto n = static_cast<std::size_t> (juce::jmax (loopLength, undoLength));

        for (int c = 0; c < chainChannels; ++c)
        {
            auto* a = loop.getWritePointer (c);
            auto* b = undoBuffer.getWritePointer (c);

            std::swap_ranges (a, a + n, b);
        }

        std::swap (loopLength, undoLength);
        playhead = 0;
        writeHead = 0;
        state = loopLength > 0 ? State::playing : State::stopped;
        undoValid = false;
    }

    void publishState()
    {
        stateOut.store (static_cast<int> (state), std::memory_order_relaxed);
        loopSecondsOut.store (loopLength > 0 ? static_cast<float> (loopLength)
                                             / static_cast<float> (sampleRate) : 0.0f,
                              std::memory_order_relaxed);
    }

    void updateTone() noexcept
    {
        const auto hz = getParameter (tone);

        if (hz != lastToneHz || sampleRate != lastToneRate)
        {
            for (auto& filter : toneFilter)
                filter.setLowPass (sampleRate, static_cast<double> (hz));

            lastToneHz = hz;
            lastToneRate = static_cast<float> (sampleRate);
        }
    }

    /** Playback and overdub fade in over 8 ms, so starting or punching never clicks. */
    void advanceGainRamp() noexcept
    {
        const auto target = state == State::stopped ? 0.0f : 1.0f;

        if (gain == target)
            return;

        const auto step = 1.0f / static_cast<float> (rampSamples);
        gain = target > gain ? std::min (target, gain + step)
                             : std::max (target, gain - step);
    }

    //==============================================================================
    juce::AudioBuffer<float> loop, undoBuffer;
    std::array<OnePole, chainChannels> toneFilter;

    std::atomic<int>   command { cmdNone };
    std::atomic<int>   stateOut { static_cast<int> (State::stopped) };
    std::atomic<float> loopSecondsOut { 0.0f };

    int capacitySamples = 0;
    int loopLength = 0, undoLength = 0;
    int playhead = 0, writeHead = 0;
    int rampSamples = 384;
    float gain = 0.0f;
    float lastToneHz = -1.0f, lastToneRate = -1.0;
    bool undoValid = false;

    State state { State::stopped };   // audio-thread copy of stateOut
};

} // namespace pedal
