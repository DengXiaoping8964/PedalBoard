#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_basics/juce_audio_basics.h>

#include "Common/PedalLanguage.h"
#include "UI/DarkLookAndFeel.h"

#include <cmath>

/*==============================================================================
  PedalBoard - the custom widgets an effect card is built from.

  Header-only on purpose: the CMake target lists Source/UI/DarkLookAndFeel.cpp but
  no PedalWidgets.cpp, so every method here is inline and adds no build file.

  Design constraints that shaped these classes:

    * None of these widgets derive from juce::Slider. The obvious Knob design puts a
      hidden Slider child inside a custom Component and paints the arc behind it, but
      JUCE 8 has no supported way to make a Slider stop drawing entirely: the rotary
      arc and thumb are always produced by LookAndFeel::drawRotarySlider, and to hide
      them you have to install a second, private LookAndFeel on the child that no-ops
      that method. That private-LookAndFeel dance is fragile (focus ring, tooltip and
      child z-order all interact with it) and impossible to eyeball without a compiler.
      So the Knob owns its own drag maths directly in mouseDown/mouseDrag. This is the
      alternative the brief explicitly sanctions. The same reasoning applies to
      DiscreteSelector.
    * Programmatic value writes must never echo back through onValueChange, or the
      engine write -> widget -> engine write loop fires forever. The suppressCallback
      flag guards exactly that: it is set around every setValue()/setValueInternal call
      made by the card, and cleared before any user gesture is forwarded.
    * paint() must not heap-allocate beyond what drawText fundamentally needs. The
      discrete labels are copied into a StringArray in the constructor so the selected
      label is a const String& at repaint time, not a fresh build.
==============================================================================*/
namespace pedal
{

//==============================================================================
/**
    A rotary parameter control: label on top, arc in the middle, formatted value below.

    Drag mapping (documented, since it replaces the Slider we did not use):
      - vertical drag up increases the value, 300 device pixels covers the full range;
      - holding Shift divides the sensitivity by 5 for fine adjustment;
      - if setRange() supplied a non-zero interval the value is snapped to it;
      - a double click returns the control to the default set by
        setDoubleClickDefault();
      - the left/down and right/up arrow keys move one interval (or 1% of the range
        when continuous) so the control is usable from the keyboard once focused.
*/
class Knob : public juce::Component
{
public:
    using TextFormat = std::function<juce::String (double)>;

    Knob (juce::String labelText, double minValue, double maxValue, TextFormat formatter)
        : label (std::move (labelText)),
          minValue (minValue),
          maxValue (maxValue),
          interval (0.0),
          value (minValue),
          defaultOnDoubleClick (minValue),
          format (std::move (formatter))
    {
        setWantsKeyboardFocus (true);
        setRepaintsOnMouseActivity (true);
    }

    ~Knob() override = default;

    //==============================================================================
    /** Programmatic write: never fires onValueChange (see the suppress note above). */
    void setValue (double newValue)
    {
        const juce::ScopedValueSetter<bool> suppress (suppressCallback, true);
        value = clamp (newValue);
        repaint();
    }

    double getValue() const { return value; }

    void setActive (bool shouldBeActive)
    {
        if (active != shouldBeActive)
        {
            active = shouldBeActive;
            repaint();
        }
    }

    void setRange (double lo, double hi, double stepInterval)
    {
        minValue  = lo;
        maxValue  = hi;
        interval  = stepInterval;
        value     = clamp (value);
        repaint();
    }

    void setDoubleClickDefault (double defaultValue) { defaultOnDoubleClick = clamp (defaultValue); }

    /** Replaces the caption. Only the interface language needs it: everything
        else about a knob is fixed when it is built. */
    void setLabel (juce::String newLabel) { label = std::move (newLabel); repaint(); }

    std::function<void (double)> onValueChange;   // fired on user edits only

