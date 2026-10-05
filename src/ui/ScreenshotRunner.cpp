// ScreenshotRunner implementation: the picture plan, plus the interaction smoke test that follows it.
#include "ui/ScreenshotRunner.h"

#include "app/Log.h"
#include "engine/builtin/BuiltinProcessor.h"
#include "engine/builtin/Compressor.h"
#include "engine/builtin/Equalizer.h"
#include "ui/Theme.h"
#include "ui/Toast.h"
#include "ui/UiScale.h"
#include "ui/fx/CompPanel.h"
#include "ui/fx/EqPanel.h"
#include "ui/fx/NoisePanel.h"
#include "engine/builtin/NoiseRemoval.h"
#include "ui/fx/SlotList.h"
#include "ui/setup/SetupChoices.h"
#include "ui/setup/SetupSheet.h"
#include "ui/setup/SetupSheets.h"
#include "ui/setup/SetupWidgets.h"
#include "ui/widgets/Dropdown.h"
#include "ui/widgets/Knob.h"
#include "ui/widgets/PlayThrough.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace hr
{
namespace
{
    // The EQ graph may cost this many reference paints (see measureEqPaint) before the smoke test fails. It costs about 0.75 of
    // one on the dev machine, so this only trips when the graph's paint has become several times heavier than it is meant to be.
    constexpr double kMaxEqPaintCost = 4.0;

    // ---- Synthetic input for the smoke test ---------------------------------------------------------
    template <class T>
    void collect (juce::Component& root, std::vector<T*>& found)
    {
        for (auto* child : root.getChildren())
        {
            if (auto* match = dynamic_cast<T*> (child))
                found.push_back (match);

            collect<T> (*child, found);
        }
    }

    juce::MouseEvent makeEvent (juce::Component& c, juce::Point<float> position, bool shift, juce::Point<float> downPosition,
                                bool dragged, int clicks = 1, bool alt = false)
    {
        int flags = juce::ModifierKeys::leftButtonModifier;

        if (shift)
            flags |= juce::ModifierKeys::shiftModifier;

        if (alt)
            flags |= juce::ModifierKeys::altModifier;

        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), position, juce::ModifierKeys (flags),
                                 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(), downPosition,
                                 juce::Time::getCurrentTime(), clicks, dragged);
    }

    // Press at `from`, move to `to`, release: what a drag with the mouse does.
    void drag (juce::Component& c, juce::Point<float> from, juce::Point<float> to, bool shift = false)
    {
        c.mouseDown (makeEvent (c, from, shift, from, false));
        c.mouseDrag (makeEvent (c, to, shift, from, true));
        c.mouseUp (makeEvent (c, to, shift, from, true));
    }

    void doubleClick (juce::Component& c, juce::Point<float> at)
    {
        c.mouseDoubleClick (makeEvent (c, at, false, at, false, 2));
    }

    // Button::triggerClick() posts a message; the tests want the click to have happened when the call returns.
    void click (juce::Button& b)
    {
        if (b.onClick)
            b.onClick();
    }

    void wheel (juce::Component& c, juce::Point<float> at, float deltaY)
    {
        juce::MouseWheelDetails details;
        details.deltaX = 0.0f;
        details.deltaY = deltaY;
        details.isReversed = false;
        details.isSmooth = false;
        details.isInertial = false;
        c.mouseWheelMove (makeEvent (c, at, false, at, false), details);
    }

    // The mic's EQ, and the EQ panel that is open in the drawer (if any).
    Equalizer* micEq (AppController& controller)
    {
        for (auto slotTree : AppState::getFx (controller.getState().getMicChannel()))
        {
            const auto slot = AppState::describeSlot (slotTree);

            if (slot.builtinId == "eq")
                return dynamic_cast<Equalizer*> (controller.getFxProcessor (slot.uid));
        }

        return nullptr;
    }

    EqPanel* findEqPanel (juce::Component& root)
    {
        std::vector<EqPanel*> panels;
        collect<EqPanel> (root, panels);
        return panels.empty() ? nullptr : panels.front();
    }

    void setEqParam (Equalizer& eq, const juce::String& id, float value)
    {
        if (auto* p = eq.apvts.getParameter (id))
            p->setValueNotifyingHost (p->convertTo0to1 (value));
    }

    // ---- Demo data ------------------------------------------------------------------------------------
    // Stand-ins so the Add effect list and the plugin rows have something to show. They are only ever put in the
    // in-memory plugin list of a screenshot run: nothing is saved and none of them can actually be loaded.
    juce::PluginDescription fakePlugin (const juce::String& name, const juce::String& vendor, const juce::String& format, int id)
    {
        juce::PluginDescription d;
        d.name = name;
        d.descriptiveName = name;
        d.manufacturerName = vendor;
        d.pluginFormatName = format;
        d.fileOrIdentifier = "/demo/" + name;
        d.uniqueId = d.deprecatedUid = id;
        d.category = "Fx";
        d.isInstrument = false;
        d.numInputChannels = d.numOutputChannels = 2;
        return d;
    }

    juce::Array<juce::PluginDescription> demoPlugins()
    {
        return { fakePlugin ("TDR Nova", "Tokyo Dawn Records", "VST3", 1),
                 fakePlugin ("TDR Kotelnikov", "Tokyo Dawn Records", "VST3", 2),
                 fakePlugin ("ReaComp", "Cockos", "VST3", 3),
                 fakePlugin ("ReaEQ", "Cockos", "VST3", 4),
                 fakePlugin ("LSP Compressor Stereo", "LSP Plugins", "LV2", 5),
                 fakePlugin ("LSP Parametric Equalizer x16", "LSP Plugins", "LV2", 6),
                 fakePlugin ("Calf Multiband Gate", "Calf Studio Gear", "LV2", 7),
                 fakePlugin ("Tap Reverberator", "Tom Szilagyi", "LADSPA", 8) };
    }
}

ScreenshotRunner::ScreenshotRunner (juce::File outputDirectory, std::function<void (bool)> whenFinished)
    : directory (std::move (outputDirectory)), finished (std::move (whenFinished))
{
}

ScreenshotRunner::~ScreenshotRunner()
{
    stopTimer();
    endRun();

    if (setupHome != juce::File())
        setupHome.deleteRecursively();
}

void ScreenshotRunner::start()
{
    if (! directory.createDirectory().wasOk())
    {
        Log::error ("Cannot create " + directory.getFullPathName());
        allOk = false;
        finish();
        return;
    }

    beginRun (false);
    buildPlan();
    lastMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (30);
}

void ScreenshotRunner::beginRun (bool micMuted)
{
    AppController::Options options;
    options.demo = true;
    options.demoMicMuted = micMuted;
    options.persist = false;

    controller = std::make_unique<AppController> (options);
    controller->start();
    controller->getState().setSetting (ids::firstRunDone, 1);      // the first-run sheet only appears in its own shot

    for (const auto& plugin : demoPlugins())
        controller->getPlugins().getKnownPluginList().addType (plugin);

    // A scratch home folder for the setup wizard to install into, and a stand-in AppImage to install.
    if (setupHome == juce::File())
    {
        setupHome = juce::File::createTempFile ("headroom-setup-home");
        setupHome.createDirectory();
    }

    const auto appImage = setupHome.getChildFile ("Downloads/Headroom-" + install::currentVersion() + "-x86_64.AppImage");
    appImage.getParentDirectory().createDirectory();

    if (! appImage.existsAsFile())
        appImage.replaceWithText ("a stand-in for the AppImage");

    installRunner = std::make_shared<install::RecordingRunner>();

    InstallManager::Setup setup;
    setup.locations = install::Locations (setupHome);
    setup.runner = installRunner;
    setup.appImage = appImage;
    setup.supported = true;
    setup.settingsFolders = { setupHome.getChildFile (".config/headroom") };
    setup.pauseAfterWirePlumberMs = 0;
    setup.serviceCheckMs = 0;
    setup.udevRule = setupHome.getChildFile ("70-headroom-wave.rules");

    installManager = std::make_unique<InstallManager> (std::move (setup));
    installManager->onOpenInstalled = [this] (const juce::File& program) { openedProgram = program; };
    installManager->onUninstall = [this] (bool deleteSettings) { uninstallAsked = true; uninstallDeletes = deleteSettings; };

    ui = std::make_unique<MainComponent> (*controller, installManager.get());
    ui->setSize (theme::kDefaultWidth, theme::kDefaultHeight);
    mutedRun = micMuted;
}

void ScreenshotRunner::endRun()
{
    ui.reset();
    controller.reset();
    installManager.reset();
}

// The scratch home is installed (the Settings rows for it show) or not (the first-run wizard installs it).
void ScreenshotRunner::setInstalled (bool installed)
{
    auto& installer = installManager->getInstaller();

    if (installed)
    {
        install::InstallOptions options;
        options.source = installManager->getAppImage();
        options.startNow = false;
        installer.install (options);
    }
    else
    {
        installer.uninstall ({});
    }
}

void ScreenshotRunner::wizardNext()
{
    if (auto* sheet = ui->getSetupSheet())
        click (sheet->getNextButton());

    ui->flushUpdates();
}

