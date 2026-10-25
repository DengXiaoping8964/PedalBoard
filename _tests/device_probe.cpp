/*
  Prints exactly what an AudioDeviceManager on this machine can see, and what it
  ends up opening when nothing is asked for - which is the path PedalBoard takes on
  a first run. Written because the running app showed empty device selectors while
  reporting a 48 kHz / 480-sample stream, and the two facts can only both be true if
  the listing and the opened device disagree about names.

  Build:  cmake --build _tests/build --config Release --target device_probe
  Run:    _tests\build\Release\device_probe.exe
*/
#include <juce_audio_devices/juce_audio_devices.h>

#include <cstdio>

// The device names on this machine are Chinese ("麦克风", "扬声器"), and the console
// starts in codepage 936, which renders JUCE's UTF-8 output as mojibake. Setting the
// output codepage is the difference between a probe that can be read and one that
// looks like a bug in the app.
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

namespace
{

void describeTypes (juce::AudioDeviceManager& manager, const char* tag)
{
    std::printf ("\n[%s] available device types\n", tag);

    for (auto* type : manager.getAvailableDeviceTypes())
    {
        if (type == nullptr)
            continue;

        type->scanForDevices();

        const auto inputs  = type->getDeviceNames (true);
        const auto outputs = type->getDeviceNames (false);

        // AudioIODeviceType exposes only a type name (no identifier accessor in
        // JUCE 8), plus the default index per direction, which is what tells us
        // which of these names an initialise-with-nothing ends up opening.
        std::printf ("  type %-22s inputs=%d outputs=%d defaultIn=%d defaultOut=%d\n",
                     type->getTypeName().toRawUTF8(),
                     inputs.size(), outputs.size(),
                     type->getDefaultDeviceIndex (true),
                     type->getDefaultDeviceIndex (false));

        for (int i = 0; i < inputs.size(); ++i)
            std::printf ("      in [%d]  %s\n", i, inputs[i].toRawUTF8());

        for (int i = 0; i < outputs.size(); ++i)
            std::printf ("      out [%d] %s\n", i, outputs[i].toRawUTF8());
    }
}

void reportCurrent (juce::AudioDeviceManager& manager, const char* tag)
{
    std::printf ("\n[%s] after initialise\n", tag);
    std::printf ("  getCurrentAudioDeviceType() = \"%s\"\n",
                 manager.getCurrentAudioDeviceType().toRawUTF8());

    const auto setup = manager.getAudioDeviceSetup();
    std::printf ("  setup.inputDeviceName       = \"%s\"\n", setup.inputDeviceName.toRawUTF8());
    std::printf ("  setup.outputDeviceName      = \"%s\"\n", setup.outputDeviceName.toRawUTF8());
    std::printf ("  setup.sampleRate            = %g  bufferSize=%d\n",
                 setup.sampleRate, setup.bufferSize);

    if (auto* device = manager.getCurrentAudioDevice())
    {
        std::printf ("  open device: getName()=\"%s\" getTypeName()=\"%s\" %g Hz %d samples\n",
                     device->getName().toRawUTF8(), device->getTypeName().toRawUTF8(),
                     device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
        std::printf ("             available out=%s in=%s\n",
                     device->getOutputChannelNames().joinIntoString (", ").toRawUTF8(),
                     device->getInputChannelNames().joinIntoString (", ").toRawUTF8());
    }
    else
    {
        std::printf ("  open device: NONE\n");
    }
}

} // namespace

int main()
{
    SetConsoleOutputCP (65001);   // UTF-8, so Chinese device names survive the trip

    juce::ScopedJuceInitialiser_GUI initialise;

    juce::AudioDeviceManager inputManager;
    describeTypes (inputManager, "input manager, before");

    if (auto error = inputManager.initialise (2, 0, nullptr, false); error.isNotEmpty())
        std::printf ("\n[input manager] initialise error: %s\n", error.toRawUTF8());
    reportCurrent (inputManager, "input manager");

    juce::AudioDeviceManager outputManager;
    if (auto error = outputManager.initialise (0, 2, nullptr, false); error.isNotEmpty())
        std::printf ("\n[output manager] initialise error: %s\n", error.toRawUTF8());
    reportCurrent (outputManager, "output manager");

    // Let the devices run for a moment, then report again: a device that opens but
    // never starts would explain a latency figure with no name behind it.
    juce::Thread::sleep (400);

    reportCurrent (inputManager, "input manager, after 400 ms");
    reportCurrent (outputManager, "output manager, after 400 ms");

    return 0;
}
