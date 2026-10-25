/*
  End-to-end check of the virtual recording path, using the real production code:
  pedal::AudioRingBuffer + pedal::VBCableRouter, not a copy of their logic.

  A 440 Hz sine is written into the ring at the engine's block rate while the
  router opens CABLE Input. A second AudioDeviceManager records CABLE Output, the
  endpoint a DAW would record from, and the probe measures what arrived. A stream
  that "opens" but carries nothing - which is what the app showed before the
  manager was initialised - fails the RMS and frequency checks.

  Build:  cmake --build _tests/build --config Release --target virtual_path_probe
  Run:    _tests\build\Release\virtual_path_probe.exe
  Close PedalBoard first: the render endpoint can only be opened by one client at
  a time in shared mode, and this probe has to be that client.
*/
#include "Common/PedalConfig.h"
#include "Common/PedalLockFree.h"
#include "VirtualDevice/VBCableRouter.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

namespace
{

constexpr double sampleRate = 48000.0;
constexpr int blockSize = 480;
constexpr double toneHz = 440.0;
constexpr float amplitude = 0.25f;

/** Records channel 0 of a capture device into a buffer allocated up front: the
    callback must not touch the heap, even in a probe. */
struct Recorder : juce::AudioIODeviceCallback
{
    explicit Recorder (int numSamples) : storage (static_cast<std::size_t> (numSamples), 0.0f) {}

    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData, int numInputChannels,
                                           float* const*, int, int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        auto at = written.load (std::memory_order_relaxed);

        if (numInputChannels <= 0 || inputChannelData[0] == nullptr
            || at + numSamples > static_cast<int> (storage.size()))
            return;

        std::memcpy (storage.data() + at, inputChannelData[0],
                     static_cast<std::size_t> (numSamples) * sizeof (float));

        written.store (at + numSamples, std::memory_order_release);
    }

    void audioDeviceStopped() override {}

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        std::printf ("capture aboutToStart: %g Hz buffer=%d name=%s\n",
                     device->getCurrentSampleRate(),
                     device->getCurrentBufferSizeSamples(),
                     device->getName().toRawUTF8());
    }

    std::vector<float> storage;
    std::atomic<int> written { 0 };
};

} // namespace