void ScreenshotRunner::buildPlan()
{
    plan.clear();

    // Each setUp is also a smoke test: it drives the same code paths a click would.
    plan.push_back ({ "mixer", 0, [this]
    {
        ui->showTab (Tab::mixer);
        ui->getMixer().setMix (Mix::Monitor);
    } });

    plan.push_back ({ "mixer-stream", 600, [this] { ui->getMixer().setMix (Mix::Stream); } });

    // Input channels (a second mic that is there, a mic that is not) and the Headphones mix playing to two devices, with the "Play
    // through" list open; the checks that follow use it. Everything is put back afterwards, so that the pictures after these are
    // what they were.
    plan.push_back ({ "mixer-inputs", 800, [this] { addDemoInputs(); } });
    plan.push_back ({ "mixer-playthrough", 400, [this] { openPlayThrough (Mix::Monitor); } });
    plan.push_back ({ {}, 100, [this] { checkHeadphonePlayThrough(); } });
    plan.push_back ({ {}, 250, [this] { openPlayThrough (Mix::Stream); } });
    plan.push_back ({ {}, 100, [this] { checkStreamPlayThrough(); } });
    plan.push_back ({ {}, 250, [this] { removeDemoInputs(); } });

    plan.push_back ({ "fx-mic", 900, [this]
    {
        ui->getMixer().setMix (Mix::Monitor);
        ui->openFxDrawer ("mic", false);
        ui->getDrawer().expandBuiltin ("eq");
        ui->flushUpdates();

        // A voice EQ like a person might make (mud out, presence and air in), so the picture has a curve to show.
        if (auto* eq = micEq (*controller))
        {
            setEqParam (*eq, "b3Freq", 280.0f);  setEqParam (*eq, "b3Gain", -3.0f);  setEqParam (*eq, "b3Q", 1.2f);  setEqParam (*eq, "b3On", 1.0f);
            setEqParam (*eq, "b5Freq", 3400.0f); setEqParam (*eq, "b5Gain", 3.5f);   setEqParam (*eq, "b5Q", 0.9f);  setEqParam (*eq, "b5On", 1.0f);
            setEqParam (*eq, "b7Type", 2.0f);    setEqParam (*eq, "b7Freq", 9500.0f); setEqParam (*eq, "b7Gain", 2.5f);
            setEqParam (*eq, "b7Q", 0.7071f);    setEqParam (*eq, "b7On", 1.0f);
        }

        if (auto* panel = findEqPanel (ui->getDrawer()))
        {
            panel->getFeed().setForceLive (true);       // nothing is "showing" in screenshot mode; the spectrum needs to run anyway

            // The pointer rests on the presence band, so the picture shows what the label says.
            auto& graph = panel->getGraph();
            const juce::Point<float> at (graph.xForFreq (3400.0f), graph.yForDb (3.5f));
            graph.setSelectedBand (4);
            graph.mouseMove (makeEvent (graph, at, false, at, false));
        }
    } });

    plan.push_back ({ "eq-expanded", 700, [this]
    {
        if (auto* panel = findEqPanel (ui->getDrawer()))
        {
            panel->openExpanded();

            if (auto* sheet = panel->getSheet())
            {
                auto& big = sheet->getGraph();
                const juce::Point<float> at (big.xForFreq (280.0f), big.yForDb (-3.0f));
                big.setSelectedBand (2);
                big.mouseMove (makeEvent (big, at, false, at, false));
            }
        }
    } });

    plan.push_back ({ {}, 100, [this] { measureEqPaint(); } });

    // The compressor's history is 5 s wide and only fills while the panel is on screen: let the demo voice run that long.
    plan.push_back ({ "fx-comp", 5600, [this]
    {
        if (auto* panel = findEqPanel (ui->getDrawer()))
            panel->closeExpanded();

        ui->getDrawer().collapseAll();
        ui->getDrawer().expandBuiltin ("comp");
    } });

    // Details open, with one setting changed by hand so Amount says Custom.
    plan.push_back ({ "fx-comp-details", 600, [this]
    {
        const auto uid = AppState::describeSlot (AppState::getFx (controller->getState().getMicChannel()).getChild (2)).uid;
        std::vector<CompPanel*> panels;
        collect<CompPanel> (ui->getDrawer(), panels);

        if (auto* comp = dynamic_cast<Compressor*> (controller->getFxProcessor (uid)); comp != nullptr && ! panels.empty())
        {
            auto* ratio = comp->apvts.getParameter ("ratio");
            ratio->setValueNotifyingHost (ratio->convertTo0to1 (4.5f));
            panels.front()->setDetailsOpen (true);
        }
    }, { theme::kDefaultWidth, 900 } });          // taller than usual so the whole open panel fits

    // The noise panel needs a moment: the network loads in the background and the spectrum wants a second of audio.
    // The picture is taken in a pause between two phrases, when there is room noise to show being removed.
    plan.push_back ({ "fx-noise", 2500, [this]
    {
        ui->getDrawer().collapseAll();
        ui->getDrawer().expandBuiltin ("noise");
    }, { theme::kDefaultWidth, theme::kDefaultHeight }, [this]
    {
        auto* noise = dynamic_cast<NoiseRemoval*> (controller->getFxProcessor (AppState::describeSlot (AppState::getFx (controller->getState().getMicChannel()).getChild (0)).uid));
        return noise != nullptr && noise->getStatus() == NoiseRemoval::Status::ready && ! noise->isVoiceActive() && noise->getReductionDb() > 6.0f;
    } });

    plan.push_back ({ "fx-add", 300, [this]
    {
        ui->getDrawer().collapseAll();
        ui->getDrawer().showAddPanel (true);
    } });

    plan.push_back ({ "apps", 300, [this]
    {
        ui->getDrawer().showAddPanel (false);
        ui->closeFxDrawer (false);
        ui->showTab (Tab::apps);
    } });

    plan.push_back ({ "settings", 300, [this]
    {
        setInstalled (true);            // the Setup and Uninstall rows are there for an installed Headroom
        ui->showTab (Tab::settings);
    } });
    plan.push_back ({ "settings-tall", 100, [] {}, { theme::kDefaultWidth, 1500 } });

    plan.push_back ({ "firstrun", 300, [this]
    {
        ui->showTab (Tab::mixer);
        controller->getState().setSetting (ids::firstRunDone, 0);
    } });

    // The setup wizard, step by step, as a first launch of an AppImage that is not installed shows it.
    plan.push_back ({ "setup-1", 300, [this]
    {
        setInstalled (false);
        ui->showTab (Tab::mixer);
        ui->showSetupWizard (SetupSheet::Mode::firstRun);
    } });

    for (int step = 2; step <= 6; ++step)
        plan.push_back ({ "setup-" + juce::String (step), 250, [this] { wizardNext(); } });

    // Set up Headroom: the choices go into the model, it installs itself into the scratch home, the last page says so.
    plan.push_back ({ "setup-7", 250, [this] { wizardNext(); }, { theme::kDefaultWidth, theme::kDefaultHeight },
                      [this] { auto* sheet = ui->getSetupSheet(); return sheet != nullptr && sheet->getPage() == SetupSheet::Page::done; } });

    plan.push_back ({ "uninstall", 250, [this]
    {
        ui->hideSheet();
        ui->showTab (Tab::settings);
        ui->showUninstallSheet();
    } });

    plan.push_back ({ "update", 100, [this] { ui->hideSheet(); ui->showTab (Tab::mixer); }, { 560, 300 } });

    plan.push_back ({ "fx-game", 300, [this]
    {
        controller->getState().setSetting (ids::firstRunDone, 1);
        ui->openFxDrawer ("game", false);
        auto& c = *controller;
        c.addBuiltinFx ("game", "gate");
        c.addPluginFx ("game", demoPlugins()[0]);
        c.addPluginFx ("game", demoPlugins()[4]);
        ui->flushUpdates();

        // One of them "crashed last time".
        auto fx = AppState::getFx (c.getState().findChannel ("game"));

        if (fx.getNumChildren() >= 2)
        {
            auto crashed = fx.getChild (1);
            crashed.setProperty (ids::disabled, 1, nullptr);

            // What AppController does at startup for a slot the crash guard blames.
            Notice notice;
            notice.id = "crash:" + crashed[ids::uid].toString();
            notice.severity = Notice::Severity::warning;
            notice.text = crashed[ids::name].toString() + " crashed and was turned off.";
            notice.actionLabel = "Turn back on";
            notice.action = [&c, uid = crashed[ids::uid].toString()] { c.reenableFx (uid); };
            c.postNotice (std::move (notice));
        }
    } });

    plan.push_back ({ "mixer-rename", 200, [this]
    {
        ui->closeFxDrawer (false);

        for (const auto& notice : std::vector<Notice> (controller->getNotices()))
            controller->dismissNotice (notice.id);

        for (int i = 0; i < 4; ++i)
            controller->addChannel();

        ui->flushUpdates();

        if (auto* strip = ui->getMixer().findStrip ("music"))
            strip->beginRename();
    }, { 860, 540 } });

    plan.push_back ({ "toast", 0, [] {}, { 520, 56 } });

    // From here on: no pictures, just the interactions a person would do, driven with synthetic mouse events.
    plan.push_back ({ {}, 100, [this] { checkMixerControls(); } });
    plan.push_back ({ {}, 100, [this] { checkNoisePanel(); } });
    plan.push_back ({ {}, 100, [this] { checkDrawer(); } });
    plan.push_back ({ {}, 250, [this] { checkDrawerAfter(); } });
    plan.push_back ({ {}, 250, [this] { checkSetupWizardStart(); } });
    plan.push_back ({ {}, 100, [this] { checkSetupWizardFinish(); }, { theme::kDefaultWidth, theme::kDefaultHeight },
                      [this] { auto* sheet = ui->getSetupSheet(); return sheet == nullptr || sheet->getPage() == SetupSheet::Page::done || sheet->getPage() == SetupSheet::Page::failed; } });
    plan.push_back ({ {}, 250, [this] { checkUpdateAndUninstall(); } });
    plan.push_back ({ {}, 250, [this] { checkChannelsAndSheets(); } });
    plan.push_back ({ {}, 250, [this] { checkAfterRemoval(); } });
}

// ---------------------------------------------------------------------------------------------------
// Smoke test
// ---------------------------------------------------------------------------------------------------

void ScreenshotRunner::check (bool ok, const juce::String& what)
{
    Log::info (juce::String (ok ? "  ok    " : "  FAIL  ") + what);

    if (! ok)
        ++failedChecks;
}