    //==============================================================================
    void resized() override { repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto w = static_cast<float> (getWidth());
        const auto h = static_cast<float> (getHeight());

        // Layout: name occupies the top strip, the value the bottom, the dial between.
        // The label/value heights are integral because Graphics::drawText is only
        // confirmed here with a Rectangle<int> / (x,y,w,h) overload; the dial stays in
        // float space for the arc.
        const int labelH = static_cast<int> (juce::jlimit (9.0f, 16.0f, h * 0.20f));
        const int valueH = labelH;
        const juce::Rectangle<int> labelArea (0, 0, getWidth(), labelH);
        const juce::Rectangle<int> valueArea (0, getHeight() - valueH, getWidth(), valueH);

        auto& laf = DarkLookAndFeel::instance();
        const auto text  = active ? laf.textPrimary : laf.textDim;
        const auto dim   = laf.textDim;
        const auto accent = active ? laf.accent : laf.accentDim;

        // Label above the dial.
        g.setColour (dim);
        g.setFont (DarkLookAndFeel::makeFont (static_cast<float> (labelH) * 0.82f));
        g.drawText (label, labelArea.reduced (2, 0), juce::Justification::centred, true);

        // Formatted value below the dial. The formatter is the display, so building
        // its String here is required, not incidental.
        g.setColour (text);
        g.setFont (DarkLookAndFeel::makeFont (static_cast<float> (valueH) * 0.86f));
        g.drawText (format != nullptr ? format (value) : juce::String (value, 2),
                    valueArea.reduced (2, 0), juce::Justification::centred, true);

        // The dial arc, sized to the smaller dial dimension. The angle convention here
        // mirrors LookAndFeel_V4::drawRotarySlider exactly (addCentredArc with a 0.75*pi
        // to 2.25*pi sweep, thumb offset by halfPi), because that is the arc maths this
        // JUCE version actually ships and the only one verified in the checkout.
        const juce::Rectangle<float> dialArea (0.0f, static_cast<float> (labelH), w,
                                               h - static_cast<float> (labelH + valueH));
        const auto dialBounds = dialArea.reduced (6.0f);
        const auto cx = dialBounds.getCentreX();
        const auto cy = dialBounds.getCentreY();
        const auto radius = juce::jmin (dialBounds.getWidth(), dialBounds.getHeight()) * 0.5f;
        if (radius < 4.0f)
            return;

        const auto lineW = juce::jmin (6.0f, radius * 0.30f);
        const auto arcRadius = radius - lineW * 0.5f;

        const auto startAngle = juce::MathConstants<float>::pi * 0.75f;
        const auto endAngle   = juce::MathConstants<float>::pi * 2.25f;
        const auto toAngle    = startAngle + static_cast<float> (proportion()) * (endAngle - startAngle);

        juce::Path backgroundArc;
        backgroundArc.addCentredArc (cx, cy, arcRadius, arcRadius,
                                     0.0f, startAngle, endAngle, true);
        g.setColour (laf.panelHighlighted);
        g.strokePath (backgroundArc, juce::PathStrokeType (lineW,
                                                           juce::PathStrokeType::curved,
                                                           juce::PathStrokeType::rounded));

        if (active)
        {
            juce::Path valueArc;
            valueArc.addCentredArc (cx, cy, arcRadius, arcRadius,
                                    0.0f, startAngle, toAngle, true);
            g.setColour (accent);
            g.strokePath (valueArc, juce::PathStrokeType (lineW,
                                                          juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
        }

        // Thumb dot at the current angle; the halfPi shift is what makes a 0.75*pi
        // start angle read as "pointing down-left" the way JUCE draws it.
        const auto thumbWidth = lineW * 2.0f;
        const juce::Point<float> thumbPoint (cx + arcRadius * std::cos (toAngle - juce::MathConstants<float>::halfPi),
                                             cy + arcRadius * std::sin (toAngle - juce::MathConstants<float>::halfPi));
        g.setColour (accent);
        g.fillEllipse (juce::Rectangle<float> (thumbWidth, thumbWidth).withCentre (thumbPoint));
    }

    //==============================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! active)
            return;

        grabKeyboardFocus();
        dragStartValue = value;
        dragStartY = static_cast<double> (e.y);
        dragging = true;
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! active || ! dragging)
            return;

        const double range = maxValue - minValue;
        if (range <= 0.0)
            return;

        // Sensitivity: 300 px maps to the full range; Shift makes it five times finer.
        const double pixelsForFullRange = e.mods.isShiftDown() ? 1500.0 : 300.0;
        const double deltaY = dragStartY - static_cast<double> (e.y);   // up is positive
        double newValue = dragStartValue + (deltaY / pixelsForFullRange) * range;

        if (interval > 0.0)
            newValue = snap (newValue);

        applyFromUser (clamp (newValue));
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (active)
            applyFromUser (clamp (defaultOnDoubleClick));
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (! active)
            return false;

        const double range = maxValue - minValue;
        const double step = interval > 0.0 ? interval : range * 0.01;
        if (step <= 0.0)
            return false;

        if (key.isKeyCode (juce::KeyPress::rightKey) || key.isKeyCode (juce::KeyPress::upKey))
        {
            applyFromUser (clamp (value + step));
            return true;
        }

        if (key.isKeyCode (juce::KeyPress::leftKey) || key.isKeyCode (juce::KeyPress::downKey))
        {
            applyFromUser (clamp (value - step));
            return true;
        }

        return false;
    }

private:
    //==============================================================================
    juce::String label;
    double minValue, maxValue, interval;
    double value;
    double defaultOnDoubleClick;
    TextFormat format;

