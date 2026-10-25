#include "UI/MainComponent.h"

#include <cmath>
#include <functional>

#include "Effects/EffectFactory.h"
#include "UI/DarkLookAndFeel.h"
#include "VirtualDevice/VBCableRouter.h"

#include <vector>
#include <memory>

namespace pedal
{

namespace
{
    /** One place that decides how a millisecond figure is written, so the status
        bar and the latency captions never disagree. */
    juce::String ms (double value)
    {
        return juce::String (value, 1) + " ms";
    }

    juce::String percent (double value)
    {
        return juce::String (value * 100.0, 0) + "%";
    }

    juce::String decibels (double value)
    {
        return juce::String (value, 1) + " dB";
    }

    /** Item ids in every box are index + 1, because 0 is ComboBox's "nothing
        selected" marker and a real item must never collide with it. */
    constexpr int promptId = 0;
}

//==============================================================================
//  ChainList
//==============================================================================
void ChainList::layoutCards()
{
    auto y = 0;

    for (auto* card : cards)
    {
        if (card == nullptr)
            continue;

        const auto height = EffectCard::getPreferredHeight (card->getEffect());
        card->setBounds (0, y, getWidth(), height);
        y += height + cardGap;
    }

    // The Viewport derives its scroll range from this component's bounds, so the
    // column has to report its own full height (see juce_Viewport.cpp,
    // updateVisibleArea(): it moves the content, never resizes it).
    setSize (getWidth(), juce::jmax (cardGap, y));
}

int ChainList::gapForY (int y) const noexcept
{
    const auto count = getNumCards();

    for (int i = 0; i < count; ++i)
    {
        const auto& bounds = cards[static_cast<std::size_t> (i)]->getBounds();

        if (y < bounds.getY())
            return i;

        if (y < bounds.getBottom())
            return y - bounds.getY() < bounds.getHeight() / 2 ? i : i + 1;
    }

    return count;
}

EffectCard* ChainList::cardForGap (int gap) const noexcept
{
    if (cards.empty())
        return nullptr;

    const auto index = static_cast<std::size_t> (juce::jlimit (0, getNumCards() - 1, gap));
    return cards[index];
}

void ChainList::paint (juce::Graphics& g)
{
    if (dropGap < 0 || cards.empty())
        return;

    int y = 0;

    if (dropGap >= getNumCards())
        y = cards[static_cast<std::size_t> (getNumCards() - 1)]->getBounds().getBottom()
          + cardGap / 2;
    else
        y = cards[static_cast<std::size_t> (dropGap)]->getBounds().getY() - cardGap / 2;

    g.setColour (DarkLookAndFeel::instance().accent);
    g.fillRoundedRectangle (juce::Rectangle<float> (0.0f, static_cast<float> (y) - 2.0f,
                                                    static_cast<float> (getWidth()), 4.0f), 2.0f);
}

//==============================================================================
//  MainComponent
//==============================================================================
MainComponent::MainComponent (AudioEngine& engineToUse)
    : engine (engineToUse)
{
    setOpaque (true);

    /*  The order here is the order that matters:

        1. applyAndStart() opens both managers and builds the chain from the
           settings file, so the card list below has something real to display;
        2. the widgets are created after it, so refreshGlobalControls() reads back
           the values the file restored instead of showing defaults;
        3. the chooser is only shown when the user has never been through it, when
           start-up reported a problem, or when neither device actually came up.
            */
    AudioEngine::Settings settings;
    AudioEngine::loadFromFile (settings);

    /*  The language has to be installed before a single widget is built: the
        strings are translated at creation for the widgets that hold their text,
        and at paint time for the ones that do not, and both read the mappings. */
    setLanguage (settings.language == AudioEngine::Settings::noLanguage
                                       ? Language::english
                                       : static_cast<Language> (settings.language));

    // A file that names a language is the record that the question was answered.
    if (settings.language != AudioEngine::Settings::noLanguage)
        noteLanguageChosen();

    /*  Read back into the engine, which owns the file: a language chosen here is
        lost on exit otherwise, because persistSettings() writes the engine's copy
        rather than this component's. */
    engine.setStoredLanguage (static_cast<int> (settings.language));
    engine.setDevicesChosenFrom (settings.devicesChosen);

    problemText = engine.applyAndStart (settings);

    buildGlobalControls();
    buildChainArea();

    rebuildCards();
    refreshGlobalControls();
    populateDeviceBoxes();

    populateRateBox();
    populateBufferBox();
    populateAddBox();

    /*  The language question goes first when it has never been answered: the
        device chooser is the other thing a first run shows, and it is a lot to
        read in a language that was not picked. The chooser is asked for by the
        prompt's own handler in that case, not here.

        `chosen`, not "does the file exist", is the first-run test: persistSettings()
        also runs when the window closes, so a file proves nothing about whether the
        user ever saw the panel. */
    wantsDeviceChooser = ! settings.devicesChosen || problemText.isNotEmpty()
                         || ! engine.isRunning();

    if (! languageWasChosen())
        showLanguagePrompt();
    else if (wantsDeviceChooser)
        showDeviceChooser();

    updateStatusText();
    startTimer (uiRefreshMs);

    // A global listener, because the release that ends a card drag can land on
    // any component - or outside this one entirely.
    juce::Desktop::getInstance().addGlobalMouseListener (this);
}

MainComponent::~MainComponent()
{
    stopTimer();
    juce::Desktop::getInstance().removeGlobalMouseListener (this);

    persistSettings();

    /*  The cards are children of the chain list but owned by this component, so
        they leave the tree before they are deleted; deleting them after the
        viewport died would be a double free, and JUCE 8 has no removeFromParent()
        - removeChildComponent() detaches without deleting (verified in
        juce_Component.h). */
    if (chainList != nullptr)
    {
        for (auto& card : cards)
            chainList->removeChildComponent (card.get());
    }

    cards.clear();
    chainList = nullptr;
}

void MainComponent::persistSettings()
{
    engine.saveSettings();
}

//==============================================================================
void MainComponent::buildGlobalControls()
{
    auto installBox = [this] (std::unique_ptr<juce::ComboBox>& box)
    {
        box = std::make_unique<juce::ComboBox>();
        box->setColour (juce::ComboBox::backgroundColourId, DarkLookAndFeel::instance().panelBackground);
        box->setColour (juce::ComboBox::textColourId,        DarkLookAndFeel::instance().textPrimary);
        box->setColour (juce::ComboBox::outlineColourId,     DarkLookAndFeel::instance().panelHighlighted);
        box->setColour (juce::ComboBox::arrowColourId,       DarkLookAndFeel::instance().textDim);
        box->setColour (juce::ComboBox::buttonColourId,      DarkLookAndFeel::instance().panelHighlighted);
        addAndMakeVisible (*box);
    };

    installBox (inputTypeBox);
    installBox (inputDeviceBox);
    installBox (outputTypeBox);
    installBox (outputDeviceBox);
    installBox (bufferBox);
    installBox (sampleRateBox);
    installBox (addBox);

    inputTypeBox->onChange   = [this] { inputTypeChanged(); };
    inputDeviceBox->onChange = [this] { inputDeviceChanged(); };
    outputTypeBox->onChange  = [this] { outputTypeChanged(); };
    outputDeviceBox->onChange = [this] { outputDeviceChanged(); };
    bufferBox->onChange      = [this] { bufferChanged(); };
    sampleRateBox->onChange  = [this] { sampleRateChanged(); };
    addBox->onChange         = [this] { addEffectChanged(); };

    addBox->setTextWhenNothingSelected (tr ("Add effect..."));

    dryWetKnob = std::make_unique<Knob> (tr ("Dry/Wet"), 0.0, 1.0,
                                         [] (double v) { return percent (v); });
    trimKnob   = std::make_unique<Knob> (tr ("Input Trim"), -24.0, 24.0,
                                         [] (double v) { return decibels (v); });
    expressionKnob = std::make_unique<Knob> (tr ("Expression"), 0.0, 1.0,
                                             [] (double v) { return percent (v); });

    dryWetKnob->setDoubleClickDefault (1.0);
    trimKnob->setDoubleClickDefault (0.0);
    expressionKnob->setDoubleClickDefault (0.5);

    dryWetKnob->onValueChange = [this] (double v) { engine.setGlobalWet (static_cast<float> (v)); };
    trimKnob->onValueChange   = [this] (double v) { engine.setInputTrimDb (static_cast<float> (v)); };

    // Draggable only while no MIDI controller is driving the same atomic; with a
    // pedal connected the position is read-only, so the knob shows the truth
    // instead of fighting the incoming CC.
    expressionKnob->onValueChange = [this] (double v)
    {
        if (! engine.hasExpressionMidiInput())
            engine.setExpressionFromKnob (static_cast<float> (v));
    };

    monitorFlag = std::make_unique<FlagButton> (tr ("MONITOR"), true);
    monitorFlag->onClicked = [this] (bool on) { engine.setMonitorEnabled (on); };

    virtualFlag = std::make_unique<FlagButton> (tr ("TO CABLE"), true);
    virtualFlag->onClicked = [this] (bool on)
    {
        engine.setVirtualOutputEnabled (on);
        updateStatusText();
        persistSettings();
    };

    physicalMeter = std::make_unique<LevelMeter> (LevelMeter::Orientation::horizontal);
    physicalMeter->setLabel (tr ("MONITOR"));
    virtualMeter  = std::make_unique<LevelMeter> (LevelMeter::Orientation::horizontal);
    virtualMeter->setLabel (tr ("CABLE"));

    devicesButton = std::make_unique<SmallTextButton> (tr ("Devices"));
    devicesButton->onClick = [this] { showDeviceChooser(); };

    addAndMakeVisible (*dryWetKnob);
    addAndMakeVisible (*trimKnob);
    addAndMakeVisible (*expressionKnob);
    addAndMakeVisible (*monitorFlag);
    addAndMakeVisible (*virtualFlag);
    addAndMakeVisible (*physicalMeter);
    addAndMakeVisible (*virtualMeter);
    addAndMakeVisible (*devicesButton);

    /*  The chooser is a child that covers the whole window when it is up, so it is
        created and wired here and merely hidden; building it lazily would mean
        enumerating devices from inside a paint cycle. addChildComponent rather
        than addAndMakeVisible because it must not show itself before start-up has
        decided whether the user has to choose anything. */
    chooser = std::make_unique<DeviceChooserPanel> (engine);
    addChildComponent (*chooser);

    chooser->onAccepted = [this]
    {
        problemText = chooser->applySelections();

        hideDeviceChooser();

        // Whatever the state of the devices, the user has now been through this
        // panel, which is the fact the next launch looks for.
        engine.setDevicesChosen();

        // Everything the panel may have moved: devices, clock, and therefore the
        // latency figures and the add-effect availability.
        populateDeviceBoxes();
        populateRateBox();
        populateBufferBox();
        populateAddBox();
        refreshGlobalControls();
        updateStatusText();
        persistSettings();
    };

    chooser->onCancelled = [this]
    {
        hideDeviceChooser();

        engine.setDevicesChosen();

        if (! engine.isRunning())
            problemText = tr ("No audio device is running. Open Devices to pick an input and an output.");

        // The VIRTUAL OUT switch in the panel takes effect immediately, so the
        // top bar's lamp has to follow it even when the panel was closed without
        // an Apply; otherwise it shows the opposite of the router's real state.
        refreshGlobalControls();
        updateStatusText();
        persistSettings();
    };

    chooser->onLanguageChosen = [this] (Language language) { applyLanguage (language); };
}

void MainComponent::buildChainArea()
{
    viewport = std::make_unique<juce::Viewport>();
    viewport->setScrollBarsShown (true, false);
    viewport->setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);
    viewport->setScrollBarThickness (10);