void ScreenshotRunner::checkMixerControls()
{
    auto& c = *controller;
    ui->setSize (theme::kDefaultWidth, theme::kDefaultHeight);
    ui->showTab (Tab::mixer);
    ui->getMixer().setMix (Mix::Monitor);
    ui->flushUpdates();

    if (auto* mic = ui->getMixer().findStrip ("mic"))
    {
        check (mic->getFader().getTooltip().contains ("hear yourself"), "mic: the Headphones fader explains the -inf start");
        check (mic->getFader().getDb() <= kMinusInfDb && ! mic->getMuteButton().getToggleState(), "mic: starts at -inf and is not muted");
    }

    auto* strip = ui->getMixer().findStrip ("music");
    check (strip != nullptr, "mixer: music strip exists");

    if (strip == nullptr)
        return;

    auto db = [&] { return AppState::describe (c.getState().findChannel ("music")).monitorDb; };
    auto& fader = strip->getFader();
    const auto cap = fader.getCapBounds().toFloat().getCentre();

    drag (fader, cap, cap + juce::Point<float> (0.0f, -40.0f));
    check (db() > 0.5f, "fader: dragging up raises the level (" + juce::String (db(), 1) + " dB)");

    fader.mouseDoubleClick (makeEvent (fader, fader.getCapBounds().toFloat().getCentre(), false, cap, false, 2));
    check (db() == 0.0f, "fader: double-click returns to 0 dB");

    for (int i = 0; i < 3; ++i)
        wheel (fader, cap, -0.5f);

    check (std::abs (db() + 3.0f) < 0.01f, "fader: three wheel notches down = -3 dB (" + juce::String (db(), 1) + ")");

    const auto capNow = fader.getCapBounds().toFloat().getCentre();
    drag (fader, capNow, capNow + juce::Point<float> (0.0f, 10.0f), true);
    check (db() < -3.0f && db() > -4.5f, "fader: Shift-drag is fine control (" + juce::String (db(), 2) + ")");

    click (strip->getMuteButton());
    check (AppState::describe (c.getState().findChannel ("music")).monitorMuted, "mute button mutes");
    click (strip->getMuteButton());
    check (! AppState::describe (c.getState().findChannel ("music")).monitorMuted, "mute button unmutes");

    click (strip->getLinkButton());
    ui->flushUpdates();
    check (AppState::describe (c.getState().findChannel ("music")).linked, "link toggle links");

    fader.mouseDoubleClick (makeEvent (fader, capNow, false, capNow, false, 2));
    const auto linked = AppState::describe (c.getState().findChannel ("music"));
    check (linked.streamDb == linked.monitorDb, "linked: both mixes follow the fader");
    click (strip->getLinkButton());

    // Stream mix.
    ui->getMixer().setMix (Mix::Stream);
    ui->flushUpdates();
    check (ui->getMixer().getMix() == Mix::Stream, "mix switch selects Stream");
    ui->getMixer().setMix (Mix::Monitor);

    // Keyboard.
    ui->keyPressed (juce::KeyPress ('2', juce::ModifierKeys::ctrlModifier, 0));
    check (ui->getTab() == Tab::apps, "Ctrl+2 opens Apps");
    ui->keyPressed (juce::KeyPress ('3', juce::ModifierKeys::ctrlModifier, 0));
    check (ui->getTab() == Tab::settings, "Ctrl+3 opens Settings");
    ui->keyPressed (juce::KeyPress ('1', juce::ModifierKeys::ctrlModifier, 0));
    check (ui->getTab() == Tab::mixer, "Ctrl+1 opens Mixer");

    // Settings controls write through to the model.
    ui->showTab (Tab::settings);
    std::vector<SegmentedControl*> segments;
    collect<SegmentedControl> (ui->getSettings(), segments);

    if (! segments.empty())
    {
        segments.front()->setSelected (2, true);
        check (c.getState().getLatency() == 512, "settings: Safe latency selected = 512 frames");
        segments.front()->setSelected (1, true);
    }

    if (segments.size() == 2)
    {
        check (segments[1]->getSelected() == 0, "settings: UI size starts on Automatic");
        segments[1]->setSelected (2, true);
        check (std::abs ((double) c.getState().getSetting (ids::uiScale) - 1.25) < 0.001, "settings: picking 125% stores it");
        segments[1]->setSelected (0, true);
        check (UiScale::isAutomatic (c.getState().getSetting (ids::uiScale)), "settings: picking Automatic stores 0");
    }

    std::vector<juce::Slider*> sliders;
    collect<juce::Slider> (ui->getSettings(), sliders);
    check (sliders.size() == 2 && sliders[0]->isVisible() && std::abs (sliders[0]->getValue() - 0.5) < 0.001,
           "settings: the Wave sliders show the device values");

    // Restore mic gain, Use Headroom for all apps, Keep Headroom as the default, Start at login; then the Wave:3 hardware
    // switches (ClipGuard, Re-apply at startup, Safe gain), which are only shown for a Wave:3, and last the experimental opt-in,
    // which is only shown for a model that has one.
    std::vector<SwitchToggle*> switches;
    collect<SwitchToggle> (ui->getSettings(), switches);

    if (switches.size() == 8)
    {
        auto& useHeadroom = *switches[1];
        auto& keepIt = *switches[2];

        check (useHeadroom.getToggleState() && keepIt.getToggleState() && keepIt.isEnabled(),
               "settings: 'Keep Headroom as the default' starts on, under 'Use Headroom for all apps'");

        keepIt.setToggleState (false, juce::dontSendNotification);
        click (keepIt);
        check (! c.getState().getKeepDefault() && c.getState().getMakeDefault(), "settings: switching the lock off is stored");
        keepIt.setToggleState (true, juce::dontSendNotification);
        click (keepIt);
        check (c.getState().getKeepDefault(), "settings: switching it back on is stored");

        useHeadroom.setToggleState (false, juce::dontSendNotification);
        click (useHeadroom);
        ui->flushUpdates();
        check (! keepIt.isEnabled() && keepIt.getToggleState(),
               "settings: the lock is greyed out (and keeps its value) while 'Use Headroom for all apps' is off");

        useHeadroom.setToggleState (true, juce::dontSendNotification);
        click (useHeadroom);
        ui->flushUpdates();
        check (keepIt.isEnabled(), "settings: the lock is available again once 'Use Headroom for all apps' is back on");
    }
    else
    {
        check (false, "settings: expected 8 switches, found " + juce::String ((int) switches.size()));
    }

    // Wave:3 hardware: the demo Wave:3 has to be allowed first; then ClipGuard is a switch, and Safe gain
    // (which needs no permission) takes over the mic's own gain.
    if (switches.size() == 8)
    {
        auto& clipGuard = *switches[4];
        auto& reapply = *switches[5];
        auto& safeGain = *switches[6];

        std::vector<juce::TextButton*> buttons;
        collect<juce::TextButton> (ui->getSettings(), buttons);
        juce::TextButton* allow = nullptr;

        for (auto* button : buttons)
            if (button->getButtonText().startsWith ("Allow hardware controls"))
                allow = button;

        check (allow != nullptr && allow->isVisible() && ! clipGuard.isVisible() && safeGain.isVisible() && ! safeGain.isEnabled(),
               "settings: a Wave:3 without permission offers 'Allow hardware controls'; Safe gain waits for it");

        if (allow != nullptr)
        {
            click (*allow);
            ui->getSettings().syncFromModel();       // the demo backend's own notification comes later, on the message loop
            check (c.getHardwareControls().usbPermission && ! allow->isVisible() && clipGuard.isVisible() && reapply.isVisible()
                       && reapply.getToggleState() && ! clipGuard.getToggleState() && safeGain.isEnabled(),
                   "settings: once allowed, ClipGuard (off) and 'Re-apply at startup' (on) show, and Safe gain can be switched on");

            clipGuard.setToggleState (true, juce::dontSendNotification);
            click (clipGuard);
            ui->getSettings().syncFromModel();
            check (c.getHardwareControls().clipGuard == std::optional<bool> (true) && (int) c.getState().getSetting (ids::clipGuard) == 1
                       && clipGuard.getToggleState(),
                   "settings: the ClipGuard switch reaches the mic and is remembered");

            clipGuard.setToggleState (false, juce::dontSendNotification);
            click (clipGuard);
            ui->getSettings().syncFromModel();
            check (c.getHardwareControls().clipGuard == std::optional<bool> (false) && ! clipGuard.getToggleState(),
                   "settings: and switches off again");
        }

        c.setHardwareMicGain (1.0f);
        safeGain.setToggleState (true, juce::dontSendNotification);
        click (safeGain);
        ui->getSettings().syncFromModel();
        check (c.getSafeGainInfo().on && std::abs (c.getBackendStatus().hardwareMicGain - 0.5412f) < 0.01f && ! sliders[0]->isEnabled(),
               "settings: Safe gain lowers the mic to +24 dB (54 %) and takes over the gain slider");

        safeGain.setToggleState (false, juce::dontSendNotification);
        click (safeGain);
        ui->getSettings().syncFromModel();
        check (! c.getSafeGainInfo().on && std::abs (c.getBackendStatus().hardwareMicGain - 1.0f) < 0.01f && sliders[0]->isEnabled(),
               "settings: switching Safe gain off puts the mic's gain back");

        c.setHardwareMicGain (0.5f);        // the demo mic's own start value again
    }

    ui->showTab (Tab::mixer);
}

void ScreenshotRunner::checkNoisePanel()
{
    auto& c = *controller;
    ui->openFxDrawer ("mic", false);
    auto& drawer = ui->getDrawer();
    drawer.collapseAll();
    drawer.expandBuiltin ("noise");
    ui->flushUpdates();

    std::vector<NoisePanel*> panels;
    collect<NoisePanel> (drawer, panels);
    check (panels.size() == 1, "noise: the panel is in the drawer");

    if (panels.size() != 1)
        return;

    auto& panel = *panels.front();
    auto* noise = dynamic_cast<NoiseRemoval*> (c.getFxProcessor (AppState::describeSlot (AppState::getFx (c.getState().getMicChannel()).getChild (0)).uid));
    check (noise != nullptr, "noise: the mic's first effect is Noise Removal");

    if (noise == nullptr)
        return;

    // Mode: Best is the start; the control writes the parameter.
    check (panel.getModeControl().getSelected() == 1 && noise->getRequestedMode() == NoiseRemoval::Mode::best, "noise: starts on Best");
    panel.getModeControl().setSelected (2, true);
    check (noise->getRequestedMode() == NoiseRemoval::Mode::light, "noise: picking Light sets the mode");
    panel.getModeControl().setSelected (0, true);
    check (noise->getRequestedMode() == NoiseRemoval::Mode::fast, "noise: picking Fast sets the mode");
    panel.getModeControl().setSelected (1, true);
    check (noise->getRequestedMode() == NoiseRemoval::Mode::best, "noise: and Best again");

    // The spectrum runs only while the panel is on screen: the panel switched the audio thread's tap on.
    panel.tickMeter (0.033);
    check (noise->getSpectrumTap().active.load(), "noise: the spectrum tap is on while the panel is showing");

    // Hold to listen: routes the mic to the headphones and away from apps, and lets go again when the drawer closes.
    const auto height = panel.getPreferredHeight();
    panel.getHearButton().setHeld (true, true);
    check (c.isHearingRemoved() && noise->isHearingRemoved(), "noise: holding the button starts listening to what is removed");
    panel.getHearButton().setHeld (false, true);
    check (! c.isHearingRemoved() && ! noise->isHearingRemoved(), "noise: letting go ends it");
    panel.getHearButton().setHeld (true, true);
    ui->closeFxDrawer (false);
    ui->flushUpdates();
    check (! c.isHearingRemoved() && ! noise->isHearingRemoved(), "noise: closing the drawer while holding ends it");
    check (! noise->getSpectrumTap().active.load(), "noise: and switches the spectrum tap off");

    ui->openFxDrawer ("mic", false);
    drawer.collapseAll();
    drawer.expandBuiltin ("noise");
    ui->flushUpdates();
    panels.clear();
    collect<NoisePanel> (drawer, panels);

    if (panels.size() == 1)
    {
        panels.front()->setAdvancedOpen (true);
        ui->flushUpdates();
        check (panels.front()->getPreferredHeight() > height + 100, "noise: Advanced opens more controls and the row grows");
        check (panels.front()->getHeight() == panels.front()->getPreferredHeight(), "noise: the drawer gave the panel its new height");
        panels.front()->setAdvancedOpen (false);
    }
}

void ScreenshotRunner::checkDrawer()
{
    auto& c = *controller;
    ui->openFxDrawer ("mic", false);
    auto& drawer = ui->getDrawer();
    drawer.collapseAll();
    drawer.expandBuiltin ("eq");
    ui->flushUpdates();

    check (ui->isFxDrawerOpen(), "drawer opens on the mic");

    auto slotUid = [&] (int index) { return AppState::describeSlot (AppState::getFx (c.getState().getMicChannel()).getChild (index)).uid; };
    checkEq (drawer, slotUid (1));

    // A knob. (Folding the EQ away destroys its panel: the analyzer must stop with it.)
    drawer.collapseAll();

    if (auto* eq = micEq (c))
        check (! eq->analyzerActive.load(), "eq: the analyzer stops once no EQ view is on screen");

    // The compressor panel: history, meter, curve, Amount and the details.
    checkCompressorPanel (slotUid (2));

    // Reorder: drag the first row's handle below the third.
    drawer.collapseAll();
    ui->flushUpdates();
    std::vector<SlotRow*> rows;
    collect<SlotRow> (drawer, rows);
    std::sort (rows.begin(), rows.end(), [] (SlotRow* a, SlotRow* b) { return a->getY() < b->getY(); });
    check (rows.size() == 4, "drawer: the mic has four effects");

    if (rows.size() == 4)
    {
        const auto firstUid = rows[0]->getUid();
        const auto grip = rows[0]->getGripCentre().toFloat();
        drag (*rows[0], grip, grip + juce::Point<float> (0.0f, 100.0f));
        ui->flushUpdates();

        const auto fx = AppState::getFx (c.getState().getMicChannel());
        check (fx.getChild (2)[ids::uid].toString() == firstUid, "drawer: dragging a row reorders the chain");
    }

    // Add effect: search, then Enter picks the first match.
    drawer.showAddPanel (true);
    ui->flushUpdates();

    std::vector<juce::TextEditor*> editors;
    collect<juce::TextEditor> (drawer, editors);
    check (drawer.isAddPanelOpen() && ! editors.empty(), "drawer: the Add effect panel opens with a search field");

    if (! editors.empty())
    {
        editors.front()->setText ("gate", true);
        editors.front()->keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    }
}