    bool active = true;
    bool suppressCallback = false;

    // Drag state for the manual (non-Slider) implementation.
    bool dragging = false;
    double dragStartValue = 0.0;
    double dragStartY = 0.0;

    double clamp (double v) const noexcept { return juce::jlimit (minValue, maxValue, v); }

    double proportion() const noexcept
    {
        const double range = maxValue - minValue;
        return range > 0.0 ? (value - minValue) / range : 0.0;
    }

    double snap (double v) const noexcept
    {
        if (interval <= 0.0)
            return v;
        return minValue + std::round ((v - minValue) / interval) * interval;
    }

    void applyFromUser (double newValue)
    {
        value = newValue;
        repaint();

        if (! suppressCallback && onValueChange)
            onValueChange (value);
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Knob)
};

//==============================================================================
/**
    A selector for a ParamDescription with numSteps >= 2: draws the parameter name, a
    row of tappable step cells, and the selected step's label. Clicking cell k writes
    k as the value. The label strings are copied in the constructor so the current
    label is read from storage at repaint time and nothing is built per frame.
*/
class DiscreteSelector : public juce::Component
{
public:
    DiscreteSelector (juce::String labelText, int numSteps, const char* const* labels)
        : label (std::move (labelText)),
          steps (juce::jmax (2, numSteps)),
          currentIndex (0)
    {
        for (int i = 0; i < steps; ++i)
            names.add (labels != nullptr && labels[i] != nullptr ? tr (labels[i])
                                                                  : juce::String (i));
    }

    ~DiscreteSelector() override = default;

    void setValue (double newValue)
    {
        const auto index = clampIndex (static_cast<int> (newValue + 0.5));
        const juce::ScopedValueSetter<bool> suppress (suppressCallback, true);
        currentIndex = index;
        repaint();
    }

    double getValue() const { return static_cast<double> (currentIndex); }

    void setActive (bool shouldBeActive)
    {
        if (active != shouldBeActive)
        {
            active = shouldBeActive;
            repaint();
        }
    }

    std::function<void (double)> onValueChange;

    //==============================================================================
    void resized() override { repaint(); }

