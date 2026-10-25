#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "Audio/AudioEngine.h"
#include "Common/PedalConfig.h"
#include "UI/DarkLookAndFeel.h"
#include "UI/MainComponent.h"

/*==============================================================================
  Application bootstrap.

  Ownership here is not a style choice, it is the only order that does not crash:

    * JUCEApplication owns the AudioEngine and the window.
    * shutdown() deletes the window first. ~ResizableWindow deletes its content,
      and MainComponent's destructor writes the settings file and stops touching
      the engine - so the engine has to still be alive at that moment.
    * The engine is released second, and its destructor calls shutdown(), which
      removes the audio callbacks and closes both devices.

  Making the engine a member of the window instead would invert that: a window's
  own members are destroyed before its base class destructor runs, so the content
  component would be deleted after the engine it references.

  JUCEApplication itself moved modules in JUCE 8: it is declared in
  juce_gui_basics/application/juce_Application.h, not in juce_gui_extra. That is
  why this file includes only juce_gui_basics.
==============================================================================*/
namespace pedal
{

class PedalBoardWindow : public juce::DocumentWindow
{
public:
    explicit PedalBoardWindow (AudioEngine& engineToUse)
        : juce::DocumentWindow ("PedalBoard",
                                 DarkLookAndFeel::windowBackground,
                                 juce::DocumentWindow::minimiseButton
                               | juce::DocumentWindow::maximiseButton
                               | juce::DocumentWindow::closeButton,
                                 true),   // added to the desktop here: see below
          engine (engineToUse)
    {
        setLookAndFeel (&DarkLookAndFeel::instance());

        // A native title bar: the window frame is not where any of this app's
        // value is, and asking JUCE to draw it would mean reimplementing snap,
        // shadow and DPI behaviour that already works.
        setUsingNativeTitleBar (true);

        setContentOwned (new MainComponent (engine), true);

        // After the content: MainComponent installs the saved language in its
        // constructor, so this is the first point where tr() is the user's table.
        setName (tr ("PedalBoard - guitar chain, monitor and virtual cable output"));

        /*  Fitted to the display instead of a fixed 1240x880.

            The native frame is not part of the bounds JUCE is handed: the peer
            puts the *client area* at these coordinates, so Windows draws the
            caption above them. Measured on the first machine this ran on
            (1366x768, _toolchain_test\window_probe.ps1): a client of 1240x768 at
            y=0 produced a window rect whose top was y=-31, which left the
            caption - and with it the close button - off the screen and the
            window impossible to move or dismiss.

            The margins are logical pixels, which JUCE keeps roughly
            scale-independent, so they do not need recomputing per DPI. */
        auto requestedWidth  = 1240;
        auto requestedHeight = 880;
        auto requestedX      = -1;
        auto requestedY      = -1;

        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            const auto usable    = display->userBounds.toNearestInt();
            const auto topMargin = 48;   // measured caption: 31
            const auto edgeMargin = 8;   // measured invisible resize border

            requestedWidth  = juce::jmax (minWindowWidth, juce::jmin (requestedWidth,  usable.getWidth()  - 2 * edgeMargin));
            requestedHeight = juce::jmax (minWindowHeight, juce::jmin (requestedHeight, usable.getHeight() - topMargin - edgeMargin));
            requestedX      = usable.getX() + (usable.getWidth() - requestedWidth)  / 2;
            requestedY      = usable.getY() + topMargin;
        }

        setResizable (true, false);
        setResizeLimits (minWindowWidth, minWindowHeight, 2400, 2000);

        if (requestedX >= 0 && requestedY >= 0)
            setBounds (requestedX, requestedY, requestedWidth, requestedHeight);
        else
            centreWithSize (requestedWidth, requestedHeight);   // no display reported

        setVisible (true);
    }

    /** Explicit, so the order is stated rather than implied: the content is
        deleted here, while the engine it references is still alive, instead of
        from ~ResizableWindow after this class has already been torn down. */
    ~PedalBoardWindow() override { setContentOwned (nullptr, false); }

    void closeButtonPressed() override
    {
        juce::JUCEApplicationBase::getInstance()->systemRequestedQuit();
    }

private:
    AudioEngine& engine;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PedalBoardWindow)
};

//==============================================================================
class PedalBoardApplication : public juce::JUCEApplication
{
public:
    PedalBoardApplication() = default;
    ~PedalBoardApplication() override = default;

    const juce::String getApplicationName() override    { return "PedalBoard"; }
    const juce::String getApplicationVersion() override { return "1.0.0"; }

    /** One instance per machine: two processes opening the same guitar input and
        the same CABLE Input stream would fight over both. */
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise (const juce::String&) override
    {
        // Installed before anything is built, so every widget - including the
        // custom ones that read the palette statically - starts dark.
        juce::LookAndFeel::setDefaultLookAndFeel (&DarkLookAndFeel::instance());

        engine = std::make_unique<AudioEngine>();
        window = std::make_unique<PedalBoardWindow> (*engine);
    }

    void shutdown() override
    {
        window.reset();     // deletes MainComponent, which saves its settings
        engine.reset();     // then closes the devices on a still-valid engine

        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        // No confirmation: the settings file is written continuously, so there is
        // nothing to lose by going straight away.
        quit();
    }

    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<PedalBoardWindow> window;
    std::unique_ptr<AudioEngine> engine;
};

} // namespace pedal

// Generates the Windows boilerplate that launches the app above.
START_JUCE_APPLICATION (pedal::PedalBoardApplication)
