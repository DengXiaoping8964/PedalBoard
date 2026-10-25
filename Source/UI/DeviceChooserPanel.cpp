#include "UI/DeviceChooserPanel.h"

#include <algorithm>
#include <cmath>

#include "UI/DarkLookAndFeel.h"
#include "VirtualDevice/VBCableRouter.h"

namespace pedal
{

namespace
{
    /** Item id 1 is always the "nothing chosen" entry in every box here, because
        ComboBox reserves id 0 for "no selection at all" and mixing the two makes
        getSelectedId() ambiguous. Real items start at 2. */
    constexpr int noneItemId = 1;
    constexpr int firstItemId = 2;

    const char* const noMidiLabel = "(no MIDI input)";

    juce::String msLabel (int samples, double rate)
    {
        if (rate <= 0.0 || samples <= 0)
            return juce::String (samples);

        return juce::String (samples) + " ("
             + juce::String (1000.0 * static_cast<double> (samples) / rate, 1) + " ms)";
    }
}

//==============================================================================
DeviceChooserPanel::DeviceChooserPanel (AudioEngine& engineToUse)
    : engine (engineToUse)
{
    setInterceptsMouseClicks (true, true);

    auto makeBox = [this] (std::unique_ptr<juce::ComboBox>& box)
    {
        box = std::make_unique<juce::ComboBox>();
        box->setColour (juce::ComboBox::backgroundColourId, DarkLookAndFeel::instance().panelHighlighted);
        box->setColour (juce::ComboBox::textColourId,        DarkLookAndFeel::instance().textPrimary);
        box->setColour (juce::ComboBox::outlineColourId,     DarkLookAndFeel::instance().textDim);
        box->setColour (juce::ComboBox::buttonColourId,      DarkLookAndFeel::instance().panelBackground);
        box->setColour (juce::ComboBox::arrowColourId,       DarkLookAndFeel::instance().textPrimary);
        addAndMakeVisible (*box);
    };

    makeBox (inputTypeBox);
    makeBox (inputDeviceBox);
    makeBox (outputTypeBox);
    makeBox (outputDeviceBox);
    makeBox (bufferBox);
    makeBox (sampleRateBox);
    makeBox (midiBox);

    // Changing a device type only re-populates that section's own name box. The
    // two sides never touch each other, which is the point of the whole panel.
    inputTypeBox->onChange = [this]
    {
        if (suppressChanges)
            return;
        const juce::ScopedValueSetter<bool> guard (suppressChanges, true);
        fillDeviceBox (*inputDeviceBox, inputTypeBox->getText(), true);
    };

    outputTypeBox->onChange = [this]
    {
        if (suppressChanges)
            return;
        const juce::ScopedValueSetter<bool> guard (suppressChanges, true);
        fillDeviceBox (*outputDeviceBox, outputTypeBox->getText(), false);
    };

    // Rate changes what the buffer size means in milliseconds, so the buffer box
    // is relabelled - not re-applied - whenever the clock is picked.
    sampleRateBox->onChange = [this]
    {
        if (suppressChanges)
            return;
        const juce::ScopedValueSetter<bool> guard (suppressChanges, true);
        fillBufferBox();
    };

    virtualFlag = std::make_unique<FlagButton> (tr ("VIRTUAL OUT"), true);
    virtualFlag->onClicked = [this] (bool on)
    {
        // Switching the cable on or off is applied straight away: it cannot disturb
        // the monitoring path, and waiting for Accept would leave the status line
        // describing a state the user has already changed.
        engine.setVirtualOutputEnabled (on);
        refreshCableStatus();
        repaint();
    };
    addAndMakeVisible (*virtualFlag);

    rescanButton = std::make_unique<SmallTextButton> (tr ("Rescan"));
    rescanButton->onClick = [this]
    {
        // The only supported way to notice a driver installed since launch.
        engine.rescanDevices();
        engine.getVirtualRouter().scanForCable();
        populate();
        refreshCableStatus();
    };
    addAndMakeVisible (*rescanButton);

    acceptButton = std::make_unique<SmallTextButton> (tr ("Apply"));
    acceptButton->onClick = [this] { if (onAccepted) onAccepted(); };
    addAndMakeVisible (*acceptButton);

    cancelButton = std::make_unique<SmallTextButton> (tr ("Close"));
    cancelButton->onClick = [this] { if (onCancelled) onCancelled(); };
    addAndMakeVisible (*cancelButton);

    /*  The language lives here rather than in the top bar: row one already runs
        to 1048 logical pixels at its fixed widths, which is past the window's
        1040 minimum, so a seventh control would have been clipped on a small
        screen. Each entry is written in the script it selects, so the box is
        readable before anything has been translated. */
    makeBox (languageBox);
    languageBox->addItem (languageName (Language::english), englishItem);
    languageBox->addItem (languageName (Language::chinese), chineseItem);
    languageBox->setSelectedId (currentLanguage() == Language::chinese ? chineseItem
                                                                       : englishItem,
                                juce::dontSendNotification);
    languageBox->onChange = [this]
    {
        if (suppressChanges)
            return;

        onLanguageChosen (languageBox->getSelectedId() == chineseItem ? Language::chinese
                                                                      : Language::english);
    };
}

DeviceChooserPanel::~DeviceChooserPanel() = default;

//==============================================================================
void DeviceChooserPanel::populate()
{
    const juce::ScopedValueSetter<bool> guard (suppressChanges, true);

    fillTypeBox (*inputTypeBox, true);
    fillDeviceBox (*inputDeviceBox, inputTypeBox->getText(), true);

    fillTypeBox (*outputTypeBox, false);
    fillDeviceBox (*outputDeviceBox, outputTypeBox->getText(), false);

    fillRateBox();
    fillBufferBox();
    fillMidiBox();

    virtualFlag->setOn (engine.getVirtualRouter().isEnabled());

    refreshCableStatus();
    repaint();
}

void DeviceChooserPanel::refreshText()
{
    virtualFlag->setLabelText (tr ("VIRTUAL OUT"));
    rescanButton->setButtonText (tr ("Rescan"));
    acceptButton->setButtonText (tr ("Apply"));
    cancelButton->setButtonText (tr ("Close"));

    /*  Only the selection is touched, never the item list: this runs from inside
        the box's own onChange when the user is the one who changed it, and
        clear()/addItem() on a ComboBox mid-callback is how the add-effect box
        already had to be deferred. The two entries are each other's language, so
        they do not need re-translating either. */
    languageBox->setSelectedId (currentLanguage() == Language::chinese ? chineseItem
                                                                       : englishItem,
                                juce::dontSendNotification);

    /*  The MIDI box is the one list in here with a translated entry - "(no MIDI
        input)" - so it is the one box that has to be rebuilt. Device type and
        device name come from the system and are never translated, which is why
        they are left alone rather than re-enumerated. */
    {
        const juce::ScopedValueSetter<bool> guard (suppressChanges, true);
        fillMidiBox();
    }

    refreshCableStatus();
    repaint();
}

void DeviceChooserPanel::fillTypeBox (juce::ComboBox& box, bool wantInputs)
{
    const auto keep = box.getText();
    const auto wanted = wantInputs ? engine.getInputDeviceType() : engine.getOutputDeviceType();

    box.clear (juce::dontSendNotification);

    // Enumerated through the engine, which reads the list off its own manager; both
    // managers see the same types on Windows, but the input side is asked for input
    // capable types and the output side for output capable ones.
    const auto types = engine.listDeviceTypes (wantInputs, ! wantInputs);

    for (int i = 0; i < types.size(); ++i)
        box.addItem (types[i], i + firstItemId);

    const auto index = types.indexOf (keep.isNotEmpty() ? keep : wanted);
    box.setSelectedId (index >= 0 ? index + firstItemId : 0, juce::dontSendNotification);
}

void DeviceChooserPanel::fillDeviceBox (juce::ComboBox& box, const juce::String& typeName,
                                        bool wantInputs)
{
    const auto keep = box.getText();
    const auto wanted = wantInputs ? engine.getInputDeviceName() : engine.getOutputDeviceName();

    box.clear (juce::dontSendNotification);

    if (typeName.isEmpty())
        return;

    const auto names = engine.listDevicesOfType (typeName, wantInputs);

    for (int i = 0; i < names.size(); ++i)
        box.addItem (names[i], i + firstItemId);

    const auto index = names.indexOf (keep.isNotEmpty() ? keep : wanted);
    box.setSelectedId (index >= 0 ? index + firstItemId : 0, juce::dontSendNotification);
}

void DeviceChooserPanel::fillRateBox()
{
    availableRates = engine.getAvailableSampleRates();

    sampleRateBox->clear (juce::dontSendNotification);

    for (int i = 0; i < availableRates.size(); ++i)
        sampleRateBox->addItem (juce::String (availableRates[i], 0) + " Hz", i + firstItemId);

    const auto current = engine.getSampleRate();
    auto bestIndex = -1;
    auto bestGap = 1.0e9;

    for (int i = 0; i < availableRates.size(); ++i)
    {
        const auto gap = std::abs (availableRates[i] - current);

        if (gap < bestGap)
        {
            bestGap = gap;
            bestIndex = i;
        }
    }

    sampleRateBox->setSelectedId (bestIndex >= 0 ? bestIndex + firstItemId : 0,
                                  juce::dontSendNotification);
}

void DeviceChooserPanel::fillBufferBox()
{
    availableBuffers = engine.getAvailableBufferSizes();
    const auto rate = engine.getSampleRate();

    bufferBox->clear (juce::dontSendNotification);

    for (int i = 0; i < availableBuffers.size(); ++i)
        bufferBox->addItem (msLabel (availableBuffers[i], rate), i + firstItemId);

    const auto index = availableBuffers.indexOf (engine.getBufferSizeSamples());
    bufferBox->setSelectedId (index >= 0 ? index + firstItemId : 0, juce::dontSendNotification);
}

void DeviceChooserPanel::fillMidiBox()
{
    const auto keep = midiBox->getText();
    const auto opened = engine.getExpressionMidiInputName();

    midiBox->clear (juce::dontSendNotification);
    midiBox->addItem (tr (noMidiLabel), noneItemId);

    const auto names = AudioEngine::getMidiInputNames();

    for (int i = 0; i < names.size(); ++i)
        midiBox->addItem (names[i], i + firstItemId);

    const auto wanted = keep.isNotEmpty() ? keep : opened;

    if (wanted.isEmpty())
    {
        midiBox->setSelectedId (noneItemId, juce::dontSendNotification);
        return;
    }

    const auto index = names.indexOf (wanted);
    midiBox->setSelectedId (index >= 0 ? index + firstItemId : noneItemId, juce::dontSendNotification);
}

//==============================================================================
DeviceChooserPanel::Selection DeviceChooserPanel::getSelection() const
{
    Selection s;

    s.inputType = inputTypeBox->getText();
    s.inputDevice = inputDeviceBox->getText();
    s.outputType = outputTypeBox->getText();
    s.outputDevice = outputDeviceBox->getText();

    const auto rateIndex = sampleRateBox->getSelectedItemIndex();
    if (rateIndex >= 0 && rateIndex < availableRates.size())
        s.sampleRate = availableRates[rateIndex];

    const auto bufferIndex = bufferBox->getSelectedItemIndex();
    if (bufferIndex >= 0 && bufferIndex < availableBuffers.size())
        s.bufferSize = availableBuffers[bufferIndex];

    s.midiDevice = midiBox->getSelectedId() == noneItemId ? juce::String() : midiBox->getText();

    return s;
}

juce::String DeviceChooserPanel::applySelections()
{
    const auto s = getSelection();
    juce::String problems;

    // Devices first: a clock applied to a half-configured pair is the one way to
    // make the monitoring path briefly sound like a pitch shifter.
    //
    // Whole sentences through trf, not a translated prefix glued to the value: the
    // word order of "Input: <error>" is not the same in every language.
    if (s.inputDevice.isNotEmpty())
    {
        if (const auto error = engine.setInputDevice (s.inputType, s.inputDevice); error.isNotEmpty())
            problems << trf ("Input: %1", error) << "\n";
    }

    if (s.outputDevice.isNotEmpty())
    {
        if (const auto error = engine.setOutputDevice (s.outputType, s.outputDevice); error.isNotEmpty())
            problems << trf ("Output: %1", error) << "\n";
    }

    if (s.sampleRate > 0.0)
    {
        if (const auto error = engine.setSampleRate (s.sampleRate); error.isNotEmpty())
            problems << trf ("Sample rate: %1", error) << "\n";
    }

    if (s.bufferSize > 0)
    {
        if (const auto error = engine.setBufferSize (s.bufferSize); error.isNotEmpty())
            problems << trf ("Buffer size: %1", error) << "\n";
    }

    if (s.midiDevice.isEmpty())
        engine.closeExpressionMidiInput();
    else if (engine.hasExpressionMidiInput() && s.midiDevice == engine.getExpressionMidiInputName())
    {
        // Already open; re-opening would drop the callback for a block.
    }
    else if (const auto error = engine.openExpressionMidiInput (s.midiDevice); error.isNotEmpty())
        problems << trf ("MIDI: %1", error) << "\n";

    engine.getVirtualRouter().setEnabled (virtualFlag->isOn());

    // The boxes now show what the engine actually got, not what was asked for: a
    // device that refused the rate has to be visible here rather than later.
    populate();

    problemLine = problems;
    return problems;
}

//==============================================================================
void DeviceChooserPanel::refreshCableStatus()
{
    auto& router = engine.getVirtualRouter();
    const auto status = router.getStatus();

    cableLine = router.getStatusLine();

    /*  Colour is decided from the router's own availability, not by reading the
        status sentence back: the sentence is translated, so "not detected" stops
        being a test once the interface is not in English. */
    cableMissing = status.availability == VBCableRouter::Availability::notInstalled
                   || status.availability == VBCableRouter::Availability::unknown;

    // The licence notice is shown when the cable is missing, and also when it is
    // present but would not be obvious - the terms apply either way, and the user
    // is the one who will be recording through it.
    if (status.availability == VBCableRouter::Availability::notInstalled
        || status.availability == VBCableRouter::Availability::unknown)
        cableGuidance = VBCableRouter::getInstallationGuidance();    else if (status.availability == VBCableRouter::Availability::installed)
        cableGuidance = tr ("VB-CABLE is present but the stream is not running. Press Apply "
                            "to open it, or switch VIRTUAL OUT off and on again.");
    else
    {
        const auto driver = status.deviceType.isNotEmpty() ? status.deviceType
                                                           : tr ("the default driver");

        cableGuidance = trf ("Recording through CABLE Output adds its own buffer on top of the "
                             "%1 ms shown here, because the cable is opened over %2. "
                             "The monitor path is the low-latency one; recording is not.",
                             juce::String (status.extraLatencyMs, 0), driver);
    }

    summaryLine = engine.getSettingsSummary();
}

//==============================================================================
juce::Rectangle<int> DeviceChooserPanel::cardBounds() const noexcept
{
    // A dimmed backdrop with a fixed-size card centred in it, sized down to what
    // the window can actually show so nothing is unreachable at the minimum size.
    const auto width = juce::jmin (660, getWidth() - 24);
    const auto height = juce::jmin (640, getHeight() - 24);

    return getLocalBounds().withSizeKeepingCentre (width, juce::jmax (320, height));
}

juce::Rectangle<int> DeviceChooserPanel::languageBoxBounds (const juce::Rectangle<int>& card) noexcept
{
    // In the button row rather than among the six device rows: the interface
    // language is not a device setting, and it takes effect on its own without
    // Apply. Placed after Rescan, whose box ends at card.getX() + 98.
    const auto buttonsY = card.getBottom() - 18 - 24;
    return { card.getX() + 228, buttonsY, 120, 24 };
}

DeviceChooserPanel::Rows DeviceChooserPanel::layoutRows (const juce::Rectangle<int>& card) noexcept
{
    constexpr int margin = 18;
    constexpr int narrow = 150;
    constexpr int gap = 12;
    constexpr int rowPitch = 44;   // caption + box + breathing room

    Rows r;
    r.captionX = card.getX() + margin;
    r.boxX = r.captionX;
    r.narrowWidth = narrow;
    r.wideX = r.boxX + narrow + gap;
    r.wideWidth = juce::jmax (120, card.getRight() - margin - r.wideX);

    /*  The intro paragraph above this row is three lines of 13-pixel leading
        starting at card.getY() + 38, so it reaches +77; the first caption used to
        sit at +62 and the two were painted over each other (seen in the first
        screenshot of the running app). +88 clears the paragraph with room left
        over, and resized() reads the same value so the boxes cannot disagree. */
    r.inputY  = card.getY() + 88;
    r.outputY = r.inputY + rowPitch;
    r.clockY  = r.outputY + rowPitch;
    r.midiY   = r.clockY + rowPitch;
    r.cableY  = r.midiY + rowPitch + 6;

    return r;
}

void DeviceChooserPanel::paint (juce::Graphics& g)
{
    auto& laf = DarkLookAndFeel::instance();

    g.fillAll (laf.windowBackground.withAlpha (0.86f));

    const auto card = cardBounds();
    const auto cardFloat = card.toFloat().reduced (0.5f);
    const auto rows = layoutRows (card);

    g.setColour (laf.panelBackground);
    g.fillRoundedRectangle (cardFloat, 8.0f);
    g.setColour (laf.panelHighlighted);
    g.drawRoundedRectangle (cardFloat, 8.0f, 1.0f);

    const auto bodyFont = DarkLookAndFeel::makeFont (11.0f);

    g.setFont (DarkLookAndFeel::makeFont (17.0f).boldened());
    g.setColour (laf.textPrimary);
    g.drawText (tr ("Audio devices"), card.getX() + 18, card.getY() + 10,
                card.getWidth() - 36, 22, juce::Justification::centredLeft, false);

    g.setFont (bodyFont);
    g.setColour (laf.textDim);
    g.drawMultiLineText (tr ("Input and output are separate on purpose: the guitar enters one "
                             "device and the chain leaves through the other, and switching either "
                             "one leaves the second running."),
                         card.getX() + 18, card.getY() + 38 + static_cast<int> (bodyFont.getAscent()),
                         card.getWidth() - 36, juce::Justification::left, 13.0f);

    //==============================================================================
    //  Row captions. The boxes themselves are components placed by resized(); the
    //  text is drawn here so the two can never disagree about position.
    struct Caption { const char* text; int x; int y; int width; };

    /*  The captions are the only text in the app that is deliberately bilingual on
        screen: the Chinese for "INPUT DEVICE TYPE" is long enough to crowd the
        narrow column it sits above, so the English stays underneath it. */
    const Caption captions[] =
    {
        { "INPUT DEVICE TYPE",    rows.boxX,     rows.inputY,  rows.narrowWidth },
        { "INPUT DEVICE",         rows.wideX,    rows.inputY,  rows.wideWidth },
        { "MONITOR DEVICE TYPE",  rows.boxX,     rows.outputY, rows.narrowWidth },
        { "MONITOR DEVICE",       rows.wideX,    rows.outputY, rows.wideWidth },
        { "BUFFER SIZE",          rows.boxX,     rows.clockY,  rows.narrowWidth },
        { "SAMPLE RATE",          rows.wideX,    rows.clockY,  rows.narrowWidth },
        { "VIRTUAL OUTPUT",       rows.wideX + rows.narrowWidth + 12, rows.clockY, rows.narrowWidth },
        { "EXPRESSION PEDAL (CC11 / CC1)", rows.boxX, rows.midiY, rows.wideWidth },
    };

    g.setFont (DarkLookAndFeel::makeFont (9.0f).boldened());
    g.setColour (laf.textDim);

    for (auto& caption : captions)
    {
        const auto english = juce::String (caption.text);
        const auto text = currentLanguage() == Language::chinese
                            ? tr (caption.text) + "  " + english : english;

        g.drawText (text, caption.x, caption.y, caption.width, 14,
                    juce::Justification::centredLeft, false);
    }

    //==============================================================================
    //  The cable block: one action line, then the notice, then what is running.
    auto y = rows.cableY;

    g.setFont (DarkLookAndFeel::makeFont (12.0f).boldened());
    g.setColour (cableMissing ? laf.warning : laf.good);
    g.drawText (cableLine, rows.captionX, y, card.getWidth() - 36, 18,
                juce::Justification::centredLeft, true);
    y += 22;

    if (! cableGuidance.isEmpty())
    {
        g.setFont (bodyFont);
        g.setColour (laf.textDim);
        g.drawMultiLineText (cableGuidance, rows.captionX,
                             y + static_cast<int> (bodyFont.getAscent()),
                             card.getWidth() - 36, juce::Justification::left, 14.0f);

        /*  Measured, not assumed. This used to advance a fixed 118 pixels, which
            was already too little for the English notice - the summary line drew
            across the third paragraph - and Chinese wraps to a different number of
            lines again. */
        juce::AttributedString wrapped { cableGuidance };
        wrapped.setFont (bodyFont);
        wrapped.setLineSpacing (14.0f);
        wrapped.setJustification (juce::Justification::left);

        juce::TextLayout layout;
        layout.createLayout (wrapped, static_cast<float> (card.getWidth() - 36));

        y += static_cast<int> (layout.getHeight()) + 12;
    }

    if (! summaryLine.isEmpty())
    {
        g.setFont (bodyFont);
        g.setColour (laf.textPrimary);
        g.drawText (summaryLine, rows.captionX, y, card.getWidth() - 36, 16,
                    juce::Justification::centredLeft, true);
        y += 18;
    }

    if (! problemLine.isEmpty())
    {
        g.setFont (DarkLookAndFeel::makeFont (11.0f).boldened());
        g.setColour (laf.bad);
        g.drawMultiLineText (problemLine, rows.captionX,
                             y + static_cast<int> (bodyFont.getAscent()),
                             card.getWidth() - 36, juce::Justification::left, 13.0f);
    }

    // The language box is a component too; only its caption is drawn.
    const auto langBox = languageBoxBounds (card);

    g.setFont (DarkLookAndFeel::makeFont (9.0f).boldened());
    g.setColour (laf.textDim);
    g.drawText (tr ("INTERFACE LANGUAGE"),
                juce::Rectangle<int> (langBox.getX() - 122, langBox.getY(), 114, langBox.getHeight()),
                juce::Justification::centredRight, false);

    // The buttons are components; this only draws the rule they sit under.
    g.setColour (laf.panelHighlighted);
    g.drawLine (static_cast<float> (card.getX() + 18), static_cast<float> (card.getBottom() - 36),
                static_cast<float> (card.getRight() - 18), static_cast<float> (card.getBottom() - 36));
}

void DeviceChooserPanel::resized()
{
    const auto card = cardBounds();
    const auto rows = layoutRows (card);

    constexpr int boxHeight = 22;
    const auto boxY = 16;   // below the caption drawn in paint()

    inputTypeBox->setBounds   (rows.boxX,  rows.inputY + boxY, rows.narrowWidth, boxHeight);
    inputDeviceBox->setBounds (rows.wideX, rows.inputY + boxY, rows.wideWidth, boxHeight);

    outputTypeBox->setBounds   (rows.boxX,  rows.outputY + boxY, rows.narrowWidth, boxHeight);
    outputDeviceBox->setBounds (rows.wideX, rows.outputY + boxY, rows.wideWidth, boxHeight);

    bufferBox->setBounds     (rows.boxX,  rows.clockY + boxY, rows.narrowWidth, boxHeight);
    sampleRateBox->setBounds (rows.wideX, rows.clockY + boxY, rows.narrowWidth, boxHeight);

    virtualFlag->setBounds (rows.wideX + rows.narrowWidth + 12, rows.clockY + boxY - 2,
                            rows.narrowWidth, boxHeight + 4);

    midiBox->setBounds (rows.boxX, rows.midiY + boxY, rows.narrowWidth * 2 + 12, boxHeight);

    const auto buttonsY = card.getBottom() - 18 - 24;
    rescanButton->setBounds   (card.getX() + 18, buttonsY, 80, 24);
    cancelButton->setBounds   (card.getRight() - 18 - 80, buttonsY, 80, 24);
    acceptButton->setBounds   (cancelButton->getX() - 8 - 80, buttonsY, 80, 24);

    languageBox->setBounds (languageBoxBounds (card));
}

} // namespace pedal
