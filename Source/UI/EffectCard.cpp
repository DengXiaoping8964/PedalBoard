#include "UI/EffectCard.h"

#include <algorithm>

namespace pedal
{

namespace
{
    /** Badge text: "01" .. "16", so a column of cards reads like a board and the
        numbers line up in a stack. Built here rather than in paint() because the
        card repaints far more often than it changes slot. */
    juce::String badgeFor (int slotIndex)
    {
        return juce::String (slotIndex + 1).paddedLeft ('0', 2);
    }

    /*  30 was already too narrow for "Down" - the first screenshot of the running
        app showed "Do.." - and 下移 is two full-width glyphs at the header's
        button font, so the box has to grow rather than the text shrink. The
        title's available width is derived from this constant, so it follows. */
    constexpr int moveButtonWidth = 42;
    constexpr int removeButtonWidth = 26;
    constexpr int flagWidth = 52;
    constexpr int flagHeight = 20;
    constexpr int headerGap = 4;
}

//==============================================================================
EffectCard::EffectCard (AudioEngine& engineToUse, PedalEffect& effectToDisplay, int slotIndex)
    : engine (engineToUse),
      effect (effectToDisplay),
      slot (slotIndex)
{
    setRepaintsOnMouseActivity (true);

    badgeText = badgeFor (slot);
    knobRows = (effect.getNumParameters() + maxKnobsPerRow - 1) / maxKnobsPerRow;

    //==============================================================================
    //  Header children. These are created before the controls so they sit at the
    //  top of the z-order list, which is what makes them win the mouse over the
    //  card body.
    bypassFlag = std::make_unique<FlagButton> (tr ("ON"), true);
    bypassFlag->onClicked = [this] (bool engaged)
    {
        // The lamp means "in circuit", the atomic means "bypassed": one negation,
        // here, and refresh() re-reads the truth so the two can never drift.
        effect.setBypassed (! engaged);
        refresh();
    };
    addAndMakeVisible (*bypassFlag);

    moveUp = std::make_unique<SmallTextButton> (tr ("Up"));
    moveUp->onClick = [this] { if (onRequestMove) onRequestMove (this, -1); };
    addAndMakeVisible (*moveUp);

    moveDown = std::make_unique<SmallTextButton> (tr ("Down"));
    moveDown->onClick = [this] { if (onRequestMove) onRequestMove (this, +1); };
    addAndMakeVisible (*moveDown);

    removeButton = std::make_unique<SmallTextButton> ("X");
    removeButton->onClick = [this] { if (onRequestRemove) onRequestRemove (this); };
    addAndMakeVisible (*removeButton);

    //==============================================================================
    buildControls();
    buildActionButtons();

    // One pull straight away: the card may be created after the settings file has
    // restored values, and it must show them, not the defaults.
    refresh();
}

EffectCard::~EffectCard() = default;

//==============================================================================
void EffectCard::buildControls()
{
    const auto numParams = effect.getNumParameters();
    const auto& descriptors = effect.getParameters();

    // Indexed by parameter id with a null in whichever column does not apply, so
    // refresh() can walk the two arrays in lockstep without a type tag per control.
    knobs.assign (static_cast<std::size_t> (numParams), nullptr);
    selectors.assign (static_cast<std::size_t> (numParams), nullptr);
    controls.clear();
    controls.reserve (static_cast<std::size_t> (numParams));

    for (int i = 0; i < numParams; ++i)
    {
        // A copy, not a reference into the descriptor vector: the lambda below
        // outlives this loop, and the copy holds only const char* pointers to
        // static storage, so it costs nothing and cannot dangle.
        const ParamDescription descriptor = descriptors[static_cast<std::size_t> (i)];

        if (descriptor.numSteps >= 2 && descriptor.stepLabels != nullptr)
        {
            auto selector = std::make_unique<DiscreteSelector> (tr (descriptor.name),
                                                                descriptor.numSteps,
                                                                descriptor.stepLabels);
            selector->onValueChange = [this, i] (double value)
            {
                effect.setParameter (i, static_cast<float> (value));
            };

            selectors[static_cast<std::size_t> (i)] = selector.get();
            addAndMakeVisible (*selector);
            controls.push_back (std::move (selector));
        }
        else
        {
            auto knob = std::make_unique<Knob> (tr (descriptor.name),
                                                descriptor.minValue, descriptor.maxValue,
                                                [descriptor] (double value)
                                                {
                                                    return describeParameterValue (descriptor,
                                                                                   static_cast<float> (value));
                                                });

            knob->setDoubleClickDefault (descriptor.defaultValue);
            knob->onValueChange = [this, i] (double value)
            {
                effect.setParameter (i, static_cast<float> (value));
            };

            knobs[static_cast<std::size_t> (i)] = knob.get();
            addAndMakeVisible (*knob);
            controls.push_back (std::move (knob));
        }
    }
}

void EffectCard::buildActionButtons()
{
    const auto available = effect.actions();
    hasActions = ! available.empty();

    for (auto& action : available)
    {
        auto button = std::make_unique<SmallTextButton> (tr (action.label));
        const auto id = action.id;

        button->onClick = [this, id]
        {
            // handleAction() may open a file chooser (the cabinet) or move the
            // looper transport; either way the card has to re-read its own state.
            effect.handleAction (id);
            refresh();
        };

        actionButtons.push_back (std::move (button));
    }

    for (auto& button : actionButtons)
        addAndMakeVisible (*button);
}

//==============================================================================
void EffectCard::setSlotIndex (int newIndex)
{
    if (slot == newIndex)
        return;

    slot = newIndex;
    badgeText = badgeFor (slot);
    updateMoveButtons();
    repaint (headerArea());
}

void EffectCard::setDropTarget (bool isDropTarget)
{
    if (dropTarget == isDropTarget)
        return;

    dropTarget = isDropTarget;
    repaint();
}

void EffectCard::setBeingDragged (bool isBeingDragged)
{
    if (beingDragged == isBeingDragged)
        return;

    beingDragged = isBeingDragged;
    repaint();
}

void EffectCard::refresh()
{
    const auto active = ! effect.isBypassed();
    const auto numParams = effect.getNumParameters();

    for (int i = 0; i < numParams; ++i)
    {
        const auto value = static_cast<double> (effect.getParameter (i));

        if (auto* knob = knobs[static_cast<std::size_t> (i)])
            knob->setValue (value);
        else if (auto* selector = selectors[static_cast<std::size_t> (i)])
            selector->setValue (value);
    }

    // setValue() suppresses the widget's own callback, so writing the value back
    // here cannot echo into the atomic and loop.
    bypassFlag->setOn (active);

    for (int i = 0; i < numParams; ++i)
    {
        if (auto* knob = knobs[static_cast<std::size_t> (i)])
            knob->setActive (active);
        if (auto* selector = selectors[static_cast<std::size_t> (i)])
            selector->setActive (active);
    }

    statusText = effect.getStatusText();
    warningText = effect.getWarningText();

    updateMoveButtons();
    repaint();
}

void EffectCard::updateMoveButtons()
{
    const auto count = engine.getNumSlots();
    moveUp->setEnabled (slot > 0 && slot < count);
    moveDown->setEnabled (slot >= 0 && slot + 1 < count);
    removeButton->setEnabled (true);
}

//==============================================================================
//  Drag grip. The card's own mouse callbacks only ever fire for a press on the
//  header itself: the flag, the three header buttons and every knob are child
//  components and consume their own clicks, so a press that reaches here is a
//  press on the grip by construction.
juce::Rectangle<int> EffectCard::headerArea() const noexcept
{
    return getLocalBounds().withHeight (headerHeight);
}

void EffectCard::mouseDown (const juce::MouseEvent& e)
{
    // Rectangle<int>::contains only accepts Point<int>; MouseEvent::position is
    // a Point<float>.
    if (! headerArea().contains (e.position.toInt()))
        return;

    draggingGrip = true;

    if (onGripDrag)
        onGripDrag (this, e);
}

void EffectCard::mouseDrag (const juce::MouseEvent& e)
{
    if (! draggingGrip)
        return;

    if (onGripDrag)
        onGripDrag (this, e);
}

void EffectCard::mouseUp (const juce::MouseEvent&)
{
    // The gesture itself ends in MainComponent, which owns the mouse listener that
    // sees the release wherever it lands and then rebuilds the board. Nothing here
    // may delete this card, because this code is still running inside its gesture.
    draggingGrip = false;
}

//==============================================================================
int EffectCard::getPreferredHeight (const PedalEffect& target)
{
    const auto rows = (target.getNumParameters() + maxKnobsPerRow - 1) / maxKnobsPerRow;

    // The footer is reserved whether or not there is anything to say in it, so a
    // warning that appears later (no IR loaded, tape full) cannot shove the card
    // below it out from under the pointer.
    return headerHeight
         + juce::jmax (1, rows) * knobRowHeight
         + (target.actions().empty() ? 0 : actionsHeight)
         + footerHeight;
}

//==============================================================================
void EffectCard::resized()
{
    auto area = getLocalBounds();

    const auto header = area.removeFromTop (headerHeight);

    // Right to left across the header: X, Down, Up, then the ON lamp.
    auto right = header.getRight() - edgeMargin;
    removeButton->setBounds (right - removeButtonWidth, 3, removeButtonWidth, headerHeight - 6);
    right -= removeButtonWidth + headerGap;
    moveDown->setBounds (right - moveButtonWidth, 3, moveButtonWidth, headerHeight - 6);
    right -= moveButtonWidth + headerGap;
    moveUp->setBounds (right - moveButtonWidth, 3, moveButtonWidth, headerHeight - 6);
    right -= moveButtonWidth + headerGap + 2;

    bypassFlag->setBounds (right - flagWidth, (headerHeight - flagHeight) / 2, flagWidth, flagHeight);

    // The controls fill the row band, laid out by the order they were created in.
    auto body = area.withHeight (juce::jmax (1, knobRows) * knobRowHeight);
    const auto perRow = static_cast<std::size_t> (maxKnobsPerRow);
    const auto total = static_cast<std::size_t> (effect.getNumParameters());

    for (std::size_t i = 0; i < total && i < controls.size(); ++i)
    {
        const auto row = i / perRow;
        const auto column = i % perRow;

        // How many share this row: the last row is usually short and its knobs
        // get the whole remaining width rather than a quarter each.
        const auto remaining = total - row * perRow;
        const auto inThisRow = static_cast<int> (juce::jmin (perRow, remaining));

        const auto rowTop = body.getY() + static_cast<int> (row) * knobRowHeight;
        const auto width = body.getWidth() / inThisRow;

        controls[i]->setBounds (body.getX() + static_cast<int> (column) * width,
                                rowTop + 2, width, knobRowHeight - 4);
    }

    /*  withY() keeps the original height, which would run the footer past the
        bottom of the card: the action row then overlaps it and the warning line
        is clipped away entirely. Rebuild the rect against the card's bottom. */
    auto rest = juce::Rectangle<int> (area.getX(), body.getBottom(), area.getWidth(),
                                      juce::jmax (0, area.getBottom() - body.getBottom()));

    if (hasActions && ! actionButtons.empty())
    {
        auto actions = rest.removeFromTop (actionsHeight).reduced (edgeMargin, 2);
        const auto gap = 4;
        const auto each = (actions.getWidth() - gap * (static_cast<int> (actionButtons.size()) - 1))
                        / static_cast<int> (actionButtons.size());
        auto x = actions.getX();

        for (auto& button : actionButtons)
        {
            button->setBounds (x, actions.getY(), juce::jmax (40, each), actions.getHeight());
            x += juce::jmax (40, each) + gap;
        }
    }

    // Footer: two lines, drawn in paint(), occupying whatever is left.
    footerBounds = rest;
}

//==============================================================================
void EffectCard::paint (juce::Graphics& g)
{
    auto& laf = DarkLookAndFeel::instance();
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
    const auto corner = 7.0f;

    const auto active = ! effect.isBypassed();

    g.setColour (dropTarget || beingDragged ? laf.panelHighlighted : laf.panelBackground);
    g.fillRoundedRectangle (bounds, corner);

    // The left edge is the state lamp at card scale: accent when in circuit, dim
    // when bypassed, and the warning colour while it is the drop target.
    g.setColour (dropTarget ? laf.accent
               : (active ? laf.accent : laf.accentDim));
    g.fillRect (juce::Rectangle<float> (bounds.getX(), bounds.getY(), 3.0f, bounds.getHeight()));

    if (beingDragged)
    {
        g.setColour (laf.windowBackground.withAlpha (0.35f));
        g.fillRoundedRectangle (bounds, corner);
    }

    g.setColour (dropTarget ? laf.accent : (active ? laf.panelHighlighted : laf.textDim.withAlpha (0.35f)));
    g.drawRoundedRectangle (bounds, corner, 1.0f);

    //==============================================================================
    //  Header strip.
    const auto header = headerArea();
    g.setColour (laf.panelHighlighted.withAlpha (0.45f));
    g.fillRect (header.withHeight (headerHeight - 1));

    const auto titleFont = DarkLookAndFeel::makeFont (13.0f).boldened();
    const auto smallFont = DarkLookAndFeel::makeFont (10.0f);

    g.setFont (smallFont);
    g.setColour (active ? laf.accent : laf.textDim);
    g.drawText (badgeText, juce::Rectangle<int> (edgeMargin + 2, 0, 24, headerHeight),
                juce::Justification::centredLeft, false);

    g.setFont (titleFont);
    g.setColour (active ? laf.textPrimary : laf.textDim);

    // The title is stored in English by the effect, which is also the key into
    // the table; statusText and warningText are already translated because the
    // effects that compose them know the sentence, this card does not.
    g.drawText (tr (effect.getTitle()),
                juce::Rectangle<int> (edgeMargin + 26, 0,
                                      header.getWidth() - (edgeMargin + 26 + flagWidth
                                                           + 3 * moveButtonWidth + removeButtonWidth
                                                           + 4 * headerGap + 6),
                                      headerHeight),
                juce::Justification::centredLeft, true);

    //==============================================================================
    //  Footer: status first, then the warning in the colour that means "act".
    const auto font = DarkLookAndFeel::makeFont (10.0f);
    g.setFont (font);

    auto footer = footerBounds.reduced (edgeMargin, 1);

    if (! statusText.isEmpty())
    {
        g.setColour (laf.textDim);
        g.drawText (statusText, footer.withHeight (footer.getHeight() / 2),
                    juce::Justification::centredLeft, true);
    }

    const auto warningArea = footer.withY (footer.getY() + footer.getHeight() / 2)
                                  .withHeight (footer.getHeight() - footer.getHeight() / 2);

    if (! warningText.isEmpty())
    {
        g.setColour (laf.warning);
        g.drawText (warningText, warningArea, juce::Justification::centredLeft, true);
    }
    else if (effect.isBypassed())
    {
        // The bypass state has to be legible without hunting for the lamp.
        g.setColour (laf.textDim);
        g.drawText (tr ("bypassed - the dry signal passes through"), warningArea,
                    juce::Justification::centredLeft, true);
    }
}

} // namespace pedal