    void paint (juce::Graphics& g) override
    {
        const auto w = static_cast<float> (getWidth());
        const auto h = static_cast<float> (getHeight());
        auto& laf = DarkLookAndFeel::instance();

        // Height is computed once as an int so it serves both the drawText rectangle and
        // the float bar geometry, keeping the two in lockstep.
        const int labelH = static_cast<int> (juce::jlimit (9.0f, 15.0f, h * 0.26f));
        const juce::Rectangle<float> barArea (0.0f, static_cast<float> (labelH), w,
                                              h - static_cast<float> (labelH));

        g.setColour (laf.textDim);
        g.setFont (DarkLookAndFeel::makeFont (static_cast<float> (labelH) * 0.85f));
        g.drawText (label, 2, 0, getWidth() - 4, labelH, juce::Justification::centredLeft, true);

        if (barArea.getWidth() <= 1.0f || barArea.getHeight() <= 1.0f)
            return;

        const auto cellWidth = barArea.getWidth() / static_cast<float> (steps);
        const juce::Rectangle<float> row = barArea.reduced (1.0f, 3.0f);

        for (int i = 0; i < steps; ++i)
        {
            const juce::Rectangle<float> cell (row.getX() + cellWidth * static_cast<float> (i),
                                               row.getY(), cellWidth, row.getHeight());
            const juce::Rectangle<float> inset = cell.reduced (1.5f, 0.0f);

            const bool selected = i == currentIndex;
            if (selected && active)
                g.setColour (laf.accent);
            else if (selected)
                g.setColour (laf.accentDim);
            else
                g.setColour (laf.panelBackground);

            g.fillRoundedRectangle (inset, 3.0f);
            g.setColour (selected ? laf.windowBackground : laf.textDim);
            g.drawRoundedRectangle (inset, 3.0f, 1.0f);
        }

        // Current step's label, drawn inside the selected cell. names[] is a stored
        // String read by reference, so no allocation happens here.
        const auto& current = names.getReference (currentIndex);
        const auto selectedX = row.getX() + cellWidth * static_cast<float> (currentIndex);
        const int textX = static_cast<int> (selectedX) + 3;
        const int textWidth = static_cast<int> (cellWidth) - 6;
        g.setColour (active ? laf.windowBackground : laf.textDim);
        g.setFont (DarkLookAndFeel::makeFont (row.getHeight() * 0.7f));
        g.drawText (current, textX, static_cast<int> (row.getY()), textWidth,
                    static_cast<int> (row.getHeight()), juce::Justification::centred, true);
    }

    //==============================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! active)
            return;

        const float h = static_cast<float> (getHeight());
        const float labelHeight = juce::jlimit (9.0f, 15.0f, h * 0.26f);
        const float barTop = labelHeight;

        if (static_cast<float> (e.y) < barTop)
            return;

        const float barWidth = static_cast<float> (getWidth());
        if (barWidth <= 1.0f)
            return;

        const auto index = clampIndex (static_cast<int> ((static_cast<float> (e.x) / barWidth)
                                                         * static_cast<float> (steps)));
        if (index != currentIndex)
        {
            currentIndex = index;
            repaint();

            if (! suppressCallback && onValueChange)
                onValueChange (static_cast<double> (currentIndex));
        }
    }

private:
    juce::String label;
    int steps;
    int currentIndex;
    juce::StringArray names;   // built once, read by reference in paint()

    bool active = true;
    bool suppressCallback = false;

    int clampIndex (int i) const noexcept { return juce::jlimit (0, steps - 1, i); }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DiscreteSelector)
};

//==============================================================================
/**
    The chunky on/off switch used for the per-card bypass and for the Monitor and
    virtual-device toggles. Filled with the accent colour when on, outlined when off,
    and greyed out when the owning effect is inactive, so the state is legible at a
    glance across the board.
*/
class FlagButton : public juce::Component
{
public:
    FlagButton (juce::String labelText, bool startOn)
        : label (std::move (labelText)),
          on (startOn)
    {
    }

    ~FlagButton() override = default;

    void setOn (bool shouldBeOn)
    {
        if (on != shouldBeOn)
        {
            on = shouldBeOn;
            repaint();
        }
    }

    bool isOn() const { return on; }