    // The viewport deletes its content, so the raw pointer below is only ever an
    // observer: ownership stays on the other side of setViewedComponent().
    auto content = std::make_unique<ChainList>();
    chainList = content.get();
    viewport->setViewedComponent (content.release(), true);

    addAndMakeVisible (*viewport);
}

//==============================================================================
//  Device selection
//==============================================================================
void MainComponent::populateDeviceBoxes()
{
    const juce::ScopedValueSetter<bool> suppress (suppressBoxCallbacks, true);

    auto fillTypes = [this] (juce::ComboBox& box, bool wantInputs)
    {
        /*  A ComboBox reports no text at all until something has been selected, so
            keeping box.getText() alone means the first populate ends with a full
            list and no selection - four empty selectors above a running stream. The
            engine knows which type it actually opened, so that is the fallback. */
        const auto keep = box.getText().isNotEmpty()
                            ? box.getText()
                            : (wantInputs ? engine.getInputDeviceType() : engine.getOutputDeviceType());

        box.clear (juce::dontSendNotification);

        const auto types = engine.listDeviceTypes (wantInputs, ! wantInputs);

        for (int i = 0; i < types.size(); ++i)
            box.addItem (types[i], i + 1);

        const auto index = types.indexOf (keep);
        box.setSelectedId (index >= 0 ? index + 1 : 0, juce::dontSendNotification);
    };

    fillTypes (*inputTypeBox, true);
    fillTypes (*outputTypeBox, false);

    populateNameBox (*inputDeviceBox,
                     engine.listDevicesOfType (inputTypeBox->getText(), true),
                     engine.getInputDeviceName());

    populateNameBox (*outputDeviceBox,
                     engine.listDevicesOfType (outputTypeBox->getText(), false),
                     engine.getOutputDeviceName());
}