void ScreenshotRunner::checkCompressorPanel (const juce::String& compSlotUid)
{
    auto& c = *controller;
    auto& drawer = ui->getDrawer();

    drawer.collapseAll();
    drawer.expandBuiltin ("comp");
    ui->flushUpdates();

    auto* comp = dynamic_cast<Compressor*> (c.getFxProcessor (compSlotUid));

    if (comp != nullptr)
        comp->applyAmount (Compressor::kDefaultAmount);         // the picture steps may have left it custom

    std::vector<CompPanel*> panels;
    collect<CompPanel> (drawer, panels);
    check (comp != nullptr && panels.size() == 1, "compressor: the panel is shown");

    if (comp == nullptr || panels.size() != 1)
        return;

    auto& panel = *panels.front();
    auto& curve = panel.getCurve();
    auto& amountKnob = panel.getAmountKnob();
    auto value = [&] (const char* id) { return comp->apvts.getRawParameterValue (id)->load(); };

    panel.tickMeter (1.0 / 30.0);
    check (panel.isTapActive() && comp->displayActive.load(), "compressor: the display tap is on while the panel is on screen");
    check (! panel.isDetailsOpen() && ! panel.getDetailsToggle().getToggleState(), "compressor: Details start collapsed");

    std::vector<ParamKnob*> knobs;
    collect<ParamKnob> (panel, knobs);
    auto visibleKnobs = [&]
    {
        int shown = 0;

        for (auto* knob : knobs)
            shown += knob->isVisible() ? 1 : 0;

        return shown;
    };

    check (knobs.size() == 6 && visibleKnobs() == 0, "compressor: the six detail knobs stay hidden until Details opens");

    // Details toggle: shows the six knobs, and the drawer row grows with the panel.
    const int collapsedHeight = panel.getPreferredHeight();
    panel.getDetailsToggle().setToggleState (true, juce::dontSendNotification);
    click (panel.getDetailsToggle());
    ui->flushUpdates();

    std::vector<SlotRow*> slotRows;
    collect<SlotRow> (drawer, slotRows);
    SlotRow* row = nullptr;

    for (auto* candidate : slotRows)
        if (candidate->getUid() == compSlotUid)
            row = candidate;

    check (panel.isDetailsOpen() && visibleKnobs() == 6 && panel.getPreferredHeight() > collapsedHeight,
           "compressor: Details shows the six knobs");
    check (row != nullptr && row->getHeight() == row->getPreferredHeight() && panel.getHeight() == panel.getPreferredHeight(),
           "compressor: the drawer row grows with Details");

    // Amount: 50 % is today's default, and turning it up compresses more.
    check (std::abs (comp->getAmount() - 50.0f) < 0.01f && ! comp->isAmountCustom() && amountKnob.getReadout() == "50%",
           "amount: starts at 50%");

    const auto dial = amountKnob.getDialCentre();
    drag (amountKnob, dial, dial + juce::Point<float> (0.0f, -34.0f));
    check (comp->getAmount() > 60.0f && value ("threshold") < -22.0f && value ("ratio") > 3.4f && value ("makeup") > 4.5f && ! comp->isAmountCustom(),
           "amount: dragging up lowers the threshold and raises ratio and makeup (" + juce::String (comp->getAmount(), 0) + "%, "
               + juce::String (value ("threshold"), 1) + " dB, " + juce::String (value ("ratio"), 1) + ":1)");

    amountKnob.mouseDoubleClick (makeEvent (amountKnob, dial, false, dial, false, 2));
    check (std::abs (comp->getAmount() - 50.0f) < 0.01f && std::abs (value ("threshold") + 20.0f) < 0.05f
               && std::abs (value ("ratio") - 3.0f) < 0.05f && std::abs (value ("makeup") - 4.0f) < 0.05f,
           "amount: double-click returns to the standard 50%");

    // Transfer curve, sideways on the round handle = threshold.
    const float thresholdBefore = value ("threshold");
    const auto handle = curve.kneeHandlePosition();
    drag (curve, handle, handle + juce::Point<float> (24.0f, 0.0f));
    const float thresholdAfter = value ("threshold");
    const float expectedShift = 24.0f / curve.getPlotBounds().getWidth() * 60.0f;          // the handle follows the pointer
    check (std::abs (thresholdAfter - thresholdBefore - expectedShift) < 0.6f,
           "curve: dragging the knee sideways moves the threshold (" + juce::String (thresholdBefore, 1) + " -> " + juce::String (thresholdAfter, 1) + " dB)");
    check (comp->isAmountCustom() && amountKnob.getReadout() == "Custom", "amount: editing a detail says Custom");

    const auto handleAgain = curve.kneeHandlePosition();
    drag (curve, handleAgain, handleAgain + juce::Point<float> (20.0f, 0.0f), true);
    check (value ("threshold") > thresholdAfter && value ("threshold") < thresholdAfter + 3.0f,
           "curve: Shift-drag on the knee is fine control (" + juce::String (value ("threshold") - thresholdAfter, 2) + " dB)");

    const auto knee = curve.kneeHandlePosition();
    curve.mouseDoubleClick (makeEvent (curve, knee, false, knee, false, 2));
    check (std::abs (value ("threshold") + 20.0f) < 0.05f, "curve: double-click on the knee restores the threshold");

    // Up or down on the line = ratio; down is firmer.
    const float ratioBefore = value ("ratio");
    const auto grab = curve.ratioGrabPoint();
    drag (curve, grab, grab + juce::Point<float> (0.0f, 30.0f));
    const float ratioFirmer = value ("ratio");
    check (ratioFirmer > ratioBefore + 1.0f,
           "curve: dragging the line down raises the ratio (" + juce::String (ratioBefore, 1) + " -> " + juce::String (ratioFirmer, 1) + ")");

    const auto grabAgain = curve.ratioGrabPoint();
    drag (curve, grabAgain, grabAgain + juce::Point<float> (0.0f, -60.0f));
    check (value ("ratio") < ratioFirmer - 1.0f && value ("ratio") >= 1.0f, "curve: dragging it up lowers the ratio (" + juce::String (value ("ratio"), 1) + ")");

    const auto line = curve.ratioGrabPoint();
    curve.mouseDoubleClick (makeEvent (curve, line, false, line, false, 2));
    check (std::abs (value ("ratio") - 3.0f) < 0.05f, "curve: double-click on the line restores the ratio");

    // Amount takes a custom setting back.
    amountKnob.mouseDoubleClick (makeEvent (amountKnob, dial, false, dial, false, 2));
    check (! comp->isAmountCustom() && amountKnob.getReadout() == "50%", "amount: double-click clears Custom");

    // A detail knob is another way to make it Custom.
    if (! knobs.empty())
    {
        auto& first = *knobs.front();
        const auto mid = juce::Point<float> ((float) first.getWidth() * 0.5f, 26.0f);
        const float before = value ("threshold");
        drag (first, mid, mid + juce::Point<float> (0.0f, -30.0f));
        check (value ("threshold") > before + 2.0f && comp->isAmountCustom(),
               "knob: dragging a detail knob moves its value (" + juce::String (before, 1) + " -> " + juce::String (value ("threshold"), 1) + ") and sets Custom");

        first.mouseDoubleClick (makeEvent (first, mid, false, mid, false, 2));
        check (std::abs (value ("threshold") + 20.0f) < 0.1f, "knob: double-click restores the default");
    }

    // The tap runs only while somebody looks at the panel.
    Log::info ("  compressor tap: " + juce::String ((juce::int64) comp->getDisplayRecordsWritten()) + " records written, "
               + juce::String ((juce::int64) comp->getDisplayRecordsDropped()) + " dropped while the panel was on screen");
    drawer.collapseAll();
    ui->flushUpdates();
    check (! comp->displayActive.load(), "compressor: the display tap goes off when the panel is collapsed");

    comp->applyAmount (Compressor::kDefaultAmount);         // factory state again for the checks that follow
}

// The EQ: nodes, wheel, double-clicks, solo, the expanded editor.
void ScreenshotRunner::checkEq (FxDrawer& drawer, const juce::String& eqSlotUid)
{
    auto& c = *controller;
    auto* eq = dynamic_cast<Equalizer*> (c.getFxProcessor (eqSlotUid));
    auto* panel = findEqPanel (drawer);

    check (eq != nullptr && panel != nullptr, "eq: the panel is there");

    if (eq == nullptr || panel == nullptr)
        return;

    auto value = [eq] (const char* id) { return eq->apvts.getRawParameterValue (id)->load(); };
    auto& graph = panel->getGraph();

    panel->getFeed().setForceLive (true);       // (nothing is "showing" in screenshot mode)
    panel->getFeed().tick();

    check (graph.getNumCurvePoints() > 100 && graph.getPlotBounds().getWidth() > 250.0f, "eq: the graph has a plot and curves");
    check (eq->analyzerActive.load(), "eq: the analyzer runs while the graph is on screen");

    // Drag a waiting band (band 2, a low shelf sitting off at 100 Hz) up and to the right: it switches itself on and follows.
    check (value ("b2On") < 0.5f, "eq: band 2 is waiting");
    const auto node = graph.getNodePosition (1);
    drag (graph, node, node + juce::Point<float> (40.0f, -24.0f));
    check (value ("b2On") > 0.5f && value ("b2Gain") > 1.0f && value ("b2Freq") > 150.0f,
           "eq: dragging a node switches the band on and changes frequency and gain (" + juce::String (value ("b2Gain"), 1) + " dB, "
               + juce::String (value ("b2Freq"), 0) + " Hz)");

    // Shift = fine: the same move changes it a tenth as much.
    {
        const float before = value ("b2Freq");
        const auto at = graph.getNodePosition (1);
        drag (graph, at, at + juce::Point<float> (20.0f, 0.0f), true);
        const float ratio = value ("b2Freq") / before;
        check (ratio > 1.0f && ratio < 1.15f, "eq: Shift-drag is fine control (x" + juce::String (ratio, 3) + " for 20 px)");
    }

    // The wheel over a node changes Q.
    {
        const float before = value ("b2Q");
        wheel (graph, graph.getNodePosition (1), 1.0f);
        check (value ("b2Q") > before * 1.05f, "eq: the wheel over a node widens or narrows it (Q " + juce::String (before, 2) + " -> " + juce::String (value ("b2Q"), 2) + ")");
    }

    // Double-click on a node: flat.
    doubleClick (graph, graph.getNodePosition (1));
    check (value ("b2Gain") == 0.0f, "eq: double-click on a node resets its gain");

    // Double-click on empty graph: the nearest free band (band 4, waiting at 1 kHz) comes on there, as a peak.
    {
        const juce::Point<float> at (graph.xForFreq (2000.0f), graph.yForDb (-6.0f));
        check (value ("b4On") < 0.5f, "eq: band 4 is waiting");
        doubleClick (graph, at);
        check (value ("b4On") > 0.5f && value ("b4Type") == 0.0f && std::abs (value ("b4Freq") - 2000.0f) < 150.0f && std::abs (value ("b4Gain") + 6.0f) < 0.6f,
               "eq: double-click on empty space adds a peak band there (" + juce::String (value ("b4Freq"), 0) + " Hz, " + juce::String (value ("b4Gain"), 1) + " dB)");
    }

    // Alt + drag solos the band while the button is held.
    {
        const auto at = graph.getNodePosition (3);
        graph.mouseDown (makeEvent (graph, at, false, at, false, 1, true));
        check (eq->getSoloBand() == 3, "eq: Alt + press on a node solos that band");
        graph.mouseDrag (makeEvent (graph, at + juce::Point<float> (5.0f, 0.0f), false, at, true, 1, true));
        graph.mouseUp (makeEvent (graph, at, false, at, true, 1, true));
        check (eq->getSoloBand() == -1, "eq: releasing the mouse ends the solo");
    }

    // The band menu: another type, another slope, off and on again.
    {
        graph.applyMenuResult (3, 100 + 3);                  // band 4 -> Low cut
        check (value ("b4Type") == 3.0f, "eq: the band menu changes the type");
        graph.applyMenuResult (3, 200 + 2);
        check (value ("b4Slope") == 2.0f, "eq: and the cut slope");
        graph.applyMenuResult (3, 1);
        check (value ("b4On") < 0.5f, "eq: Turn off switches the band off");
        graph.applyMenuResult (3, 100 + 5);                  // picking a type for a band that is off switches it on
        check (value ("b4On") > 0.5f && value ("b4Type") == 5.0f, "eq: choosing a type for a waiting band switches it on");
    }

    // The expanded editor: open, drag there, close with Esc.
    click (panel->getExpandButton());
    ui->flushUpdates();
    check (panel->isExpandedOpen() && panel->getSheet() != nullptr && panel->getSheet()->isVisible(), "eq: Expand opens the editor");

    if (auto* sheet = panel->getSheet())
    {
        check (sheet->getParentComponent() == ui.get() && sheet->getBounds() == ui->getLocalBounds(), "eq: the editor covers the window");
        check (sheet->getPanelBounds().getWidth() >= 720 && sheet->getPanelBounds().getHeight() >= 460, "eq: the editor is big enough to work in");

        auto& big = sheet->getGraph();
        check (big.getPlotBounds().getWidth() > 700.0f, "eq: the big graph is wide (" + juce::String (big.getPlotBounds().getWidth(), 0) + " px)");

        const auto at = big.getNodePosition (5);
        drag (big, at, at + juce::Point<float> (30.0f, -20.0f));
        check (value ("b6On") > 0.5f && value ("b6Gain") > 0.5f, "eq: nodes drag in the big graph too");

        // The header switches: spectrum source and dB range.
        std::vector<SegmentedControl*> switches;
        collect<SegmentedControl> (*sheet, switches);
        check (switches.size() == 2, "eq: the editor has a Post / Pre switch and a range switch");

        if (switches.size() == 2)
        {
            switches[0]->setSelected (1, true);
            check (big.getSpectrumSource() == EqAnalyzerFeed::Source::pre, "eq: the Pre switch shows the spectrum before the EQ");
            switches[0]->setSelected (0, true);
            switches[1]->setSelected (1, true);
            check (big.getRangeDb() == 12.0f, "eq: the range switch changes the dB range");
            switches[1]->setSelected (2, true);
            check (big.getRangeDb() == 18.0f, "eq: and back");
        }

        // Numbers can be typed into the band strip: 3 kHz, -4.5 dB and a Q of 2 on band 6.
        check (sheet->enterValue (5, EqValueKind::frequency, "3k") && std::abs (value ("b6Freq") - 3000.0f) < 1.0f, "eq: typing 3k sets 3000 Hz");
        sheet->enterValue (5, EqValueKind::gain, "-4.5 dB");
        check (std::abs (value ("b6Gain") + 4.5f) < 0.001f, "eq: typing -4.5 dB sets the gain");
        sheet->enterValue (5, EqValueKind::q, "2");
        check (std::abs (value ("b6Q") - 2.0f) < 0.001f, "eq: typing 2 sets Q");
        sheet->enterValue (5, EqValueKind::frequency, "nonsense");
        check (std::abs (value ("b6Freq") - 3000.0f) < 1.0f, "eq: typing something that is not a number changes nothing");
        sheet->enterValue (5, EqValueKind::frequency, "99999");
        check (value ("b6Freq") == 20000.0f, "eq: typing too much stops at the top of the range");

        std::vector<juce::TextEditor*> editors;
        collect<juce::TextEditor> (*sheet, editors);
        check (editors.empty(), "eq: no text field is left open");

        check (sheet->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)), "eq: Esc is handled by the editor");
        check (! panel->isExpandedOpen(), "eq: Esc closes the editor");
    }

    ui->flushUpdates();
    check (graph.isSpectrumLive (true), "eq: the small graph is live again once the editor is gone");
}

