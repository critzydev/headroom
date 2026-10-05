// MainComponent: shell layout, model / backend change dispatch, status banners, drawer animation, sheets.
#include "ui/MainComponent.h"

#include "ui/Focus.h"
#include "ui/MicStatus.h"
#include "ui/UiScale.h"
#include "ui/Theme.h"

namespace hr
{
namespace
{
    constexpr int kMaxBanners = 3;
    constexpr double kDrawerMs = 150.0;
    constexpr double kDisconnectGraceMs = 2000.0;

    float easeOut (float t)     { const float u = 1.0f - t; return 1.0f - u * u * u; }
}

MainComponent::MainComponent (AppController& c, InstallManager* installer)
    : controller (c), installManager (installer), mixer (c, meterClock), apps (c), settings (c, meterClock, installer), drawer (c, meterClock)
{
    setWantsKeyboardFocus (true);
    meterClock.setWatched (this);

    topBar.onTabChanged = [this] (Tab tab) { showTab (tab); };
    topBar.onPillClicked = [this]
    {
        showTab (Tab::settings);
        settings.scrollToMicrophone();
    };
    addAndMakeVisible (topBar);

    mixer.onToggleFx = [this] (const juce::String& id) { toggleFx (id); };
    mixer.onRemoveChannel = [this] (const juce::String& id) { showRemoveChannelSheet (id); };
    addAndMakeVisible (mixer);
    addChildComponent (apps);
    addChildComponent (settings);

    settings.onRunSetup = [this] { showSetupWizard (SetupSheet::Mode::rerun); };
    settings.onUninstall = [this] { showUninstallSheet(); };

    drawer.onClose = [this] { closeFxDrawer(); };
    addChildComponent (drawer);
    addChildComponent (sheets);

    controller.onBackendChanged = [this] { triggerAsyncUpdate(); };
    controller.onNoticesChanged = [this] { triggerAsyncUpdate(); };
    controller.getPlugins().onPluginListChanged = [this]
    {
        drawer.pluginListChanged();
        triggerAsyncUpdate();
    };
    controller.getState().addListener (this);

    refreshAll();
    setSize (theme::kDefaultWidth, theme::kDefaultHeight);
    meterClock.start();
}

MainComponent::~MainComponent()
{
    stopTimer();
    meterClock.stop();
    cancelPendingUpdate();
    controller.onBackendChanged = nullptr;
    controller.onNoticesChanged = nullptr;
    controller.getPlugins().onPluginListChanged = nullptr;
    controller.getState().removeListener (this);
}

// ---------------------------------------------------------------------------------------------------
// Navigation
// ---------------------------------------------------------------------------------------------------

void MainComponent::showTab (Tab tab)
{
    if (tab != Tab::mixer)
        closeFxDrawer (false);

    topBar.setTab (tab);
    layoutContent();
    refreshAll();
}

void MainComponent::toggleFx (const juce::String& channelId)
{
    if (drawerOpen && drawer.getChannelId() == channelId)
        closeFxDrawer();
    else
        openFxDrawer (channelId);
}

void MainComponent::openFxDrawer (const juce::String& channelId, bool animate)
{
    if (topBar.getTab() != Tab::mixer)
        showTab (Tab::mixer);

    drawer.open (channelId);
    drawerOpen = true;
    mixer.setDrawerChannel (channelId);
    drawer.setVisible (true);
    drawer.toFront (false);

    if (animate)
        animateDrawerTo (1.0f);
    else
        setDrawerProgress (1.0f);

    focusIfShowing (drawer);
}

void MainComponent::closeFxDrawer (bool animate)
{
    if (! drawerOpen)
        return;

    drawerOpen = false;
    drawer.showAddPanel (false);
    mixer.setDrawerChannel ({});

    if (animate)
        animateDrawerTo (0.0f);
    else
        setDrawerProgress (0.0f);

    focusIfShowing (*this);
}

void MainComponent::animateDrawerTo (float target)
{
    animFrom = drawerProgress;
    animTo = target;
    animStartMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
}

void MainComponent::timerCallback()
{
    const float t = juce::jlimit (0.0f, 1.0f, (float) ((juce::Time::getMillisecondCounterHiRes() - animStartMs) / kDrawerMs));
    setDrawerProgress (animFrom + (animTo - animFrom) * easeOut (t));

    if (t >= 1.0f)
        stopTimer();
}

void MainComponent::setDrawerProgress (float progress)
{
    drawerProgress = progress;
    layoutDrawer();
    drawer.setVisible (drawerProgress > 0.001f);

    if (! drawerOpen && drawerProgress <= 0.001f)
        drawer.clear();         // nothing may keep pointing into effects the user can no longer see
}

void MainComponent::layoutDrawer()
{
    const int shown = juce::roundToInt (theme::kDrawerWidth * drawerProgress);
    drawer.setBounds (contentArea.getRight() - shown, contentArea.getY(), theme::kDrawerWidth, contentArea.getHeight());
}

void MainComponent::flushUpdates()
{
    handleUpdateNowIfNeeded();
}

void MainComponent::visibilityChanged()
{
    if (isVisible())
        focusIfShowing (*this);
}

// ---------------------------------------------------------------------------------------------------
// Model changes
// ---------------------------------------------------------------------------------------------------

void MainComponent::valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& child, int)
{
    // An effect is going away: its inline panel must not outlive the processor it points into.
    if (child.hasType (ids::SLOT))
        drawer.dropPanelFor (child[ids::uid].toString());

    triggerAsyncUpdate();
}

