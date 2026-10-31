/*
  What does a single duplex AudioDeviceManager actually get on this machine?

  Direct mode depends on an interface exposing both a capture and a render endpoint
  under the same parenthesised name ("Microphone (Momix-Cab)" and "Speakers
  (Momix-Cab)"). This probe finds the first such pair, opens it in each Windows Audio
  mode, then walks the buffer sizes the driver really offers and prints the
  driver-reported input/output latency and xrun count for each.

  It is the raw-API counterpart to the app's LATENCY box: if the app shows 12 ms and
  this says otherwise, the difference is in the engine, not in the story.

  Build:  cmake --build _tests/build --config Release --target duplex_probe
  Run:    _tests\build\Release\duplex_probe.exe
*/
#include <juce_audio_devices/juce_audio_devices.h>

#include <cstdio>

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

namespace
{

juce::String familyName (const juce::String& deviceName)
{
    const auto open = deviceName.lastIndexOfChar ('(');

    if (open >= 0 && deviceName.endsWith (")"))
        return deviceName.substring (open + 1, deviceName.length() - 1).trim();

    return deviceName.trim();
}

struct Pair
{
    juce::String input, output;
};

Pair findPair (juce::AudioDeviceManager& manager, const juce::String& typeName)
{
    manager.setCurrentAudioDeviceType (typeName, true);

    Pair pair;

    if (auto* type = manager.getCurrentDeviceTypeObject())
    {
        type->scanForDevices();

        for (auto& in : type->getDeviceNames (true))
        {
            for (auto& out : type->getDeviceNames (false))
            {
                if (in == out || familyName (in).equalsIgnoreCase (familyName (out)))
                {
                    pair.input = in;
                    pair.output = out;
                    return pair;
                }
            }
        }
    }

    return pair;
}

} // namespace

int main()
{
    SetConsoleOutputCP (65001);

    juce::ScopedJuceInitialiser_GUI initialise;

    const juce::StringArray types { "Windows Audio (Exclusive Mode)",
                                    "Windows Audio (Low Latency Mode)",
                                    "Windows Audio" };

    for (auto& typeName : types)
    {
        std::printf ("\n=== %s ===\n", typeName.toRawUTF8());

        juce::AudioDeviceManager manager;

        if (auto error = manager.initialise (1, 2, nullptr, false); error.isNotEmpty())
        {
            std::printf ("initialise: %s\n", error.toRawUTF8());
            continue;
        }

        const auto pair = findPair (manager, typeName);

        if (pair.input.isEmpty() || pair.output.isEmpty())
        {
            std::printf ("no input/output pair of the same interface in this mode\n");
            continue;
        }

        std::printf ("using in=\"%s\" out=\"%s\"\n",
                     pair.input.toRawUTF8(), pair.output.toRawUTF8());

        auto setup = manager.getAudioDeviceSetup();
        setup.inputDeviceName  = pair.input;
        setup.outputDeviceName = pair.output;
        setup.useDefaultInputChannels = true;
        setup.useDefaultOutputChannels = true;
        setup.sampleRate = 48000.0;
        setup.bufferSize = 0;

        if (auto error = manager.setAudioDeviceSetup (setup, true); error.isNotEmpty())
        {
            std::printf ("open duplex: %s\n", error.toRawUTF8());
            continue;
        }

        auto* device = manager.getCurrentAudioDevice();

        if (device == nullptr)
        {
            std::printf ("no device object\n");
            continue;
        }

        const auto sizes = device->getAvailableBufferSizes();
        std::printf ("opened: %s | %g Hz | %d samples | in %d / out %d samples | sizes: ",
                     device->getName().toRawUTF8(), device->getCurrentSampleRate(),
                     device->getCurrentBufferSizeSamples(),
                     device->getInputLatencyInSamples(),
                     device->getOutputLatencyInSamples());

        for (int i = 0; i < sizes.size(); ++i)
            std::printf ("%s%d", i > 0 ? ", " : "", sizes[i]);

        std::printf ("\n");

        for (auto size : sizes)
        {
            auto s = manager.getAudioDeviceSetup();
            s.bufferSize = size;

            if (auto error = manager.setAudioDeviceSetup (s, true); error.isNotEmpty())
            {
                std::printf ("  buffer %5d: refused: %s\n", size, error.toRawUTF8());
                continue;
            }

            juce::Thread::sleep (300);

            if (auto* d = manager.getCurrentAudioDevice())
                std::printf ("  buffer %5d: got %5d (%6.2f ms) in %5d out %5d  total %7.2f ms  xruns %d\n",
                             size, d->getCurrentBufferSizeSamples(),
                             1000.0 * d->getCurrentBufferSizeSamples() / d->getCurrentSampleRate(),
                             d->getInputLatencyInSamples(), d->getOutputLatencyInSamples(),
                             1000.0 * (d->getInputLatencyInSamples() + d->getOutputLatencyInSamples())
                                 / d->getCurrentSampleRate(),
                             d->getXRunCount());
        }

        manager.closeAudioDevice();
    }

    return 0;
}