// How much one repaint of the EQ graph costs (the analyzer repaints it at 30 fps while it is on screen).
//
// A shared CI VM is not a stopwatch: one run painted the big graph in 7.8 ms and the next, on the same commit, in 25.3 ms
// (everything on that machine ran about 3 times slower for seconds at a time), so a limit in milliseconds fails at random and
// takes the whole screenshot step with it. The guard therefore compares the paint with a reference that is painted alternately
// with it, in the same kind of image: a background blit, an area and nine stroked curves with as many points as the graph's.
// A slow machine slows both alike, so the ratio stays where it is whatever the machine is doing. The milliseconds are still logged.
void ScreenshotRunner::measureEqPaint()
{
    auto* panel = findEqPanel (ui->getDrawer());

    if (panel == nullptr)
        return;

    const bool wasOpen = panel->isExpandedOpen();

    if (! wasOpen)
        panel->openExpanded();

    ui->flushUpdates();

    struct PaintCost
    {
        double paintMs = 0.0;           // one repaint of the graph (median of the rounds)
        double referenceMs = 0.0;       // one reference paint of the same size (median)
        double ratio = 0.0;             // paint / reference, taken round by round (median)
    };

    auto median = [] (std::vector<double> values)
    {
        std::sort (values.begin(), values.end());
        return values[values.size() / 2];
    };

    auto measure = [&] (EqGraph& graph, float scale)
    {
        // Both are painted into a software image, which is what the analyzer's repaints cost on Linux. (Windows would otherwise
        // only record the drawing commands here, and the numbers would say nothing.)
        const auto makeImage = [&]
        {
            return juce::Image (juce::Image::ARGB, juce::roundToInt ((float) graph.getWidth() * scale),
                                juce::roundToInt ((float) graph.getHeight() * scale), true, juce::SoftwareImageType());
        };

        auto image = makeImage();
        juce::Graphics g (image);
        g.addTransform (juce::AffineTransform::scale (scale));

        // The reference stands in for the graph: the cached background blitted over the whole graph, an area filled, and nine
        // curves (eight bands and the sum) stroked, each with as many points as the graph's curves have.
        const float width = (float) graph.getWidth(), height = (float) graph.getHeight();
        const int points = std::max (2, graph.getNumCurvePoints());
        auto backdrop = makeImage();
        juce::Graphics (backdrop).fillAll (juce::Colours::darkgrey);

        juce::Path curve, area;

        for (int i = 0; i < points; ++i)
        {
            const float t = (float) i / (float) (points - 1);
            const juce::Point<float> p (t * width, height * (0.5f + 0.2f * std::sin (t * 37.0f) * std::cos (t * 5.0f)));

            if (i == 0)
                curve.startNewSubPath (p);
            else
                curve.lineTo (p);
        }

        area = curve;
        area.lineTo (width, height);
        area.lineTo (0.0f, height);
        area.closeSubPath();

        const auto paintGraph = [&] { graph.paintEntireComponent (g, true); };
        const auto paintRef = [&]
        {
            g.drawImage (backdrop, juce::Rectangle<float> (width, height));
            g.setColour (juce::Colours::white.withAlpha (0.26f));
            g.fillPath (area);

            for (int k = 0; k < 9; ++k)
            {
                g.setColour (juce::Colours::white.withAlpha (0.6f));
                g.strokePath (curve, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded),
                              juce::AffineTransform::translation (0.0f, (float) (k - 4) * 0.03f * height));
            }
        };

        const auto perFrameMs = [] (const auto& paintOne, int frames)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();

            for (int i = 0; i < frames; ++i)
                paintOne();

            return (juce::Time::getMillisecondCounterHiRes() - t0) / (double) frames;
        };

        perFrameMs (paintGraph, 5);                     // warm-up: builds the cached background
        perFrameMs (paintRef, 5);

        constexpr int rounds = 5, framesPerRound = 10;
        std::vector<double> paintMs, referenceMs, ratios;

        for (int round = 0; round < rounds; ++round)    // alternating, so that a slowdown that comes and goes hits both
        {
            const double a = perFrameMs (paintGraph, framesPerRound);
            const double b = perFrameMs (paintRef, framesPerRound);
            paintMs.push_back (a);
            referenceMs.push_back (b);
            ratios.push_back (a / std::max (b, 1.0e-6));
        }

        return PaintCost { median (paintMs), median (referenceMs), median (ratios) };
    };

    // The default window, and a big one (the editor grows with the window).
    for (const auto windowSize : { juce::Point<int> (theme::kDefaultWidth, theme::kDefaultHeight), juce::Point<int> (1920, 1080) })
    {
        ui->setSize (windowSize.x, windowSize.y);
        ui->flushUpdates();

        if (auto* sheet = panel->getSheet())
        {
            auto& big = sheet->getGraph();
            const auto size = juce::String (big.getWidth()) + "x" + juce::String (big.getHeight());
            const auto at1 = measure (big, 1.0f), at2 = measure (big, 2.0f), compact = measure (panel->getGraph(), 2.0f);
            Log::info ("  eq paint time per frame, window " + juce::String (windowSize.x) + "x" + juce::String (windowSize.y) + ": expanded graph " + size
                       + " at 1x " + juce::String (at1.paintMs, 2) + " ms, at 2x " + juce::String (at2.paintMs, 2) + " ms; inline graph at 2x "
                       + juce::String (compact.paintMs, 2) + " ms (" + juce::String (big.getNumCurvePoints()) + " curve points); "
                       + "a reference paint of the same size takes " + juce::String (at2.referenceMs, 2) + " ms at 2x, so the graph costs "
                       + juce::String (at2.ratio, 2) + " of it");
            check (at2.ratio < kMaxEqPaintCost,
                   "eq: repainting the expanded graph (" + size + ") at 2x costs " + juce::String (at2.ratio, 2) + " reference paints (fails above "
                   + juce::String (kMaxEqPaintCost, 1) + ")");
        }
    }

    ui->setSize (theme::kDefaultWidth, theme::kDefaultHeight);

    if (! wasOpen)
        panel->closeExpanded();
}

void ScreenshotRunner::checkDrawerAfter()
{
    auto& c = *controller;
    auto& drawer = ui->getDrawer();
    const auto fx = AppState::getFx (c.getState().getMicChannel());
    check (fx.getNumChildren() == 5 && fx.getChild (4)[ids::builtinId].toString() == "gate",
           "drawer: picking Noise Gate from the list adds it (" + juce::String (fx.getNumChildren()) + " effects)");
    check (! drawer.isAddPanelOpen(), "drawer: the panel closes after picking");

    // Mic presets replace the chain; the drawer keeps up without leaving panels behind.
    drawer.collapseAll();
    drawer.expandBuiltin ("noise");
    c.applyMicPreset ("broadcast");
    ui->flushUpdates();
    check (AppState::getFx (c.getState().getMicChannel()).getNumChildren() == 5, "preset: Broadcast gives five effects");

    {
        std::vector<Dropdown*> dropdowns;
        collect<Dropdown> (drawer, dropdowns);
        check (! dropdowns.empty() && dropdowns.front()->getSelectedKey() == "broadcast", "preset: the menu shows the applied preset");
    }
    c.applyMicPreset ("off");
    ui->flushUpdates();
    check (AppState::getFx (c.getState().getMicChannel()).getNumChildren() == 0, "preset: Off removes them all");
    c.applyMicPreset ("clean");
    ui->flushUpdates();

    c.addBuiltinFx (values::micChannelId, "gate");
    ui->flushUpdates();

    {
        std::vector<Dropdown*> dropdowns;
        collect<Dropdown> (drawer, dropdowns);
        check (! dropdowns.empty() && dropdowns.front()->getSelectedKey() == "custom", "preset: adding an effect afterwards says Custom");
    }

    c.applyMicPreset ("clean");
    ui->flushUpdates();

    ui->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
    check (! ui->isFxDrawerOpen(), "Esc closes the drawer");
}

void ScreenshotRunner::checkChannelsAndSheets()
{
    // Rename through the editor a person would type into.
    if (auto* strip = ui->getMixer().findStrip ("system"))
    {
        strip->beginRename();
        std::vector<juce::TextEditor*> editors;
        collect<juce::TextEditor> (*strip, editors);
        check (editors.size() == 1, "rename: the name turns into a text field");

        if (! editors.empty())
        {
            editors.front()->setText ("Browser", false);

            if (editors.front()->onReturnKey)
                editors.front()->onReturnKey();
        }
    }

    // Adding a channel puts a new strip on screen and starts its rename.
    {
        const auto before = ui->getMixer().getNumStrips();
        ui->getMixer().addChannelFromUi();
        ui->flushUpdates();
        check (ui->getMixer().getNumStrips() == before + 1, "add channel: one more strip");

        auto* added = ui->getMixer().getLastStrip();
        check (added != nullptr && added->getWidth() == theme::kStripWidth && added->isVisible(), "add channel: the new strip has been laid out");

        if (added != nullptr)
        {
            std::vector<juce::TextEditor*> editors;
            collect<juce::TextEditor> (*added, editors);
            check (editors.size() == 1, "add channel: rename starts on the new strip");
        }
    }

    // Remove a channel through the confirmation sheet.
    ui->showRemoveChannelSheet ("chat");
    ui->flushUpdates();
    check (ui->isSheetOpen(), "remove: the confirmation sheet opens");

    std::vector<juce::TextButton*> buttons;
    collect<juce::TextButton> (*ui, buttons);

    for (auto* button : buttons)
        if (button->getButtonText() == "Remove")
            button->triggerClick();

    // First-run sheet: Done marks it as seen.
    controller->getState().setSetting (ids::firstRunDone, 0);
}