void MainComponent::handleAsyncUpdate()
{
    refreshAll();
}

void MainComponent::refreshAll()
{
    applyUiScale();
    updateMicPill();
    rebuildBanners();

    switch (topBar.getTab())
    {
        case Tab::mixer:    mixer.syncFromModel(); break;
        case Tab::apps:     apps.syncFromModel(); break;
        case Tab::settings: settings.syncFromModel(); break;
    }

    if (drawerOpen)
        drawer.syncFromModel();

    maybeShowFirstRun();
}

void MainComponent::applyUiScale()
{
    // 0 = automatic: what the desktop is set to (Xft.dpi, kwinrc, GDK_SCALE), which JUCE does not see on Xwayland.
    const double scale = UiScale::effectiveScale (controller.getState().getSetting (ids::uiScale));

    if (scale != appliedScale)
    {
        appliedScale = scale;
        juce::Desktop::getInstance().setGlobalScaleFactor ((float) scale);
    }
}

void MainComponent::updateMicPill()
{
    const auto status = controller.getBackendStatus();
    const auto view = describeMic (status);
    topBar.setMicStatus (view.dot, view.pill, view.sentence + " Click to open microphone settings.");
}

void MainComponent::rebuildBanners()
{
    std::vector<BannerSpec> specs;
    const auto status = controller.getBackendStatus();
    const auto now = juce::Time::getMillisecondCounterHiRes();

    // Not connected yet is normal for the first moments of a launch: only complain if it lasts.
    if (! status.connected)
    {
        if (disconnectedSinceMs == 0.0)
        {
            disconnectedSinceMs = now;
            juce::Component::SafePointer<MainComponent> safe (this);
            juce::Timer::callAfterDelay ((int) kDisconnectGraceMs + 100, [safe] { if (safe != nullptr) safe->triggerAsyncUpdate(); });
        }

        if (now - disconnectedSinceMs >= kDisconnectGraceMs)
        {
            BannerSpec spec;
            spec.id = "backend";
            spec.severity = BannerSpec::Severity::error;
            spec.text = "Headroom can" + theme::glyph::quoteClose + "t reach the audio system. It will keep trying.";
            specs.push_back (std::move (spec));
        }
    }
    else
    {
        disconnectedSinceMs = 0.0;

        BannerSpec spec;
        spec.id = "mic";

        switch (status.mic)
        {
            case MicState::NotFound:
                spec.severity = BannerSpec::Severity::warning;
                spec.text = noMicSentence();
                break;

            case MicState::Recovering:
                spec.severity = BannerSpec::Severity::warning;
                spec.text = "Getting your mic to start. This takes a few seconds.";
                break;

            case MicState::SilentFromStart:
                spec.severity = BannerSpec::Severity::error;
                spec.text = status.message.isNotEmpty() ? status.message : silentMicSentence (status);
                spec.actionLabel = "Try again";
                spec.action = [this] { controller.fixMic(); };
                break;

            case MicState::MutedOnDevice:
                spec.severity = BannerSpec::Severity::error;
                spec.text = status.message.isNotEmpty() ? status.message : mutedMicSentence (status);
                break;

            case MicState::Starting:
            case MicState::Ok:
                // A working mic comes without a message of its own; one that is there is something else the backend
                // wants said (for example that another program keeps changing the default device).
                if (status.message.isNotEmpty())
                {
                    spec.id = "backend-message";
                    spec.severity = BannerSpec::Severity::warning;
                    spec.text = status.message;
                    spec.actionLabel = "Open settings";
                    spec.action = [this] { showTab (Tab::settings); };
                }
                else
                {
                    spec.id = {};
                }

                break;
        }

        if (spec.id.isNotEmpty())
            specs.push_back (std::move (spec));

        if (! status.outputOk)
        {
            BannerSpec out;
            out.id = "output";
            out.severity = BannerSpec::Severity::warning;
            out.text = "No headphones found. Plug them in, or pick another output.";
            out.actionLabel = "Open settings";
            out.action = [this] { showTab (Tab::settings); };
            specs.push_back (std::move (out));
        }
    }

    for (const auto& notice : controller.getNotices())
    {
        BannerSpec spec;
        spec.id = "notice:" + notice.id;
        spec.severity = notice.severity == Notice::Severity::error ? BannerSpec::Severity::error
                      : notice.severity == Notice::Severity::warning ? BannerSpec::Severity::warning : BannerSpec::Severity::info;
        spec.text = notice.text;
        spec.actionLabel = notice.actionLabel;

        if (notice.actionLabel.isNotEmpty())
            spec.action = [this, id = notice.id] { controller.runNoticeAction (id); };

        spec.onDismiss = [this, id = notice.id] { controller.dismissNotice (id); };
        specs.push_back (std::move (spec));
    }

    if (specs.size() > (size_t) kMaxBanners)
        specs.resize ((size_t) kMaxBanners);

    // Keep rows whose content didn't change, so buttons don't flicker under the mouse.
    bool same = specs.size() == banners.size();

    for (size_t i = 0; same && i < specs.size(); ++i)
        same = banners[i]->getSpec().sameContentAs (specs[i]);

    if (same)
        return;

    banners.clear();

    for (auto& spec : specs)
    {
        auto row = std::make_unique<BannerRow> (std::move (spec));
        addAndMakeVisible (*row);
        banners.push_back (std::move (row));
    }

    sheets.toFront (false);
    layoutContent();
}

