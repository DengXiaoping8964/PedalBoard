/*
    VST3 hosting probe.

    Loads one .vst3 effect the same way PluginHostEffect does - through
    AudioPluginFormatManager and AudioPluginInstance - and reports what the host
    can actually see: the class, channel counts, the first parameters, whether
    processing changes audio, and whether state round-trips. It exists because
    "the editor opened" proves COM and the window, not that the audio path or the
    macro mapping points at the knobs the card claims.

    Run from a console:  vst3_probe.exe "C:\path\to\effect.vst3"
*/

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

namespace
{

juce::AudioPluginFormat* findVst3 (juce::AudioPluginFormatManager& manager, const juce::File& file)
{
    for (int i = 0; i < manager.getNumFormats(); ++i)
    {
        auto* format = manager.getFormat (i);

        if (format != nullptr
            && format->getName().equalsIgnoreCase ("VST3")
            && format->fileMightContainThisPluginType (file.getFullPathName()))
            return format;
    }

    return nullptr;
}

float rmsOf (const juce::AudioBuffer<float>& buffer)
{
    double sum = 0.0;

    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            sum += static_cast<double> (buffer.getSample (c, i)) * buffer.getSample (c, i);

    return static_cast<float> (std::sqrt (sum / (buffer.getNumChannels() * buffer.getNumSamples())));
}

} // namespace

int main (int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf ("usage: vst3_probe <plugin.vst3>\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    const juce::File file (juce::String::fromUTF8 (argv[1]));

    if (! file.existsAsFile())
    {
        std::printf ("missing file: %s\n", file.getFullPathName().toRawUTF8());
        return 1;
    }

    juce::AudioPluginFormatManager manager;
    juce::addDefaultFormatsToManager (manager);

    std::printf ("formats:");
    for (int i = 0; i < manager.getNumFormats(); ++i)
        std::printf (" %s", manager.getFormat (i)->getName().toRawUTF8());
    std::printf ("\n");

    auto* format = findVst3 (manager, file);

    if (format == nullptr)
    {
        std::printf ("no VST3 format accepted the file\n");
        return 1;
    }

    juce::OwnedArray<juce::PluginDescription> descriptions;
    format->findAllTypesForFile (descriptions, file.getFullPathName());

    std::printf ("classes=%d\n", descriptions.size());

    juce::PluginDescription* chosen = nullptr;

    for (auto* description : descriptions)
    {
        if (description == nullptr)
            continue;

        std::printf ("  '%s' instrument=%d channels in=%d out=%d\n",
                     description->descriptiveName.toRawUTF8(),
                     description->isInstrument ? 1 : 0,
                     description->numInputChannels, description->numOutputChannels);

        if (! description->isInstrument && chosen == nullptr)
            chosen = description;
    }

    if (chosen == nullptr)
    {
        std::printf ("no effect class found\n");
        return 1;
    }

    juce::String error;
    constexpr double rate = 44100.0;
    constexpr int block = 512;
    auto instance = manager.createPluginInstance (*chosen, rate, block, error);

    if (instance == nullptr)
    {
        std::printf ("instantiation failed: %s\n", error.toRawUTF8());
        return 1;
    }

    std::printf ("instance '%s' hasEditor=%d latency=%d\n",
                 instance->getName().toRawUTF8(),
                 instance->hasEditor() ? 1 : 0,
                 instance->getLatencySamples());

    instance->setPlayConfigDetails (juce::jmin (2, instance->getTotalNumInputChannels()),
                                    juce::jmin (2, instance->getTotalNumOutputChannels()),
                                    rate, block);
    instance->prepareToPlay (rate, block);

    const auto& parameters = instance->getParameters();
    std::printf ("parameters=%d\n", parameters.size());

    int automatable = 0;

    for (int i = 0; i < parameters.size() && i < 12; ++i)
    {
        auto* parameter = parameters[i];

        if (parameter == nullptr)
            continue;

        if (parameter->isAutomatable())
            ++automatable;

        std::printf ("  [%2d] '%s' automatable=%d default=%.4f value=%.4f steps=%d\n",
                     i, parameter->getName (64).toRawUTF8(),
                     parameter->isAutomatable() ? 1 : 0,
                     parameter->getDefaultValue(), parameter->getValue(),
                     parameter->getNumSteps());
    }

    std::printf ("automatable in first 12: %d\n", automatable);

    // White noise rather than a sine: a spatial effect with delay and reverb has
    // no single expected THD, but it must change the signal and stay finite.
    // Processed at the plugin's own defaults, before any macro is moved.
    juce::AudioBuffer<float> buffer (2, block);
    juce::Random random (0x51ee7);

    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = 0; i < block; ++i)
            buffer.setSample (c, i, 0.2f * random.nextFloat() - 0.1f);

    juce::MidiBuffer midi;
    auto inputCopy = buffer;
    const auto inputRms = rmsOf (inputCopy);

    for (int pass = 0; pass < 8; ++pass)
    {
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            buffer.copyFrom (c, 0, inputCopy, c, 0, block);

        juce::AudioBuffer<float> view (buffer.getArrayOfWritePointers(),
                                       buffer.getNumChannels(), block);
        instance->processBlock (view, midi);
    }

    float peak = 0.0f;
    float largestDifference = 0.0f;
    bool finite = true;

    for (int c = 0; c < buffer.getNumChannels(); ++c)
        for (int i = 0; i < block; ++i)
        {
            const auto v = buffer.getSample (c, i);

            if (! std::isfinite (v))
                finite = false;

            peak = juce::jmax (peak, std::abs (v));
            largestDifference = juce::jmax (largestDifference,
                                            std::abs (v - inputCopy.getSample (c, i)));
        }

    const auto outputRms = rmsOf (buffer);

    std::printf ("audio: in rms %.5f out rms %.5f peak %.5f max diff %.5f finite=%d\n",
                 inputRms, outputRms, peak, largestDifference, finite ? 1 : 0);

    // Move the first macro the same way the card does - skipping the bypass
    // parameter, exactly as the card's mapping does - and confirm the plugin's
    // own parameter object took the value.
    auto* bypassParameter = instance->getBypassParameter();
    juce::AudioProcessorParameter* firstMacro = nullptr;

    for (auto* parameter : parameters)
        if (parameter != nullptr && parameter != bypassParameter
            && parameter->isAutomatable()
            && ! parameter->getName (64).containsIgnoreCase ("bypass"))
        {
            firstMacro = parameter;
            break;
        }

    std::printf ("first macro: '%s'\n",
                 firstMacro != nullptr ? firstMacro->getName (64).toRawUTF8() : "(none)");

    if (firstMacro != nullptr)
    {
        const auto before = firstMacro->getValue();
        firstMacro->setValueNotifyingHost (0.5f);
        std::printf ("macro write: '%s' %.4f -> %.4f (requested 0.5000)\n",
                     firstMacro->getName (64).toRawUTF8(), before, firstMacro->getValue());
    }

    juce::MemoryBlock state;
    instance->getStateInformation (state);
    std::printf ("state bytes=%d\n", static_cast<int> (state.getSize()));

    if (state.getSize() > 0)
    {
        instance->setStateInformation (state.getData(), static_cast<int> (state.getSize()));
        juce::MemoryBlock second;
        instance->getStateInformation (second);
        std::printf ("state round-trip bytes=%d\n", static_cast<int> (second.getSize()));
    }

    return 0;
}