// ---------------------------------------------------------------------------------------------------
// Input channels and several outputs
// ---------------------------------------------------------------------------------------------------

namespace
{
    const char* const kDemoWebcam = "alsa_input.usb-Generic_USB_Webcam_DEMO-02.analog-stereo";
    const char* const kDemoMissing = "alsa_input.usb-Blue_Microphones_Yeti_X_2113-00.analog-stereo";
    const char* const kDemoWaveOut = "alsa_output.usb-Elgato_Systems_Elgato_Wave_3_DEMO-00.analog-stereo";
    const char* const kDemoHdmi = "alsa_output.pci-0000_01_00.1.hdmi-stereo";

    playthrough::Panel* findPlayThroughPanel (juce::Component& root)
    {
        std::vector<playthrough::Panel*> panels;
        collect<playthrough::Panel> (root, panels);
        return panels.empty() ? nullptr : panels.front();
    }

    // A click the way a pointer makes it: press and release in the middle of the component.
    void clickRow (juce::Component& row)
    {
        const auto at = row.getLocalBounds().getCentre().toFloat();
        row.mouseDown (makeEvent (row, at, false, at, false));
        row.mouseUp (makeEvent (row, at, false, at, false));
    }
}

void ScreenshotRunner::addDemoInputs()
{
    auto& c = *controller;
    ui->setSize (theme::kDefaultWidth, theme::kDefaultHeight);
    ui->showTab (Tab::mixer);
    ui->getMixer().setMix (Mix::Monitor);
    ui->flushUpdates();

    const auto before = ui->getMixer().getNumStrips();

    // A microphone that is there: picked in the ghost strip's menu ("Input..." and its device); a mic that is not.
    ui->getMixer().addInputFromUi (kDemoWebcam);
    demoWebcamChannel = ui->getMixer().getLastStrip() != nullptr ? ui->getMixer().getLastStrip()->getChannelId() : juce::String();
    demoMissingChannel = c.addInputChannel (kDemoMissing, "Yeti X");
    c.setMixOutputs (Mix::Monitor, juce::StringArray { kDemoWaveOut, kDemoHdmi });
    ui->flushUpdates();

    check (ui->getMixer().getNumStrips() == before + 2, "inputs: two more strips, one for each input channel");

    auto* webcam = ui->getMixer().findStrip (demoWebcamChannel);
    auto* missing = ui->getMixer().findStrip (demoMissingChannel);
    check (webcam != nullptr && missing != nullptr, "inputs: both strips are on screen");

    if (webcam != nullptr)
    {
        check (webcam->getLineText() == "USB Webcam Mic", "inputs: the strip says which device it listens to (" + webcam->getLineText() + ")");
        check (webcam->getFader().getDb() <= kMinusInfDb && ! webcam->getMuteButton().getToggleState(), "inputs: a new input channel starts silent in the headphones (-inf), not muted");
        check (c.engineIndexFor (demoWebcamChannel) == kInputChannel0, "inputs: the first input channel is the first input slot of the engine");
    }

    if (missing != nullptr)
        check (missing->getLineText() == "Not connected", "inputs: a device that is not there says \"Not connected\" (" + missing->getLineText() + ")");

    check (ui->getMixer().getAddStrip().isVisible(), "inputs: the ghost strip is still there");

    // "Change input...": the same channel on another device, and back.
    c.setChannelDevice (demoMissingChannel, kDemoWebcam);
    ui->flushUpdates();
    check (missing != nullptr && missing->getLineText() == "USB Webcam Mic", "inputs: Change input moves the channel to the other device");
    c.setChannelDevice (demoMissingChannel, kDemoMissing);
    ui->flushUpdates();
    check (missing != nullptr && missing->getLineText() == "Not connected", "inputs: and the device that went away shows as not connected again");

    // The control in the header and the master strip say what the Headphones mix plays to.
    check (ui->getMixer().getOutputDropdown().getSelectedText() == "Wave:3 Headphones + 1 more",
           "outputs: the control says Wave:3 and one more (" + ui->getMixer().getOutputDropdown().getSelectedText() + ")");
    check (ui->getMixer().getMasterStrip().getLineText() == "Wave:3 +1", "outputs: the master strip says Wave:3 +1 (" + ui->getMixer().getMasterStrip().getLineText() + ")");
}

void ScreenshotRunner::openPlayThrough (Mix mix)
{
    ui->getMixer().setMix (mix);
    ui->flushUpdates();
    auto& box = ui->getMixer().getOutputDropdown();

    if (box.onOpenRequested)
        box.onOpenRequested (box);
}

void ScreenshotRunner::checkHeadphonePlayThrough()
{
    auto& c = *controller;
    auto& state = c.getState();
    auto* panel = findPlayThroughPanel (*ui);
    check (panel != nullptr, "play through: the list opens under the control");

    if (panel == nullptr)
        return;

    auto list = [&] { return state.getOutputs (Mix::Monitor).joinIntoString ("|"); };

    check (panel->getNumRows() == 3, "play through: Automatic and the two devices (" + juce::String (panel->getNumRows()) + " rows)");

    if (panel->getNumRows() != 3)
        return;

    check (panel->getRowText (0).startsWith ("Automatic") && panel->getRowText (1) == "Elgato Wave:3 Headphones" && panel->getRowText (2) == "HDMI Audio",
           "play through: the rows carry the model-aware names (" + panel->getRowText (0) + ", " + panel->getRowText (1) + ", " + panel->getRowText (2) + ")");
    check (! panel->isRowTicked (0) && panel->isRowTicked (1) && panel->isRowTicked (2), "play through: the two devices are ticked, Automatic is not");

    // Untick the Wave: HDMI is the last device and cannot go; a click on it does nothing.
    clickRow (*panel->getRow (1));
    check (list() == kDemoHdmi, "play through: a click unticks a device (" + list() + ")");
    check (panel->isRowTicked (2) && ! panel->isRowTicked (1), "play through: the list follows at once");

    clickRow (*panel->getRow (2));
    check (list() == kDemoHdmi, "play through: the last device of the Headphones mix stays ticked (" + list() + ")");

    // Automatic is a choice of its own; the keyboard ticks too.
    panel->getRow (0)->keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
    check (list() == "auto" && panel->isRowTicked (0) && ! panel->isRowTicked (2), "play through: Space ticks Automatic, which replaces the others (" + list() + ")");

    // A device next to Automatic: Automatic becomes the device it means, so nothing stops playing.
    clickRow (*panel->getRow (2));
    check (list() == juce::String (kDemoWaveOut) + "|" + kDemoHdmi, "play through: a device ticked next to Automatic keeps what Automatic meant (" + list() + ")");
    check (panel->isRowTicked (1) && panel->isRowTicked (2) && ! panel->isRowTicked (0), "play through: Wave and HDMI are ticked, Automatic is not");

    // A device that was ticked and is not there stays in the list, so that it can be unticked.
    c.setMixOutputs (Mix::Monitor, juce::StringArray { kDemoWaveOut, "alsa_output.usb-Desk_Speakers_X1-00.analog-stereo" });
    panel->refresh();
    check (panel->getNumRows() == 4 && panel->isRowMissing (3) && panel->isRowTicked (3), "play through: a ticked device that is gone is listed as not connected");
    clickRow (*panel->getRow (3));
    check (list() == kDemoWaveOut, "play through: and can be unticked (" + list() + ")");

    c.setMixOutputs (Mix::Monitor, juce::StringArray { kDemoWaveOut, kDemoHdmi });
    ui->flushUpdates();
    check (ui->getMixer().getOutputDropdown().getSelectedText() == "Wave:3 Headphones + 1 more", "play through: the closed control follows the list");

    check (playthrough::isOpen (*ui), "play through: it stays open while the ticks change");
    playthrough::close (*ui);
    check (! playthrough::isOpen (*ui) && findPlayThroughPanel (*ui) == nullptr, "play through: closing it takes the panel away");
}

void ScreenshotRunner::checkStreamPlayThrough()
{
    auto& state = controller->getState();
    check (ui->getMixer().getMix() == Mix::Stream, "play through: the Stream mix has its own list");
    check (ui->getMixer().getOutputDropdown().getSelectedText() == "none", "play through: the Stream mix plays to no other device to begin with");

    auto* panel = findPlayThroughPanel (*ui);
    check (panel != nullptr, "play through: the Stream list opens too");

    if (panel == nullptr)
        return;

    check (panel->getNumRows() == 2 && panel->getRowKey (0) == kDemoWaveOut, "play through: the Stream list has the devices and no Automatic (" + juce::String (panel->getNumRows()) + " rows)");

    if (panel->getNumRows() != 2)
        return;

    clickRow (*panel->getRow (1));
    check (state.getOutputs (Mix::Stream).joinIntoString ("|") == kDemoHdmi, "play through: ticking a device adds it to the Stream mix");
    ui->flushUpdates();
    check (ui->getMixer().getOutputDropdown().getSelectedText() == "HDMI Audio", "play through: the Stream control names it (" + ui->getMixer().getOutputDropdown().getSelectedText() + ")");
    check (ui->getMixer().getMasterStrip().getLineText() == "Sent to OBS +1", "play through: the master strip still says OBS, and one more (" + ui->getMixer().getMasterStrip().getLineText() + ")");
    check (state.getOutputs (Mix::Monitor).joinIntoString ("|") == juce::String (kDemoWaveOut) + "|" + kDemoHdmi, "play through: the Headphones list is not touched by the Stream list");

    clickRow (*panel->getRow (1));
    check (state.getOutputs (Mix::Stream).isEmpty(), "play through: and unticking it takes it away; the Stream mix may play to nothing else");

    // The Esc key closes it too, once the key has been handled: the layer over the window gets it from the row the keyboard is on.
    panel->getRow (0)->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
    auto* layer = panel->getParentComponent();
    check (layer != nullptr && layer->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)), "play through: Esc is handled by the layer over the window");
    playthrough::close (*ui);
}

// Back to a plain mixer, the way a person takes an input channel away: through the confirmation sheet.
void ScreenshotRunner::removeDemoInputs()
{
    auto& c = *controller;
    ui->getMixer().setMix (Mix::Monitor);
    c.setMixOutputs (Mix::Monitor, juce::StringArray { "auto" });
    c.setMixOutputs (Mix::Stream, {});

    ui->showRemoveChannelSheet (demoWebcamChannel);
    ui->flushUpdates();
    check (ui->isSheetOpen(), "inputs: removing an input channel asks first");

    std::vector<juce::TextButton*> buttons;
    collect<juce::TextButton> (*ui, buttons);

    for (auto* button : buttons)
        if (button->getButtonText() == "Remove")
            click (*button);

    c.removeChannel (demoMissingChannel);
    ui->flushUpdates();

    check (ui->getMixer().findStrip (demoWebcamChannel) == nullptr && ui->getMixer().findStrip (demoMissingChannel) == nullptr, "inputs: both strips are gone");
    check (c.getState().getInputEndpoints().empty() && c.getBackendStatus().inputChannels.empty(), "inputs: the model and the backend have no input channel left");
    check (c.getState().getOutputDevice() == "auto", "outputs: the Headphones mix is back on Automatic");
}

