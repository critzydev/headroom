// MainComponent: the app shell. Top bar, status banners, the three views, the FX drawer and the sheets.
#pragma once

#include "Juce.h"
#include "app/AppController.h"
#include "app/InstallManager.h"
#include "ui/fx/FxDrawer.h"
#include "ui/setup/SetupSheet.h"
#include "ui/setup/SetupSheets.h"
#include "ui/views/AppsView.h"
#include "ui/views/MixerView.h"
#include "ui/views/SettingsView.h"
#include "ui/views/Sheets.h"
#include "ui/views/TopBar.h"
#include "ui/widgets/Meter.h"
#include "ui/widgets/StatusPill.h"

#include <memory>
#include <vector>

namespace hr
{
class MainComponent final : public juce::Component,
                            private juce::AsyncUpdater,
                            private juce::ValueTree::Listener,
                            private juce::Timer
{
public:
    // `installManager` may be null: then there is no setup wizard and no uninstall (a dev run, the tests).
    explicit MainComponent (AppController& controller, InstallManager* installManager = nullptr);
    ~MainComponent() override;

    // ---- Navigation (also what screenshot mode drives) ---------------------------------------------
    void showTab (Tab tab);
    Tab getTab() const noexcept                         { return topBar.getTab(); }
    MixerView& getMixer() noexcept                      { return mixer; }
    FxDrawer& getDrawer() noexcept                      { return drawer; }
    SettingsView& getSettings() noexcept                { return settings; }

    void openFxDrawer (const juce::String& channelId, bool animate = true);
    void closeFxDrawer (bool animate = true);
    bool isFxDrawerOpen() const noexcept                { return drawerOpen; }

    void showFirstRunSheet();
    void showRemoveChannelSheet (const juce::String& channelId);
    void showSetupWizard (SetupSheet::Mode mode);       // the first-run setup, or "Run setup again"
    void showUninstallSheet();                          // Settings: Uninstall Headroom...
    SetupSheet* getSetupSheet() noexcept                { return setupSheet.getComponent(); }
    juce::Component* getSheetContent() noexcept         { return sheets.getContent(); }
    void hideSheet();
    bool isSheetOpen() const noexcept                   { return sheets.isOpen(); }

    // Applies any pending model / backend changes right now instead of on the next message-loop turn.
    void flushUpdates();

    // Runs one meter step by hand (screenshot mode has no live window to drive the shared clock).
    void tickMeters (double deltaSeconds)               { meterClock.tick (deltaSeconds); }

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override   { juce::Component::grabKeyboardFocus(); }
    void visibilityChanged() override;

private:
    enum class SheetKind { none, firstRun, confirm, setup, uninstall };

    void handleAsyncUpdate() override;
    void timerCallback() override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override  { triggerAsyncUpdate(); }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override               { triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged (juce::ValueTree&, int, int) override                { triggerAsyncUpdate(); }

    void refreshAll();
    void updateMicPill();
    void rebuildBanners();
    void maybeShowFirstRun();
    void applyUiScale();
    void layoutContent();
    void layoutDrawer();
    void setDrawerProgress (float progress);
    void animateDrawerTo (float target);
    void toggleFx (const juce::String& channelId);

    AppController& controller;
    InstallManager* installManager = nullptr;
    MeterClock meterClock;

    TopBar topBar;
    std::vector<std::unique_ptr<BannerRow>> banners;
    MixerView mixer;
    AppsView apps;
    SettingsView settings;
    FxDrawer drawer;
    SheetHost sheets;
    SheetKind sheetKind = SheetKind::none;
    juce::Component::SafePointer<SetupSheet> setupSheet;

    juce::Rectangle<int> contentArea;
    bool drawerOpen = false;
    float drawerProgress = 0.0f, animFrom = 0.0f, animTo = 0.0f;
    double animStartMs = 0.0;
    double disconnectedSinceMs = 0.0;
    double appliedScale = 0.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
}