void MainComponent::populateNameBox (juce::ComboBox& box, const juce::StringArray& names,
                                     const juce::String& selectedName)
{
    box.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        box.addItem (names[i], i + 1);

    // A blank selector is never the useful answer: when the remembered name does
    // not belong to this device type (a type change just cleared it, or the device
    // was unplugged), fall back to the first device the type reports.
    const auto index = names.indexOf (selectedName);
    box.setSelectedId (index >= 0 ? index + 1 : (names.isEmpty() ? 0 : 1),
                       juce::dontSendNotification);
}

void MainComponent::populateRateBox()
{
    const juce::ScopedValueSetter<bool> suppress (suppressBoxCallbacks, true);

    availableRates = engine.getAvailableSampleRates();
    const auto current = engine.getSampleRate();

    sampleRateBox->clear (juce::dontSendNotification);

    for (int i = 0; i < availableRates.size(); ++i)
        sampleRateBox->addItem (juce::String (availableRates[i], 0), i + 1);

    auto bestIndex = -1;
    auto bestDistance = 1.0e9;

    for (int i = 0; i < availableRates.size(); ++i)
    {
        const auto distance = std::abs (availableRates[i] - current);

        if (distance < bestDistance)
        {
            bestDistance = distance;
            bestIndex = i;
        }
    }

    sampleRateBox->setSelectedId (bestIndex >= 0 ? bestIndex + 1 : 0, juce::dontSendNotification);
}