// ---------------------------------------------------------------------------------------------------
// The setup wizard, the update sheet and the uninstall sheet: what a person does, with synthetic clicks
// ---------------------------------------------------------------------------------------------------

void ScreenshotRunner::checkSetupWizardStart()
{
    auto& c = *controller;
    ui->setSize (theme::kDefaultWidth, theme::kDefaultHeight);
    ui->showTab (Tab::mixer);
    ui->hideSheet();
    setInstalled (false);
    openedProgram = juce::File();

    // A first run: the shipped channels and the settings a new state.xml has (the tests before this one changed some of them).
    {
        auto& state = c.getState();

        for (const auto& info : state.getChannelInfos())
            if (! info.isMic() && info.id != "system" && info.id != "game" && info.id != "music" && info.id != "chat")
                c.removeChannel (info.id);

        state.setSetting (ids::clipGuard, -1);
        state.setSetting (ids::safeGain, 0);
        state.setSetting (ids::startAtLogin, 1);
        state.setSetting (ids::makeDefault, 1);
        state.setSetting (ids::keepDefault, 1);
        c.applyMicPreset ("clean");
    }

    ui->showSetupWizard (SetupSheet::Mode::firstRun);
    ui->flushUpdates();
    auto* sheet = ui->getSetupSheet();
    check (sheet != nullptr && ui->isSheetOpen(), "setup: the wizard opens as a sheet");

    if (sheet == nullptr)
        return;

    const int steps = sheet->getStepCount();
    check (sheet->getPage() == SetupSheet::Page::welcome && sheet->getStepNumber() == 1, "setup: it starts on the welcome page");
    check (steps == 6, "setup: six steps with the Wave:3 controls (" + juce::String (steps) + ")");
    check (sheet->getBackButton().getButtonText() == "Not now" && sheet->getNextButton().getButtonText() == "Get started", "setup: welcome says Not now / Get started");

    // Next and Back move between the pages.
    click (sheet->getNextButton());
    check (sheet->getPage() == SetupSheet::Page::devices && sheet->getStepNumber() == 2, "setup: Next goes to the devices");
    check (sheet->getBackButton().getButtonText() == "Back", "setup: Back is called Back after the first page");
    click (sheet->getBackButton());
    check (sheet->getPage() == SetupSheet::Page::welcome, "setup: Back goes back");
    click (sheet->getNextButton());

    // Devices: live lists; a choice reaches the model at once; a mic that is not a Wave:3 takes the Wave:3 step away.
    {
        std::vector<Dropdown*> dropdowns;
        collect<Dropdown> (*sheet->getPageComponent(), dropdowns);
        check (dropdowns.size() == 2, "setup: a mic list and a headphones list");

        if (dropdowns.size() == 2)
        {
            check (dropdowns[0]->getSelectedKey() == "auto" && dropdowns[1]->getSelectedKey() == "auto", "setup: both start on Automatic (a Wave is picked for you)");

            juce::String webcam;

            for (const auto& input : c.getBackend().getInputs())
                if (! input.isWave)
                    webcam = input.id;

            check (webcam.isNotEmpty(), "setup: the demo has a second microphone to pick");

            if (webcam.isNotEmpty() && dropdowns[0]->onChange)
            {
                dropdowns[0]->onChange (webcam);
                ui->flushUpdates();
                check (c.getState().getInputDevice() == webcam && sheet->getChoices().inputDevice == webcam, "setup: the picked mic is in the model");
                check (sheet->getStepCount() == 5, "setup: without a Wave:3 there is no hardware step (" + juce::String (sheet->getStepCount()) + ")");

                dropdowns[0]->onChange ("auto");
                ui->flushUpdates();
                check (c.getState().getInputDevice() == "auto" && sheet->getStepCount() == 6, "setup: Automatic brings the Wave:3 step back");
            }
        }
    }

    click (sheet->getNextButton());
    check (sheet->getPage() == SetupSheet::Page::channels, "setup: channels page");

    // Channels: untick Game, rename Music, add one and name it, and a wizard with nothing ticked cannot go on.
    {
        std::vector<TickBox*> ticks;
        std::vector<juce::TextEditor*> editors;
        collect<TickBox> (*sheet->getPageComponent(), ticks);
        collect<juce::TextEditor> (*sheet->getPageComponent(), editors);
        check (ticks.size() == 4 && editors.size() == 4, "setup: System, Game, Music and Chat are listed and ticked");

        if (ticks.size() == 4 && editors.size() == 4)
        {
            check (std::all_of (ticks.begin(), ticks.end(), [] (TickBox* t) { return t->getToggleState(); }), "setup: all four start ticked");
            ticks[1]->setToggleState (false, juce::dontSendNotification);
            click (*ticks[1]);
            check (! sheet->getChoices().channels[1].keep, "setup: unticking Game is remembered");

            editors[2]->setText ("Tunes", false);          // as if typed: JUCE posts the change message, so call it by hand
            if (editors[2]->onTextChange)
                editors[2]->onTextChange();

            check (sheet->getChoices().channels[2].name == "Tunes", "setup: renaming Music is remembered");
        }

        std::vector<juce::TextButton*> buttons;
        collect<juce::TextButton> (*sheet->getPageComponent(), buttons);
        auto add = std::find_if (buttons.begin(), buttons.end(), [] (juce::TextButton* b) { return b->getButtonText() == "Add a channel"; });
        check (add != buttons.end(), "setup: there is an Add a channel button");

        if (add != buttons.end())
        {
            click (**add);
            editors.clear();
            ticks.clear();
            collect<juce::TextEditor> (*sheet->getPageComponent(), editors);
            collect<TickBox> (*sheet->getPageComponent(), ticks);
            check (editors.size() == 5 && sheet->getChoices().channels.back().isNew, "setup: a fifth row for the new channel");

            if (editors.size() == 5)
            {
                editors[4]->setText ("Stream", false);

                if (editors[4]->onTextChange)
                    editors[4]->onTextChange();
            }

            // Nothing ticked: no way on.
            for (auto* t : ticks)
            {
                t->setToggleState (false, juce::dontSendNotification);
                click (*t);
            }

            check (! sheet->getNextButton().isEnabled(), "setup: with no channel ticked Next is off");

            for (size_t i = 0; i < ticks.size(); ++i)
                if (i != 1)
                {
                    ticks[i]->setToggleState (true, juce::dontSendNotification);
                    click (*ticks[i]);
                }

            check (sheet->getNextButton().isEnabled(), "setup: ticking channels again turns Next on");
        }
    }

    click (sheet->getNextButton());
    check (sheet->getPage() == SetupSheet::Page::voice, "setup: voice page");

    // Voice: a preset and a noise removal mode.
    {
        std::vector<ChoiceRow*> rows;
        std::vector<SegmentedControl*> segments;
        collect<ChoiceRow> (*sheet->getPageComponent(), rows);
        collect<SegmentedControl> (*sheet->getPageComponent(), segments);
        check (rows.size() == 4 && segments.size() == 1, "setup: four presets and the noise removal mode");

        if (rows.size() == 4 && segments.size() == 1)
        {
            check (rows[0]->getToggleState() && sheet->getChoices().preset == "clean", "setup: Clean voice is the starting preset");
            check (segments[0]->getSelected() == 0 && sheet->getChoices().noiseMode == 1, "setup: Best is the starting noise removal");

            click (*rows[2]);
            check (sheet->getChoices().preset == "noisy" && rows[2]->getToggleState() && ! rows[0]->getToggleState(), "setup: Noisy room is picked");

            if (segments[0]->onChange)
                segments[0]->onChange (1);

            check (sheet->getChoices().noiseMode == 0, "setup: Fast is picked (the effect's mode 0)");

            click (*rows[3]);
            check (! segments[0]->isEnabled(), "setup: with Off there is nothing to remove noise from");
            click (*rows[2]);
            check (segments[0]->isEnabled(), "setup: and it is back with a preset");
        }
    }

    click (sheet->getNextButton());
    check (sheet->getPage() == SetupSheet::Page::system, "setup: system page");

    // System: the three switches.
    {
        std::vector<SwitchToggle*> toggles;
        collect<SwitchToggle> (*sheet->getPageComponent(), toggles);
        check (toggles.size() == 3, "setup: start at login, use for all apps, keep as the default");

        if (toggles.size() == 3)
        {
            check (toggles[0]->getToggleState() && toggles[1]->getToggleState() && toggles[2]->getToggleState(), "setup: all three start on");

            toggles[0]->setToggleState (false, juce::dontSendNotification);
            click (*toggles[0]);
            check (! sheet->getChoices().startAtLogin, "setup: start at login off");

            toggles[1]->setToggleState (false, juce::dontSendNotification);
            click (*toggles[1]);
            sheet->getPageComponent()->repaint();
            check (! sheet->getChoices().makeDefault, "setup: use for all apps off");
            check (! toggles[2]->isEnabled(), "setup: keep as the default is greyed out meanwhile");

            toggles[1]->setToggleState (true, juce::dontSendNotification);
            click (*toggles[1]);
            toggles[2]->setToggleState (false, juce::dontSendNotification);
            click (*toggles[2]);
            check (sheet->getChoices().makeDefault && ! sheet->getChoices().keepDefault, "setup: keep as the default off, the main switch on");
        }
    }

    click (sheet->getNextButton());
    check (sheet->getPage() == SetupSheet::Page::hardware, "setup: the Wave:3 step comes last");

    // Wave:3 controls: ask for the permission (the demo grants it), and the two switches are there to use.
    {
        std::vector<juce::TextButton*> buttons;
        collect<juce::TextButton> (*sheet->getPageComponent(), buttons);
        auto allow = std::find_if (buttons.begin(), buttons.end(), [] (juce::TextButton* b) { return b->getButtonText().startsWith ("Allow hardware controls"); });
        check (allow != buttons.end(), "setup: there is an Allow hardware controls button");

        if (allow != buttons.end())
            click (**allow);

        ui->flushUpdates();
        check (c.getHardwareControls().usbPermission, "setup: the permission went through the existing flow");
    }

    check (sheet->getNextButton().getButtonText() == "Set up Headroom", "setup: the last step says what Next does");
    click (sheet->getNextButton());
    check (sheet->getPage() == SetupSheet::Page::installing, "setup: Set up Headroom starts the install");
}