// ---------------------------------------------------------------------------------------------------
// Sheets
// ---------------------------------------------------------------------------------------------------

void MainComponent::maybeShowFirstRun()
{
    const bool done = (int) controller.getState().getSetting (ids::firstRunDone) != 0;

    if (done)
    {
        if (sheetKind == SheetKind::firstRun)
            hideSheet();

        return;
    }

    if (sheetKind != SheetKind::none)
        return;

    const auto status = controller.getBackendStatus();

    if (status.connected && status.mic != MicState::NotFound && status.mic != MicState::Starting)
        showFirstRunSheet();
}

void MainComponent::showFirstRunSheet()
{
    const auto chat = controller.getState().findChannel ("chat");
    const auto chatName = chat.isValid() ? chat[ids::name].toString() : juce::String ("Chat");

    auto sheet = std::make_unique<FirstRunSheet> (controller.getBackendStatus(), chatName, [this]
    {
        controller.getState().setSetting (ids::firstRunDone, 1);
        hideSheet();
    });

    sheetKind = SheetKind::firstRun;
    sheets.onEscape = sheets.onEnter = [this]
    {
        controller.getState().setSetting (ids::firstRunDone, 1);
        hideSheet();
    };
    sheets.show (std::move (sheet), FirstRunSheet::kWidth, FirstRunSheet::kHeight);
    sheets.setBounds (getLocalBounds());
}