void MainComponent::populateBufferBox()
{
    const juce::ScopedValueSetter<bool> suppress (suppressBoxCallbacks, true);

    availableBuffers = engine.getAvailableBufferSizes();
    const auto current = engine.getBufferSizeSamples();
    const auto rate = juce::jmax (1.0, engine.getSampleRate());

    bufferBox->clear (juce::dontSendNotification);

    for (int i = 0; i < availableBuffers.size(); ++i)
    {
        const auto milliseconds = static_cast<double> (availableBuffers[i]) / rate * 1000.0;
        bufferBox->addItem (juce::String (availableBuffers[i]) + " ("
                            + juce::String (milliseconds, 1) + " ms)", i + 1);
    }

    const auto index = availableBuffers.indexOf (current);
    bufferBox->setSelectedId (index >= 0 ? index + 1 : 0, juce::dontSendNotification);
}

void MainComponent::populateAddBox()
{
    const juce::ScopedValueSetter<bool> suppress (suppressBoxCallbacks, true);

    addBox->clear (juce::dontSendNotification);

    const auto chainFull = engine.getNumSlots() >= maxChainSlots;

    // Grouped by category because two dozen undifferentiated names is not a menu, it is
    // a wall of text. Section headings take no item id, so they cannot be picked.
    for (auto* category : EffectFactory::categories())
    {
        addBox->addSectionHeading (tr (category));

        for (auto& metadata : EffectFactory::metadata())
        {
            if (juce::String (metadata.category) != juce::String (category))
                continue;

            // Item ids are type + 1: see the promptId note above the anonymous namespace.
            const auto id = metadata.type + 1;
            addBox->addItem (tr (metadata.name), id);
            addBox->setItemEnabled (id, ! chainFull
                                          && ! engine.isTypeInUse (static_cast<EffectTypeID> (metadata.type)));
        }
    }
}

void MainComponent::inputTypeChanged()
{
    if (suppressBoxCallbacks)
        return;

    const juce::ScopedValueSetter<bool> suppress (suppressBoxCallbacks, true);

    populateNameBox (*inputDeviceBox,
                     engine.listDevicesOfType (inputTypeBox->getText(), true), {});
}

void MainComponent::inputDeviceChanged()
{
    if (suppressBoxCallbacks)
        return;

    problemText = engine.setInputDevice (inputTypeBox->getText(), inputDeviceBox->getText());

    // A new input can bring a different set of clocks with it.
    populateRateBox();
    populateBufferBox();
    refreshGlobalControls();
    updateStatusText();
    persistSettings();
}

void MainComponent::outputTypeChanged()
{
    if (suppressBoxCallbacks)
        return;

    const juce::ScopedValueSetter<bool> suppress (suppressBoxCallbacks, true);

    populateNameBox (*outputDeviceBox,
                     engine.listDevicesOfType (outputTypeBox->getText(), false), {});
}

void MainComponent::outputDeviceChanged()
{
    if (suppressBoxCallbacks)
        return;

    problemText = engine.setOutputDevice (outputTypeBox->getText(), outputDeviceBox->getText());

    populateRateBox();
    populateBufferBox();
    refreshGlobalControls();
    updateStatusText();
    persistSettings();
}

void MainComponent::bufferChanged()
{
    if (suppressBoxCallbacks)
        return;

    const auto index = bufferBox->getSelectedItemIndex();

    if (index >= 0 && index < availableBuffers.size())
        problemText = engine.setBufferSize (availableBuffers[index]);

    populateBufferBox();
    updateStatusText();
    persistSettings();
}

void MainComponent::sampleRateChanged()
{
    if (suppressBoxCallbacks)
        return;

    const auto index = sampleRateBox->getSelectedItemIndex();

    if (index >= 0 && index < availableRates.size())
        problemText = engine.setSampleRate (availableRates[index]);

    populateRateBox();
    populateBufferBox();
    updateStatusText();
    persistSettings();
}

void MainComponent::addEffectChanged()
{
    if (suppressBoxCallbacks)
        return;

    const auto id = addBox->getSelectedId();
    addBox->clear (juce::dontSendNotification);

    if (id <= promptId || id > numEffectTypes)
        return;

    if (! engine.addEffect (static_cast<EffectTypeID> (id - 1)))
    {
        problemText = trf ("That effect cannot be added: the chain holds %1 slots and no type may appear twice.",
                           maxChainSlots);

        // The box was cleared before the attempt, so without this the menu stays
        // empty until some other change rebuilds it.
        populateAddBox();
        updateStatusText();
        return;
    }

    // Deferred, exactly as for removal: the box is still inside its own onChange.
    pendingRebuild = true;
    revealLastCard = true;
    persistSettings();
}