    void setActive (bool shouldBeActive)
    {
        if (active != shouldBeActive)
        {
            active = shouldBeActive;
            repaint();
        }
    }

    void setLabelText (const juce::String& newLabel)
    {
        label = newLabel;
        repaint();
    }

    std::function<void (bool)> onClicked;

    //==============================================================================
    void resized() override { repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto& laf = DarkLookAndFeel::instance();
        const juce::Rectangle<float> body (0.0f, 0.0f, static_cast<float> (getWidth()),
                                           static_cast<float> (getHeight()));
        const auto corner = juce::jmin (8.0f, body.getHeight() * 0.28f);
        const auto inset = body.reduced (1.5f);

        if (! active)
        {
            g.setColour (laf.panelBackground);
            g.fillRoundedRectangle (inset, corner);
            g.setColour (laf.textDim.withAlpha (0.5f));
            g.drawRoundedRectangle (inset, corner, 1.5f);
        }
        else if (on)
        {
            g.setColour (laf.accent);
            g.fillRoundedRectangle (inset, corner);
            g.setColour (laf.accent.brighter (0.15f));
            g.drawRoundedRectangle (inset, corner, 1.5f);
        }
        else
        {
            g.setColour (laf.panelBackground);
            g.fillRoundedRectangle (inset, corner);
            g.setColour (laf.textDim);
            g.drawRoundedRectangle (inset, corner, 1.5f);
        }

        const auto textColor = ! active ? laf.textDim
                                        : (on ? laf.windowBackground : laf.textPrimary);
        g.setColour (textColor);
        g.setFont (DarkLookAndFeel::makeFont (body.getHeight() * 0.5f).boldened());
        g.drawText (label, getLocalBounds().reduced (7, 0), juce::Justification::centred, true);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        if (! active)
            return;

        on = ! on;
        repaint();

        if (onClicked)
            onClicked (on);
    }

private:
    juce::String label;
    bool on;
    bool active = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FlagButton)
};