void MainComponent::showRemoveChannelSheet (const juce::String& channelId)
{
    const auto info = AppState::describe (controller.getState().findChannel (channelId));

    if (info.id.isEmpty() || info.isMic())
        return;

    const auto fallback = AppState::describe (controller.getState().findChannel (controller.getState().getFallbackChannel()));
    const auto goesTo = fallback.id.isNotEmpty() && fallback.id != info.id ? fallback.name : juce::String ("the default channel");

    const auto consequence = info.isInput() ? juce::String ("Its effects and levels are lost. The device itself is not touched.")
                                            : "Apps playing into it move to " + goesTo + ". Its effects and levels are lost.";

    auto sheet = std::make_unique<ConfirmSheet> ("Remove " + info.name + "?",
                                                 consequence,
                                                 "Remove",
                                                 [this, channelId]
                                                 {
                                                     if (drawerOpen && drawer.getChannelId() == channelId)
                                                         closeFxDrawer (false);

                                                     hideSheet();
                                                     controller.removeChannel (channelId);
                                                 },
                                                 [this] { hideSheet(); });

    sheetKind = SheetKind::confirm;
    sheets.onEnter = nullptr;
    sheets.onEscape = [this] { hideSheet(); };
    sheets.show (std::move (sheet), ConfirmSheet::kWidth, ConfirmSheet::kHeight);
    sheets.setBounds (getLocalBounds());
}

void MainComponent::showSetupWizard (SetupSheet::Mode mode)
{
    if (installManager == nullptr)
        return;

    auto sheet = std::make_unique<SetupSheet> (controller, *installManager, mode, [this] (SetupSheet::Outcome)
    {
        hideSheet();
        controller.setHoldDefaults (false);     // the wizard is over: what was chosen is in the model, or it was skipped
    });

    setupSheet = sheet.get();
    sheetKind = SheetKind::setup;
    sheets.onEscape = [this] { if (setupSheet != nullptr) setupSheet->back(); };
    sheets.onEnter = [this] { if (setupSheet != nullptr) setupSheet->next(); };
    sheets.show (std::move (sheet), SetupSheet::kWidth, SetupSheet::kHeight);
    sheets.setBounds (getLocalBounds());
}

void MainComponent::showUninstallSheet()
{
    if (installManager == nullptr)
        return;

    auto sheet = std::make_unique<UninstallSheet> ([this] (bool deleteSettings)
    {
        hideSheet();

        if (installManager != nullptr)
            installManager->requestUninstall (deleteSettings);
    }, [this] { hideSheet(); });

    sheetKind = SheetKind::uninstall;
    sheets.onEnter = nullptr;
    sheets.onEscape = [this] { hideSheet(); };
    sheets.show (std::move (sheet), UninstallSheet::kWidth, UninstallSheet::kHeight);
    sheets.setBounds (getLocalBounds());
}

void MainComponent::hideSheet()
{
    sheets.hide();
    sheetKind = SheetKind::none;
    focusIfShowing (*this);
}

// ---------------------------------------------------------------------------------------------------
// Layout and keys
// ---------------------------------------------------------------------------------------------------

void MainComponent::layoutContent()
{
    auto area = getLocalBounds();
    topBar.setBounds (area.removeFromTop (theme::kTopBarHeight));

    for (auto& banner : banners)
        banner->setBounds (area.removeFromTop (theme::kBannerHeight));

    contentArea = area;
    mixer.setBounds (area);
    apps.setBounds (area);
    settings.setBounds (area);

    mixer.setVisible (topBar.getTab() == Tab::mixer);
    apps.setVisible (topBar.getTab() == Tab::apps);
    settings.setVisible (topBar.getTab() == Tab::settings);

    layoutDrawer();
    sheets.setBounds (getLocalBounds());
}

void MainComponent::resized()
{
    layoutContent();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg0);
}

bool MainComponent::keyPressed (const juce::KeyPress& key)
{
    if (key.getKeyCode() == juce::KeyPress::escapeKey && drawerOpen)
    {
        closeFxDrawer();
        return true;
    }

    if (key.getModifiers().isCommandDown() || key.getModifiers().isCtrlDown())
    {
        if (key.getKeyCode() == '1')    { showTab (Tab::mixer);    return true; }
        if (key.getKeyCode() == '2')    { showTab (Tab::apps);     return true; }
        if (key.getKeyCode() == '3')    { showTab (Tab::settings); return true; }
    }

    return false;
}
}