void MainComponent::refreshGlobalControls()
{
    monitorFlag->setOn (engine.isMonitorEnabled());
    virtualFlag->setOn (engine.getVirtualRouter().isEnabled());

    dryWetKnob->setValue (engine.getGlobalWet());
    trimKnob->setValue (engine.getInputTrimDb());
    expressionKnob->setValue (engine.getExpressionPosition());
}

//==============================================================================
//  Chain
//==============================================================================
void MainComponent::rebuildCards()
{
    if (chainList != nullptr)
    {
        for (auto& card : cards)
            chainList->removeChildComponent (card.get());
    }

    cards.clear();

    std::vector<EffectCard*> raw;
    raw.reserve (static_cast<std::size_t> (juce::jmax (0, engine.getNumSlots())));

    const auto count = engine.getNumSlots();

    for (int i = 0; i < count; ++i)
    {
        auto* effect = engine.getSlot (i);

        if (effect == nullptr)
            continue;

        auto card = std::make_unique<EffectCard> (engine, *effect, i);

        card->onRequestRemove = [this] (EffectCard* target) { removeCard (target); };
        card->onRequestMove   = [this] (EffectCard* target, int direction) { moveCard (target, direction); };

        // The card fires this for the press and for every drag movement, so the
        // first call opens the gesture and the rest update it.
        card->onGripDrag = [this] (EffectCard* target, const juce::MouseEvent& e)
        {
            if (draggingCard == nullptr)
                startCardDrag (*target, e);
            else
                updateCardDrag (*target, e);
        };

        card->refresh();
        raw.push_back (card.get());
        cards.push_back (std::move (card));
    }

    for (auto* card : raw)
        chainList->addAndMakeVisible (*card);

    chainList->setCards (std::move (raw));
    chainList->layoutCards();

    if (revealLastCard && viewport != nullptr && ! cards.empty())
    {
        auto* last = cards.back().get();
        const auto maxY = juce::jmax (0, chainList->getHeight() - viewport->getViewHeight());
        const auto y = juce::jlimit (0, maxY, last->getY());
        viewport->setViewPosition (0, y);
    }

    revealLastCard = false;

    draggingCard = nullptr;
    dragFromIndex = -1;
}

void MainComponent::removeCard (EffectCard* card)
{
    if (card == nullptr)
        return;

    if (! engine.removeSlot (card->getSlotIndex()))
        return;

    // The button that got us here is a child of the card that has to die, so the
    // deletion waits for the next timer tick instead of happening inside the click.
    pendingRebuild = true;
    persistSettings();
}

void MainComponent::moveCard (EffectCard* card, int direction)
{
    if (card == nullptr)
        return;

    const auto from = card->getSlotIndex();
    const auto to = from + direction;

    if (to < 0 || to >= engine.getNumSlots())
        return;

    if (engine.moveSlot (from, to))
    {
        pendingRebuild = true;
        persistSettings();
    }
}

void MainComponent::startCardDrag (EffectCard& card, const juce::MouseEvent& e)
{
    draggingCard = &card;
    dragFromIndex = card.getSlotIndex();
    card.setBeingDragged (true);
    chainList->setDropGap (dragFromIndex);
    updateCardDrag (card, e);
}

void MainComponent::updateCardDrag (EffectCard& card, const juce::MouseEvent& e)
{
    if (draggingCard != &card || chainList == nullptr)
        return;

    const auto inWindow = e.getEventRelativeTo (this).getPosition();
    const auto inChain = chainList->getLocalPoint (this, inWindow);

    const auto gap = chainList->gapForY (inChain.getY());

    if (gap != chainList->getDropGap())
    {
        chainList->setDropGap (gap);

        // Exactly one card lights up: the one whose body the pointer is over.
        auto* over = chainList->cardForGap (gap);

        for (auto& other : cards)
            other->setDropTarget (other.get() == over);
    }
}

void MainComponent::finishCardDrag()
{
    if (draggingCard == nullptr)
        return;

    const auto gap = chainList != nullptr ? chainList->getDropGap() : -1;
    const auto from = dragFromIndex;

    for (auto& card : cards)
    {
        card->setDropTarget (false);
        card->setBeingDragged (false);
    }

    if (chainList != nullptr)
        chainList->setDropGap (-1);

    draggingCard = nullptr;
    dragFromIndex = -1;

    if (gap < 0)
        return;

    // Converting an insertion gap into a destination index: dropping below the
    // card's own position shifts everything up by one when the old slot is removed.
    const auto to = gap > from ? gap - 1 : gap;

    if (to != from && to < engine.getNumSlots())
    {
        engine.moveSlot (from, to);
        pendingRebuild = true;
        persistSettings();
    }
}