void ScreenshotRunner::checkSetupWizardFinish()
{
    auto& c = *controller;
    auto& state = c.getState();
    auto* sheet = ui->getSetupSheet();

    check (sheet != nullptr && sheet->getPage() == SetupSheet::Page::done, "setup: the closing page appears when the install is done");

    if (sheet == nullptr)
        return;

    check (sheet->installedNow(), "setup: it installed");

    const auto locations = installManager->getInstaller().getLocations();
    check (locations.programCopy().existsAsFile() && locations.unitFile().existsAsFile() && locations.desktopFile().existsAsFile()
               && locations.iconFile().existsAsFile() && locations.wireplumberDropIn().existsAsFile(),
           "setup: the program, unit, menu entry, icon and drop-in are in the home folder");
    check (locations.unitFile().loadFileAsString().contains (" --background") && locations.unitFile().loadFileAsString().contains ("Headroom.AppImage"),
           "setup: the unit starts the installed AppImage with --background");
    check (installRunner->count ("disable headroom.service") >= 1, "setup: start at login was switched off, so the unit is not enabled");
    check (installRunner->count ("restart headroom.service") == 0, "setup: the wizard does not start the service itself (it is the running Headroom)");

    // Everything chosen is in the model.
    check (! state.findChannel ("game").isValid(), "setup: Game was removed");
    check (AppState::describe (state.findChannel ("music")).name == "Tunes", "setup: Music was renamed to Tunes");

    bool hasStream = false;

    for (const auto& channel : state.getChannelInfos())
        hasStream = hasStream || (channel.name == "Stream" && ! channel.isMic());

    check (hasStream, "setup: the new channel Stream exists");
    check (state.getSetting (ids::micPreset).toString() == "noisy", "setup: the Noisy room preset is applied");
    check (SetupChoices::noiseModeOf (c, -1) == 0, "setup: noise removal is on Fast");
    check ((int) state.getSetting (ids::startAtLogin) == 0 && state.getMakeDefault() && ! state.getKeepDefault(), "setup: the system choices are in the settings");
    check ((int) state.getSetting (ids::firstRunDone) == 1, "setup: the closing page replaces the You're set up sheet");
    check (state.getSetting (ids::clipGuard).isVoid() ? false : (int) state.getSetting (ids::clipGuard) == 1, "setup: ClipGuard was switched on through the controller");

    // Open Headroom hands over to the installed program (Main.cpp quits and starts it).
    check (sheet->getNextButton().getButtonText() == "Open Headroom" && ! sheet->getBackButton().isVisible(), "setup: the closing page has one button, Open Headroom");
    click (sheet->getNextButton());
    check (openedProgram == locations.programCopy(), "setup: Open Headroom asks to start the installed copy");
    ui->hideSheet();

    // Run again from Settings: it starts from what is set now.
    ui->showSetupWizard (SetupSheet::Mode::rerun);
    ui->flushUpdates();
    sheet = ui->getSetupSheet();
    check (sheet != nullptr, "setup: it can be run again");

    if (sheet != nullptr)
    {
        check (sheet->getChoices().preset == "noisy" && sheet->getChoices().noiseMode == 0 && ! sheet->getChoices().startAtLogin,
              "setup: run again starts from the current settings");

        bool listed = false;

        for (const auto& channel : sheet->getChoices().channels)
            listed = listed || channel.name == "Tunes";

        check (listed && sheet->getChoices().channels.size() == 4, "setup: run again lists the current channels");

        click (sheet->getBackButton());                  // Not now
        check (! ui->isSheetOpen(), "setup: Not now closes the wizard");
    }
}

void ScreenshotRunner::checkUpdateAndUninstall()
{
    ui->showTab (Tab::settings);
    ui->flushUpdates();

    // Settings: Run setup again and Uninstall are there for an installed Headroom.
    auto* runSetup = dynamic_cast<juce::Button*> (ui->getSettings().findButton ("Run setup again"));
    auto* uninstall = dynamic_cast<juce::Button*> (ui->getSettings().findButton ("Uninstall Headroom"));
    check (runSetup != nullptr && uninstall != nullptr && runSetup->isVisible() && uninstall->isVisible(), "settings: Run setup again and Uninstall are shown");

    if (runSetup != nullptr)
    {
        click (*runSetup);
        check (ui->getSetupSheet() != nullptr, "settings: Run setup again opens the wizard");

        if (auto* sheet = ui->getSetupSheet())
            click (sheet->getBackButton());

        check (! ui->isSheetOpen(), "settings: and Not now closes it");
    }

    if (uninstall != nullptr)
    {
        click (*uninstall);
        auto* sheet = dynamic_cast<UninstallSheet*> (ui->getSheetContent());
        check (sheet != nullptr, "uninstall: the confirmation sheet opens");

        if (sheet != nullptr)
        {
            check (! sheet->getDeleteSettings(), "uninstall: deleting the settings is off by default");
            click (sheet->getCancelButton());
            check (! ui->isSheetOpen() && ! uninstallAsked, "uninstall: Cancel changes nothing");
        }

        click (*uninstall);
        sheet = dynamic_cast<UninstallSheet*> (ui->getSheetContent());

        if (sheet != nullptr)
        {
            sheet->getDeleteSettingsBox().setToggleState (true, juce::dontSendNotification);
            click (sheet->getConfirmButton());
            check (uninstallAsked && uninstallDeletes && ! ui->isSheetOpen(), "uninstall: Uninstall with the box ticked asks to delete the settings too");
        }
    }

    // The update sheet.
    {
        bool updated = false, declined = false;
        UpdateSheet update ("0.3.0", "0.2.4", [&] { updated = true; }, [&] { declined = true; });
        update.setSize (UpdateSheet::kWidth, UpdateSheet::kHeight);
        click (update.getNotNowButton());
        click (update.getUpdateButton());
        check (declined && updated, "update: both buttons do their part");
        update.setBusy();
        check (update.isBusy() && ! update.getUpdateButton().isEnabled() && ! update.getNotNowButton().isEnabled(), "update: nothing can be pressed while it updates");
        update.showError ("The update did not work.");
        check (! update.isBusy() && update.getNotNowButton().getButtonText() == "Close" && ! update.getUpdateButton().isVisible(), "update: a failure leaves a Close button");
    }

    ui->showTab (Tab::mixer);       // the next checks look at the mixer's strips
    ui->flushUpdates();
}

void ScreenshotRunner::checkAfterRemoval()
{
    auto& c = *controller;
    check (! c.getState().findChannel ("chat").isValid(), "remove: Chat is gone after confirming");
    check (AppState::describe (c.getState().findChannel ("system")).name == "Browser", "rename: the new name is in the model");
    check (ui->getMixer().findStrip ("chat") == nullptr, "remove: its strip is gone");

    if (ui->isSheetOpen())
    {
        std::vector<juce::TextButton*> buttons;
        collect<juce::TextButton> (*ui, buttons);

        for (auto* button : buttons)
            if (button->getButtonText() == "Done")
                click (*button);

        ui->flushUpdates();
        check ((int) c.getState().getSetting (ids::firstRunDone) == 1 && ! ui->isSheetOpen(),
               "first run: Done closes the sheet and remembers it");
    }
    else
    {
        check (false, "first run: the sheet should have reappeared");
    }

    // Tabs: clicking selects exactly one (a JUCE radio group here once fired every tab's handler).
    std::vector<TabButton*> tabs;
    collect<TabButton> (*ui, tabs);
    check (tabs.size() == 3, "top bar: three tabs");

    for (int wanted = 0; wanted < 3 && tabs.size() == 3; ++wanted)
    {
        click (*tabs[(size_t) (wanted + 1) % 3]);
        click (*tabs[(size_t) wanted]);
        int on = 0;

        for (auto* t : tabs)
            on += t->getToggleState() ? 1 : 0;

        check (on == 1 && tabs[(size_t) wanted]->getToggleState() && ui->getTab() == (Tab) wanted,
               "top bar: clicking tab " + juce::String (wanted) + " selects only that one");
    }

    click (*tabs[0]);

    // Tab order: top bar, then the mixer header, then the strips left to right, master last.
    ui->flushUpdates();
    juce::KeyboardFocusTraverser traverser;
    juce::String order;
    int stops = 0;

    for (auto* stop : traverser.getAllComponents (ui.get()))
    {
        if (dynamic_cast<TabButton*> (stop))                    order << "Tab ";
        else if (dynamic_cast<StatusPill*> (stop))              order << "Pill ";
        else if (dynamic_cast<SegmentedControl*> (stop))        order << "Mix ";
        else if (dynamic_cast<Dropdown*> (stop))                order << "Output ";
        else if (dynamic_cast<FxButton*> (stop))                order << "Fx ";
        else if (dynamic_cast<LevelFader*> (stop))              order << "Fader ";
        else if (dynamic_cast<MuteButton*> (stop))              order << "Mute ";
        else if (dynamic_cast<LinkToggle*> (stop))              order << "Link ";
        else if (dynamic_cast<AddChannelStrip*> (stop))         order << "Add ";
        else if (dynamic_cast<LedToggle*> (stop))               order << "Protect ";
        else                                                    continue;

        ++stops;
    }

    Log::info ("  focus order: " + order.trim());
    check (order.startsWith ("Tab Tab Tab Pill Mix Output Fx Fader Mute Link Fx Fader")
               && order.trim().endsWith ("Add Protect Fader Mute"),
           "keyboard: Tab visits top bar, header, each strip in turn, then master (" + juce::String (stops) + " stops)");

    ui->showTab (Tab::apps);
    ui->showTab (Tab::settings);
    ui->showTab (Tab::mixer);
    check (true, "tabs: switching around keeps everything alive");

    if (failedChecks > 0)
    {
        Log::error (juce::String (failedChecks) + " UI check(s) failed");
        allOk = false;
    }
}

// ---------------------------------------------------------------------------------------------------
// Pictures
// ---------------------------------------------------------------------------------------------------

bool ScreenshotRunner::capture (const juce::String& name, juce::Point<int> size)
{
    ui->setSize (size.x, size.y);
    ui->flushUpdates();

    auto snapshot = [&] (float scale)
    {
        if (name == "toast")
        {
            ToastView toast ("Headroom keeps running in the background. Open it again from your app menu.");
            toast.setSize (toast.getIdealWidth(), ToastView::kHeight);
            return toast.createComponentSnapshot (toast.getLocalBounds(), true, scale);
        }

        // The "Update Headroom" sheet has a window of its own (Main.cpp): the same panel on the same backdrop.
        if (name == "update")
        {
            SheetHost host;
            host.setSize (560, 300);
            host.show (std::make_unique<UpdateSheet> ("0.3.0", "0.2.4", [] {}, [] {}), UpdateSheet::kWidth, UpdateSheet::kHeight);
            juce::Image image (juce::Image::ARGB, juce::roundToInt (560 * scale), juce::roundToInt (300 * scale), true);
            juce::Graphics g (image);
            g.addTransform (juce::AffineTransform::scale (scale));
            g.fillAll (theme::bg0);
            host.paintEntireComponent (g, false);
            return image;
        }

        return ui->createComponentSnapshot (ui->getLocalBounds(), true, scale);
    };

    bool ok = true;

    for (const float scale : { 1.0f, 2.0f })
    {
        const auto file = directory.getChildFile (name + (scale > 1.0f ? "@2x.png" : ".png"));
        const auto image = snapshot (scale);

        juce::PNGImageFormat png;
        file.deleteFile();

        bool wrote = false;

        if (image.isValid())
            if (juce::FileOutputStream stream (file); stream.openedOk())
                wrote = png.writeImageToStream (image, stream);

        Log::info ((wrote ? "Wrote " : "FAILED to write ") + file.getFullPathName());
        ok = ok && wrote;
    }

    ui->setSize (theme::kDefaultWidth, theme::kDefaultHeight);
    return ok;
}

void ScreenshotRunner::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto dt = juce::jlimit (0.0, 0.25, (now - lastMs) / 1000.0);
    lastMs = now;

    if (ui == nullptr)
        return;

    ui->tickMeters (dt);

    if (warmUpMs > 0)                       // let the demo signals reach every meter before the first shot
    {
        warmUpMs -= juce::roundToInt (dt * 1000.0);
        return;
    }

    if (stepIndex >= plan.size())
    {
        if (! mutedRun)
        {
            // Second run: the same demo with the mic muted on the device, for the banner.
            endRun();
            beginRun (true);
            plan.clear();
            plan.push_back ({ "banner-muted", 0, {} });
            stepIndex = 0;
            warmUpMs = 1200;
            return;
        }

        stopTimer();
        finish();
        return;
    }

    auto& step = plan[stepIndex];

    if (! stepStarted)
    {
        stepStarted = true;
        stepElapsedMs = 0.0;

        if (step.setUp)
            step.setUp();

        ui->flushUpdates();
    }

    stepElapsedMs += dt * 1000.0;

    if (stepElapsedMs < (double) step.settleMs)
        return;

    if (step.readyWhen && ! step.readyWhen() && stepElapsedMs < (double) step.settleMs + 6000.0)
        return;

    if (step.file.isNotEmpty())
        allOk = capture (step.file, step.size) && allOk;

    ++stepIndex;
    stepStarted = false;
}

void ScreenshotRunner::finish()
{
    endRun();

    if (finished)
        finished (allOk);
}
}