//==============================================================================
/**
    A peak meter, vertical or horizontal. The caller pushes only a new linear peak with
    setPeak(); the class converts it to dB on its own -60 dB floor, rises instantly to a
    new peak and then decays on its timer, holding a short peak line that falls slowly.
*/
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    enum class Orientation { vertical, horizontal };

    explicit LevelMeter (Orientation newOrientation)
        : orientation (newOrientation)
    {
        startTimer (30);   // ~33 Hz refresh is plenty for a peak meter
    }

    ~LevelMeter() override { stopTimer(); }

    /** linearPeak is 0..1 (or slightly above); floored to meterFloorDb inside. */
    void setPeak (float linearPeak)
    {
        // gainToDecibels is templated on one type, so the gain and the floor must both
        // be double here; the peak arrives as a float from the audio thread.
        const double db = juce::Decibels::gainToDecibels<double> (static_cast<double> (linearPeak),
                                                                   meterFloorDb);
        const double normalised = toNormalised (db);

        // Attack is instant; only rise, never lower an already-higher bar. The
        // hold is only re-armed by a new high: re-arming it every block kept it
        // alive forever and froze the line at the loudest moment since launch.
        if (normalised > level)
            level = normalised;

        if (normalised > peakHold)
        {
            peakHold = normalised;
            peakHoldTimer = peakHoldDuration;
        }
    }

    void setLabel (juce::String newLabel)
    {
        label = std::move (newLabel);
        repaint();
    }

    //==============================================================================
    void resized() override { repaint(); }

    void paint (juce::Graphics& g) override
    {
        auto& laf = DarkLookAndFeel::instance();
        const juce::Rectangle<float> area (0.0f, 0.0f, static_cast<float> (getWidth()),
                                           static_cast<float> (getHeight()));

        g.setColour (laf.panelBackground);
        g.fillRoundedRectangle (area, 3.0f);

        const double filled = juce::jlimit (0.0, 1.0, level);
        juce::Rectangle<float> fillRect;

        if (orientation == Orientation::vertical)
        {
            const auto fh = static_cast<float> (area.getHeight() * filled);
            fillRect = juce::Rectangle<float> (area.getX(), area.getBottom() - fh,
                                               area.getWidth(), fh);
        }
        else
        {
            const auto fw = static_cast<float> (area.getWidth() * filled);
            fillRect = juce::Rectangle<float> (area.getX(), area.getY(), fw, area.getHeight());
        }

        if (filled > 0.0)
        {
            const auto gradient = orientation == Orientation::vertical
                ? juce::ColourGradient::vertical (laf.meterTop, area.getY(),
                                                  laf.meterBottom, area.getBottom())
                : juce::ColourGradient::horizontal (laf.meterBottom, area.getX(),
                                                    laf.meterTop, area.getRight());
            g.setGradientFill (gradient);
            g.fillRect (fillRect);
        }

        // The held peak line. drawLine is used for both orientations because the
        // checkout only ever exercises Graphics::drawHorizontalLine, while the 4-point
        // drawLine overload is confirmed here:
        //   g.drawLine ((float) x + ..., linePos, (float) width * 0.8f, linePos)
        if (peakHold > 0.0)
        {
            const double pf = juce::jlimit (0.0, 1.0, peakHold);
            g.setColour (laf.textPrimary.withAlpha (0.85f));

            if (orientation == Orientation::vertical)
            {
                const auto py = area.getBottom() - static_cast<float> (area.getHeight() * pf);
                g.drawLine (area.getX(), py, area.getRight(), py);
            }
            else
            {
                const auto px = area.getX() + static_cast<float> (area.getWidth() * pf);
                g.drawLine (px, area.getY(), px, area.getBottom());
            }
        }

        if (! label.isEmpty())
        {
            g.setColour (laf.textDim);
            g.setFont (DarkLookAndFeel::makeFont (juce::jlimit (8.0f, 12.0f, area.getHeight() * 0.16f)));
            g.drawText (label, getLocalBounds().reduced (3), juce::Justification::topLeft, true);
        }

        g.setColour (laf.textDim.withAlpha (0.6f));
        g.drawRoundedRectangle (area.reduced (0.5f), 3.0f, 1.0f);
    }

private:
    //==============================================================================
    static constexpr double meterFloorDb = -60.0;
    static constexpr double peakHoldDuration = 12.0;   // timer ticks before the line drops

    Orientation orientation;
    juce::String label;

    double level = 0.0;       // decaying bar, normalised 0..1
    double peakHold = 0.0;    // held peak line, normalised 0..1
    double peakHoldTimer = 0.0;

    double toNormalised (double db) const noexcept
    {
        return juce::jlimit (0.0, 1.0, (db - meterFloorDb) / (0.0 - meterFloorDb));
    }

    void timerCallback() override
    {
        // Release: exponential-ish fall toward silence.
        level *= 0.80;
        if (level < 0.001)
            level = 0.0;

        // The peak line freezes for a moment, then drops faster once its hold expires.
        if (peakHoldTimer > 0.0)
            --peakHoldTimer;
        else
        {
            peakHold *= 0.90;
            if (peakHold < 0.001)
                peakHold = 0.0;
        }

        repaint();
    }

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LevelMeter)
};

//==============================================================================
/**
    A TextButton sized for the dense status strip and header, with its colours pinned to
    the dark palette so it stays legible even if a parent forgets to propagate the
    look-and-feel.
*/
class SmallTextButton : public juce::TextButton
{
public:
    explicit SmallTextButton (const juce::String& text)
        : juce::TextButton (text)
    {
        auto& laf = DarkLookAndFeel::instance();
        setColour (juce::TextButton::buttonColourId, laf.panelBackground);
        setColour (juce::TextButton::buttonOnColourId, laf.accent);
        setColour (juce::TextButton::textColourOffId, laf.textPrimary);
        setColour (juce::TextButton::textColourOnId, laf.windowBackground);
    }

    ~SmallTextButton() override = default;
};

} // namespace pedal