void MainComponent::mouseUp (const juce::MouseEvent&)
{
    // Registered globally, so this also fires for every click in the app. Only the
    // release that ends a card drag does anything.
    if (draggingCard != nullptr)
        finishCardDrag();
}

//==============================================================================
//  Status
//==============================================================================
void MainComponent::timerCallback()
{
    physicalMeter->setPeak (engine.getPhysicalPeak());
    virtualMeter->setPeak (engine.getVirtualPeak());

    if (engine.hasExpressionMidiInput())
    {
        expressionKnob->setActive (false);
        expressionKnob->setValue (engine.getExpressionPosition());
    }
    else
    {
        expressionKnob->setActive (true);
    }

    if (pendingRebuild)
    {
        pendingRebuild = false;
        rebuildCards();
        populateAddBox();
    }

    if (++statusTickCount >= statusRefreshDivisor)
    {
        statusTickCount = 0;
        updateStatusText();

        // Looper transport, cabinet IR name, xrun counts: the card footers are the
        // only place those show, and they are read from atomics so this is cheap.
        for (auto& card : cards)
            card->refresh();
    }
}

void MainComponent::updateStatusText()
{
    auto& router = engine.getVirtualRouter();
    const auto cableStatus = router.getStatus();

    statusLines.clear();

    /*  The input peak stays live with monitoring off, which is the only way to
        tell "the guitar is connected but silent" from "the guitar never arrived"
        without opening the device panel. */
    const auto inputPeak = engine.getInputPeak();
    const auto inputDb = inputPeak > 1.0e-6f
                           ? juce::jlimit (-120.0f, 6.0f, gainToDb (inputPeak))
                           : -120.0f;

    statusLines.add (trf ("Input peak %1 dBFS   %2",
                          juce::String (inputDb, 1),
                          router.getStatusLine()));

    /*  Whole sentences through trf rather than word-by-word concatenation: the
        labels in a status line are not the same words in the same order in every
        language, so "Latency" + value + "monitor" + value cannot be translated by
        translating the pieces. */
    auto latency = trf ("Latency  input %1   monitor %2   chain %3",
                        ms (engine.getInputLatencyMs()),
                        ms (engine.getOutputLatencyMs()),
                        ms (engine.getChainLatencyMs()));

    if (engine.getLatencyCompensationSaturated())
        latency += tr (" (dry compensation at its limit)");

    latency += trf ("   virtual +%1", ms (engine.getVirtualLatencyMs()));

    if (cableStatus.deviceType.isNotEmpty())
        latency += "   [" + cableStatus.deviceType + "]";

    statusLines.add (latency);

    const auto load = trf ("CPU %1%   sample rate %2 Hz   block %3   slots %4/%5   dropped blocks  input %6 (over %7 / under %8)  virtual %9 (over %10 / under %11)",
                           juce::String (static_cast<double> (engine.getCpuUsage()), 1),
                           juce::String (engine.getSampleRate(), 0),
                           engine.getBufferSizeSamples(),
                           engine.getNumSlots(),
                           maxChainSlots,
                           engine.getInputDrops(),
                           engine.getInputOverflows(),
                           engine.getInputUnderflows(),
                           engine.getVirtualDrops(),
                           engine.getVirtualOverflows(),
                           engine.getVirtualUnderflows());

    statusLines.add (load);

    /*  The last line is the "do something about this" line. A monitor path that
        stopped after start-up outranks whatever problem text is left over from
        start-up, and it is derived from live state rather than stored, so it
        disappears by itself when the device comes back instead of lingering as a
        stale complaint. */
    const auto problemLine = engine.isRunning()
                                ? problemText
                                : tr ("The monitoring path is not running. Open Devices and apply an output device.");

    if (problemLine.isNotEmpty())
        statusLines.add (problemLine);

    // The strip is painted by this component, and no child repaint ever covers
    // it: without this the CPU / latency / drop line stayed at its start-up
    // values until something else forced a full repaint.
    repaint (getLocalBounds().removeFromBottom (statusBarHeight));
}

//==============================================================================
//  Device chooser
//==============================================================================
void MainComponent::showDeviceChooser()
{
    chooser->populate();
    chooser->refreshCableStatus();

    // The chooser is created in buildGlobalControls(), which runs before the chain
    // viewport exists, so it is *not* the last child: the cards would paint over its
    // lower half (measured on the first run - the effect cards covered everything
    // below the panel's intro text). Moving it to the front makes the stacking a
    // stated fact instead of an accident of construction order.
    chooser->setBounds (getLocalBounds());
    chooser->toFront (false);
    chooser->setVisible (true);
}

void MainComponent::hideDeviceChooser()
{
    chooser->setVisible (false);
}

