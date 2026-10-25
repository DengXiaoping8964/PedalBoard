/*
  Opens VB-CABLE exactly the way VBCableRouter does and reports what the driver
  actually gave it, plus the cadence of the callbacks over two seconds.

  Written because the running app reported "Active, adds about 43.2 ms" and a
  virtual drop counter climbing at about one per engine block. The latency says
  nothing about whether the device opened at 48000/480 or 44100/512 (both produce
  43.2 ms with the wasapi estimate), and those two cases starve the ring in
  different ways. This probe answers the question with the device's own numbers
  rather than with arithmetic.

  Build:  cmake --build _tests/build --config Release --target cable_probe
  Run:    _tests\build\Release\cable_probe.exe
*/
#include <juce_audio_devices/juce_audio_devices.h>

#include <atomic>
#include <cstdio>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

namespace
{

struct Cadence : juce::AudioIODeviceCallback
{
    void audioDeviceIOCallbackWithContext (const float* const*, int,
                                           float* const* output, int numOut,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext&) override
    {
        for (int c = 0; c < numOut; ++c)
            if (output[c] != nullptr)
                juce::FloatVectorOperations::clear (output[c], numSamples);

        callbacks.fetch_add (1, std::memory_order_relaxed);
        samples.fetch_add (numSamples, std::memory_order_relaxed);
        lastBlock.store (numSamples, std::memory_order_relaxed);
    }

    void audioDeviceAboutToStart (juce::AudioIODevice* device) override
    {
        std::printf ("  aboutToStart: rate=%g buffer=%d name=%s type=%s\n",
                     device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples(),
                     device->getName().toRawUTF8(), device->getTypeName().toRawUTF8());
        started.store (true, std::memory_order_release);
    }

    void audioDeviceStopped() override
    {
        std::printf ("  audioDeviceStopped()\n");
    }

    std::atomic<int> callbacks { 0 }, samples { 0 }, lastBlock { 0 };
    std::atomic<bool> started { false };
};

} // namespace

int main()
{
    SetConsoleOutputCP (65001);

    juce::ScopedJuceInitialiser_GUI initialise;

    juce::AudioDeviceManager manager;

    juce::OwnedArray<juce::AudioIODeviceType> types;
    manager.createAudioDeviceTypes (types);

    juce::String name, typeName;

    for (auto* type : types)
    {
        type->scanForDevices();

        for (auto& candidate : type->getDeviceNames (false))
            if (candidate.containsIgnoreCase ("CABLE Input") && name.isEmpty())
            {
                name = candidate;
                typeName = type->getTypeName();
            }
    }

    if (name.isEmpty())
    {
        std::printf ("CABLE Input not found in any device type\n");
        return 1;
    }

    std::printf ("found \"%s\" in type \"%s\"\n", name.toRawUTF8(), typeName.toRawUTF8());

    /*  A fresh AudioDeviceManager has no device types at all; setCurrentAudioDeviceType
        and setAudioDeviceSetup both silently do nothing on it and report success.
        initialise() is what creates the types, and the preferred-name wildcard is
        what makes it pick the cable rather than the system speakers. */
    const auto initError = manager.initialise (0, 2, nullptr, false, name, nullptr);

    std::printf ("initialise error \"%s\", current type \"%s\", device=%s\n",
                 initError.toRawUTF8(),
                 manager.getCurrentAudioDeviceType().toRawUTF8(),
                 manager.getCurrentAudioDevice() != nullptr ? "YES" : "NO");

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.outputDeviceName = name;
    setup.inputDeviceName  = {};
    setup.sampleRate       = 48000.0;   // what AudioEngine asks for
    setup.bufferSize       = 0;         // the driver default, same as the app
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = true;

    const auto error = manager.setAudioDeviceSetup (setup, true);

    std::printf ("setup error \"%s\", device=%s\n", error.toRawUTF8(),
                 manager.getCurrentAudioDevice() != nullptr ? "YES" : "NO");

    if (error.isNotEmpty())
        return 1;

    if (auto* device = manager.getCurrentAudioDevice())
        std::printf ("opened: \"%s\" %g Hz buffer=%d outCh=%d latencyOut=%d\n",
                     device->getName().toRawUTF8(),
                     device->getCurrentSampleRate(),
                     device->getCurrentBufferSizeSamples(),
                     device->getActiveOutputChannels().countNumberOfSetBits(),
                     device->getOutputLatencyInSamples());
    else
        return 1;

    Cadence cadence;
    manager.addAudioCallback (&cadence);

    juce::Thread::sleep (2000);

    manager.removeAudioCallback (&cadence);
    manager.closeAudioDevice();

    const auto callbacks = cadence.callbacks.load();
    const auto samples = cadence.samples.load();

    std::printf ("2 s: callbacks=%d samples=%d -> %.2f callbacks/s, %.1f samples/s\n",
                 callbacks, samples,
                 callbacks / 2.0, samples / 2.0);

    return 0;
}