int main()
{
    SetConsoleOutputCP (65001);

    juce::ScopedJuceInitialiser_GUI initialise;

    std::atomic<double> engineRate { sampleRate };
    std::atomic<int> engineBlock { blockSize };

    pedal::AudioRingBuffer ring;
    ring.reset (pedal::chainChannels, static_cast<int> (pedal::ringCapacityFor (pedal::virtualRingBlocks)));

    pedal::VBCableRouter router (ring, engineRate, engineBlock);

    std::printf ("scan: %s\n", router.scanForCable() ? "found" : "NOT FOUND");

    const auto startError = router.start();
    const auto status = router.getStatus();

    std::printf ("start: error=\"%s\" open=%d availability=%d device=\"%s\" type=\"%s\"\n",
                 startError.toRawUTF8(), status.open ? 1 : 0, static_cast<int> (status.availability),
                 status.deviceName.toRawUTF8(), status.deviceType.toRawUTF8());
    std::printf ("status line: %s\n", router.getStatusLine().toRawUTF8());

    if (! status.open)
    {
        std::printf ("FAIL: the router did not open a stream\n");
        return 1;
    }

    /*  The capture side. The wildcard name is what makes initialise() pick the
        cable instead of the system microphone. */
    juce::AudioDeviceManager captureManager;
    auto captureError = captureManager.initialise (2, 0, nullptr, false, "CABLE Output*", nullptr);

    if (captureError.isEmpty())
    {
        juce::AudioDeviceManager::AudioDeviceSetup setup;
        setup.inputDeviceName = captureManager.getAudioDeviceSetup().inputDeviceName;
        setup.sampleRate = sampleRate;
        setup.bufferSize = 0;
        setup.useDefaultInputChannels = true;
        captureError = captureManager.setAudioDeviceSetup (setup, true);
    }

    if (captureError.isNotEmpty() || captureManager.getCurrentAudioDevice() == nullptr)
    {
        std::printf ("FAIL: could not open CABLE Output: \"%s\"\n", captureError.toRawUTF8());
        router.setEnabled (false);
        return 1;
    }

    Recorder recorder (static_cast<int> (sampleRate * 3.0));
    captureManager.addAudioCallback (&recorder);

    // Producer: exactly the engine's contract, one 480-sample block every 10 ms.
    float left[blockSize] {}, right[blockSize] {};
    float* channels[2] = { left, right };

    double phase = 0.0;
    const auto phaseStep = 2.0 * juce::MathConstants<double>::pi * toneHz / sampleRate;
    const auto startTime = juce::Time::getMillisecondCounterHiRes();
    const auto blockMilliseconds = 1000.0 * static_cast<double> (blockSize) / sampleRate;

    auto nextBlock = startTime;

    while (juce::Time::getMillisecondCounterHiRes() - startTime < 2200.0)
    {
        for (int i = 0; i < blockSize; ++i)
        {
            const auto value = amplitude * static_cast<float> (std::sin (phase));
            left[i] = right[i] = value;
            phase += phaseStep;

            if (phase > 2.0 * juce::MathConstants<double>::pi)
                phase -= 2.0 * juce::MathConstants<double>::pi;
        }

        ring.write (channels, pedal::chainChannels, blockSize);

        /*  Absolute deadlines with a short spin at the end. A bare Sleep(10) is
            rounded by the system timer and the producer then runs slower than the
            device, which is a probe bug that looks like a transport problem in the
            drop counter. */
        nextBlock += blockMilliseconds;

        for (;;)
        {
            const auto remaining = nextBlock - juce::Time::getMillisecondCounterHiRes();

            if (remaining <= 0.0)
                break;

            if (remaining > 1.5)
                juce::Thread::sleep (static_cast<int> (remaining - 1.0));
            else
                juce::Thread::yield();
        }
    }

    captureManager.removeAudioCallback (&recorder);
    captureManager.closeAudioDevice();
    router.setEnabled (false);

    const auto captured = recorder.written.load (std::memory_order_acquire);

    // Analyse the middle second, past the devices' start-up and before the tail.
    const auto from = static_cast<int> (sampleRate * 0.4);
    const auto to = juce::jmin (captured, static_cast<int> (sampleRate * 1.8));

    double sumSquares = 0.0;
    float peak = 0.0f;

    for (int i = from; i < to; ++i)
    {
        const auto value = recorder.storage[static_cast<std::size_t> (i)];
        sumSquares += static_cast<double> (value) * value;
        peak = juce::jmax (peak, std::abs (value));
    }

    const auto analysed = juce::jmax (1, to - from);
    const auto rms = std::sqrt (sumSquares / analysed);

    /*  Goertzel at the expected frequency, not zero crossings: a few dropped blocks
        show up as zero-filled gaps, and each gap adds or removes a crossing while
        leaving the tone's energy intact. The amplitude estimate is 2|X|/N, which is
        exactly A for a pure sine of amplitude A. */
    const auto omega = 2.0 * juce::MathConstants<double>::pi * toneHz / sampleRate;
    const auto coefficient = 2.0 * std::cos (omega);

    double s1 = 0.0, s2 = 0.0;

    for (int i = from; i < to; ++i)
    {
        const auto s0 = recorder.storage[static_cast<std::size_t> (i)] + coefficient * s1 - s2;
        s2 = s1;
        s1 = s0;
    }

    const auto power = s1 * s1 + s2 * s2 - coefficient * s1 * s2;
    const auto toneAmplitude = 2.0 * std::sqrt (juce::jmax (0.0, power)) / analysed;

    std::printf ("captured=%d samples (%.2f s), rms=%.4f peak=%.4f tone amplitude at %.0f Hz=%.4f\n",
                 captured, captured / sampleRate, rms, peak, toneHz, toneAmplitude);
    std::printf ("virtual drops: %d\n", router.getDropCount());

    const auto pass = captured > static_cast<int> (sampleRate)
                   && rms > 0.1
                   && toneAmplitude > amplitude * 0.6
                   && toneAmplitude < amplitude * 1.4;

    std::printf ("%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