//==============================================================================
//  Language
//==============================================================================
void MainComponent::applyLanguage (Language newLanguage)
{
    if (newLanguage == currentLanguage() && languageWasChosen())
        return;

    setLanguage (newLanguage);
    noteLanguageChosen();
    engine.setStoredLanguage (static_cast<int> (newLanguage));

    /*  The cards are rebuilt rather than patched. A Knob or a DiscreteSelector
        takes its caption at construction, and rebuildCards() is the path that add
        and remove already use, so there is one way for a card to pick up new text
        instead of two. It does not recreate any effect - the engine's chain is
        what it reads - so no device is touched and nothing audible happens. */
    pendingRebuild = true;

    refreshAllText();
    persistSettings();
}

void MainComponent::refreshAllText()
{
    dryWetKnob->setLabel (tr ("Dry/Wet"));
    trimKnob->setLabel (tr ("Input Trim"));
    expressionKnob->setLabel (tr ("Expression"));

    monitorFlag->setLabelText (tr ("MONITOR"));
    virtualFlag->setLabelText (tr ("TO CABLE"));

    physicalMeter->setLabel (tr ("MONITOR"));
    virtualMeter->setLabel (tr ("CABLE"));

    devicesButton->setButtonText (tr ("Devices"));

    // The add box holds translated category headings and effect names.
    addBox->setTextWhenNothingSelected (tr ("Add effect..."));
    populateAddBox();

    chooser->refreshText();

    // The native title bar is outside this component's paint(), so it needs the
    // same text pushed to it as everything else.
    if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        window->setName (tr ("PedalBoard - guitar chain, monitor and virtual cable output"));

    updateStatusText();
    repaint();
}

void MainComponent::showLanguagePrompt()
{
    /*  Both scripts in one message: the question cannot be asked in a language
        the user has not chosen yet. The two buttons are each monolingual on
        purpose - a Chinese speaker reading "中文" needs nothing else.

        AlertWindow's result codes, from juce_AlertWindow.h: with two buttons the
        first returns 1 and the second returns 0, and the escape key or the close
        box also yields 0, which is English - the language this app was written
        in, and the safe answer to a question that went unanswered. */
    const juce::String question = u8 ("Choose the language for the interface.\n"
                                       "请选择界面语言。\n\n"
                                       "This can be changed later in the Audio devices panel.\n"
                                       "之后可在「音频设备」面板中更改。");

    juce::AlertWindow::showAsync (juce::MessageBoxOptions()
                                      .withIconType (juce::AlertWindow::NoIcon)
                                      .withTitle ("PedalBoard")
                                      .withMessage (question)
                                      .withButton (u8 ("中文"))
                                      .withButton ("English")
                                      .withAssociatedComponent (this),
                                  [this] (int result)
                                  {
                                      applyLanguage (result == 1 ? Language::chinese
                                                                 : Language::english);

                                      if (wantsDeviceChooser)
                                          showDeviceChooser();
                                  });
}

//==============================================================================
//  Layout and painting
//==============================================================================
MainComponent::RowOnePositions MainComponent::rowOnePositions() noexcept
{
    RowOnePositions r;
    auto x = edgeMargin;

    r.inputType    = x;  x += inputTypeWidth  + boxGap;
    r.inputDevice  = x;  x += deviceNameWidth + 18;   // a wider gap splits input from output
    r.outputType   = x;  x += inputTypeWidth  + boxGap;
    r.outputDevice = x;  x += deviceNameWidth + 18;
    r.buffer       = x;  x += bufferBoxWidth  + boxGap;
    r.rate         = x;  x += rateBoxWidth    + boxGap;
    r.devices      = x;

    return r;
}

void MainComponent::resized()
{
    auto area = getLocalBounds();

    // Top bar and status bar are painted strips; whatever is left is the chain.
    area.removeFromBottom (statusBarHeight);
    area.removeFromTop (topBarHeight);

    // The "SIGNAL CHAIN" caption is painted in its own strip at the top of the
    // remaining area; the viewport starts below it instead of over it.
    area.removeFromTop (18);
    const auto chainArea = area;

    const auto row1 = rowOnePositions();
    const auto boxY = rowOneY + 18;
    const auto row2Y = rowTwoY;

    inputTypeBox->setBounds    (row1.inputType,    boxY, inputTypeWidth,  comboHeight);
    inputDeviceBox->setBounds  (row1.inputDevice,  boxY, deviceNameWidth, comboHeight);
    outputTypeBox->setBounds   (row1.outputType,   boxY, inputTypeWidth,  comboHeight);
    outputDeviceBox->setBounds (row1.outputDevice, boxY, deviceNameWidth, comboHeight);
    bufferBox->setBounds       (row1.buffer,       boxY, bufferBoxWidth,  comboHeight);
    sampleRateBox->setBounds   (row1.rate,         boxY, rateBoxWidth,    comboHeight);
    devicesButton->setBounds   (row1.devices,      boxY - 1, devicesWidth, comboHeight + 2);

    // Row 2 walks a cursor: switches, knobs, meters, then the add-effect box.
    auto x = edgeMargin;

    monitorFlag->setBounds (x, row2Y, flagWidth, flagHeight);
    x += flagWidth + boxGap;
    virtualFlag->setBounds (x, row2Y, flagWidth, flagHeight);
    x += flagWidth + 14;

    dryWetKnob->setBounds     (x, row2Y - 5, knobWidth, rowTwoHeight + 10);
    x += knobWidth + 4;
    trimKnob->setBounds       (x, row2Y - 5, knobWidth, rowTwoHeight + 10);
    x += knobWidth + 4;
    expressionKnob->setBounds (x, row2Y - 5, knobWidth, rowTwoHeight + 10);
    x += knobWidth + 12;

    physicalMeter->setBounds (x, row2Y + 5, meterWidth, meterHeight);
    x += meterWidth + 8;
    virtualMeter->setBounds  (x, row2Y + 5, meterWidth, meterHeight);
    x += meterWidth + 12;

    addBox->setBounds (x, row2Y + 6, addBoxWidth, comboHeight);

    viewport->setBounds (chainArea.reduced (4, 2));

    if (chainList != nullptr)
    {
        // The content's width has to come from the viewport, and its height from
        // the cards: the order below is what keeps the vertical scrollbar steady.
        chainList->setSize (viewport->getMaximumVisibleWidth(), chainList->getHeight());
        chainList->layoutCards();
    }

    chooser->setBounds (getLocalBounds());
}

void MainComponent::paint (juce::Graphics& g)
{
    auto& laf = DarkLookAndFeel::instance();
    // Not const: removeFromTop and removeFromBottom mutate the rectangle, and the
    // second call has to see the first one's remainder.
    auto bounds = getLocalBounds();

    g.fillAll (laf.windowBackground);

    const auto topArea = bounds.removeFromTop (topBarHeight);
    const auto statusArea = bounds.removeFromBottom (statusBarHeight);

    g.setColour (laf.panelBackground);
    g.fillRoundedRectangle (topArea.reduced (4, 4).toFloat(), 6.0f);
    g.fillRoundedRectangle (statusArea.reduced (4, 2).toFloat(), 6.0f);

    // Row 1 captions sit directly above each box, read from the same position
    // table resized() uses to place the boxes.
    const auto row1 = rowOnePositions();

    g.setFont (DarkLookAndFeel::makeFont (10.0f));
    g.setColour (laf.textDim);

    auto caption = [this, &g] (const juce::String& text, int x, int width)
    {
        g.drawText (text, juce::Rectangle<int> (x, rowOneY - 2, width, 16),
                    juce::Justification::centredLeft, false);
    };

    caption (tr ("IN TYPE"),        row1.inputType,    inputTypeWidth);
    caption (tr ("INPUT DEVICE"),   row1.inputDevice,  deviceNameWidth);
    caption (tr ("OUT TYPE"),       row1.outputType,   inputTypeWidth);
    caption (tr ("MONITOR DEVICE"), row1.outputDevice, deviceNameWidth);
    caption (tr ("BUFFER"),         row1.buffer,       bufferBoxWidth);
    caption (tr ("SAMPLE RATE"),    row1.rate,         rateBoxWidth);

    // The chain's own label, so an empty board is not mistaken for a crash.
    g.setFont (DarkLookAndFeel::makeFont (11.0f));
    g.setColour (laf.textDim);
    g.drawText (tr ("SIGNAL CHAIN  (drag a card by its header to reorder)"),
                juce::Rectangle<int> (edgeMargin + 6, topBarHeight, getWidth() - 2 * edgeMargin - 12, 16),
                juce::Justification::centredLeft, false);

    if (engine.getNumSlots() == 0)
    {
        g.setFont (DarkLookAndFeel::makeFont (15.0f));
        g.setColour (laf.textDim);

        // Two keys, joined here: LocalisedStrings reads its table one line at a
        // time, so a key containing a newline could never be stored.
        g.drawMultiLineText (tr ("The chain is empty.") + "\n"
                             + tr ("Pick an effect from the Add effect box at the top right."),
                             getWidth() / 2 - 200, topBarHeight + 90, 400,
                             juce::Justification::centred, 6.0f);
    }

    // Status lines: the cable state first, then latency, then load, then any
    // problem text - which is the only line allowed to be orange.
    const auto font = DarkLookAndFeel::makeFont (11.0f);
    g.setFont (font);

    auto lineY = statusArea.getY() + 3;

    for (int i = 0; i < statusLines.size(); ++i)
    {
        const auto isProblem = i == statusLines.size() - 1 && problemText.isNotEmpty();

        g.setColour (i == 0 ? (engine.getVirtualRouter().getStatus().availability
                                 == VBCableRouter::Availability::running ? laf.good : laf.textPrimary)
                            : (isProblem ? laf.warning : laf.textDim));

        g.drawText (statusLines[i],
                    juce::Rectangle<int> (edgeMargin + 6, lineY, getWidth() - 2 * edgeMargin - 12,
                                          statusLineHeight),
                    juce::Justification::centredLeft, true);

        lineY += statusLineHeight;
    }
}

} // namespace pedal
