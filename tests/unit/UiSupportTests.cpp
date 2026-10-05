// Unit tests for the UI's non-visual helpers: desktop scale detection, friendly routing-rule rows, the mic preset label.
#include "Juce.h"

#include "app/AppController.h"
#include "engine/builtin/Presets.h"
#include "ui/UiScale.h"
#include "ui/views/RuleGroups.h"

namespace hr
{
class UiSupportTests final : public juce::UnitTest
{
public:
    UiSupportTests() : juce::UnitTest ("UI support", "headroom") {}

    void runTest() override
    {
        testScaleParsing();
        testScaleChoice();
        testRuleGroups();
        testPresetLabel();
    }

private:
    static bool near (float a, float b)     { return std::abs (a - b) < 1.0e-6f; }

    void testScaleParsing()
    {
        beginTest ("Xft.dpi text becomes a scale in steps of 0.25");
        {
            expect (near (*UiScale::fromXftDpi ("96"), 1.0f));
            expect (near (*UiScale::fromXftDpi ("120"), 1.25f));       // KDE Wayland at 125 %
            expect (near (*UiScale::fromXftDpi (" 144\n"), 1.5f));
            expect (near (*UiScale::fromXftDpi ("192"), 2.0f));
            expect (near (*UiScale::fromXftDpi ("110"), 1.25f));       // snapped, not 1.146
            expect (near (*UiScale::fromXftDpi ("72"), 1.0f));         // never below 100 %
            expect (near (*UiScale::fromXftDpi ("900"), 3.0f));        // ... or above 300 %
            expect (! UiScale::fromXftDpi ("").has_value());
            expect (! UiScale::fromXftDpi ("abc").has_value());
            expect (! UiScale::fromXftDpi ("-96").has_value());
            expect (! UiScale::fromXftDpi ("0").has_value());
        }

        beginTest ("kwinrc [Xwayland] Scale");
        {
            const juce::String rc ("[Compositing]\nBackend=OpenGL\n\n[Xwayland]\nScale=1.25\n\n[Windows]\nScale=3\n");
            expect (near (*UiScale::fromKwinrc (rc), 1.25f));
            expect (near (*UiScale::fromKwinrc ("[Xwayland]\r\nScale = 1.5\r\n"), 1.5f));
            expect (near (*UiScale::fromKwinrc ("  [Xwayland]  \n  Scale=2"), 2.0f));
            expect (! UiScale::fromKwinrc ("[Windows]\nScale=1.25\n").has_value());        // only that section counts
            expect (! UiScale::fromKwinrc ("[Xwayland]\nOther=1\n").has_value());
            expect (! UiScale::fromKwinrc ("[Xwayland]\nScale=fast\n").has_value());
            expect (! UiScale::fromKwinrc ("").has_value());
        }

        beginTest ("GDK_SCALE");
        {
            expect (near (*UiScale::fromGdkScale ("2"), 2.0f));
            expect (near (*UiScale::fromGdkScale ("1"), 1.0f));
            expect (! UiScale::fromGdkScale ("").has_value());
            expect (! UiScale::fromGdkScale ("x").has_value());
        }

        beginTest ("snap");
        {
            expect (near (UiScale::snap (1.1), 1.0f));
            expect (near (UiScale::snap (1.13), 1.25f));
            expect (near (UiScale::snap (0.5), 1.0f));
            expect (near (UiScale::snap (7.0), 3.0f));
            expect (near (UiScale::snap (std::nan ("")), 1.0f));
        }
    }

    void testScaleChoice()
    {
        beginTest ("automatic uses the detected scale, a chosen size wins");
        {
            expect (UiScale::isAutomatic (juce::var()));
            expect (UiScale::isAutomatic (juce::var (0.0)));
            expect (! UiScale::isAutomatic (juce::var (1.0)));

            expect (near (UiScale::effectiveScale (juce::var (0.0), 1.25f, 1.0f), 1.25f));
            expect (near (UiScale::effectiveScale (juce::var(), 1.5f, 1.0f), 1.5f));
            expect (near (UiScale::effectiveScale (juce::var (1.5), 1.25f, 1.0f), 1.5f));
            expect (near (UiScale::effectiveScale (juce::var (2.0), 1.0f, 2.0f), 2.0f));
        }

        beginTest ("a desktop JUCE already scales is not scaled twice");
        {
            expect (near (UiScale::effectiveScale (juce::var (0.0), 2.0f, 2.0f), 1.0f));
            expect (near (UiScale::effectiveScale (juce::var (0.0), 1.0f, 1.25f), 1.0f));     // never shrinks below 100 %
            expect (near (UiScale::effectiveScale (juce::var (0.0), 1.5f, 1.0f), 1.5f));
        }
    }

    void testRuleGroups()
    {
        beginTest ("seeded rules become three friendly rows");
        {
            AppState state;
            const auto rules = state.getRoutingRules();
            const auto groups = groupRules (rules);

            expectEquals ((int) groups.size(), 3);

            if (groups.size() == 3)
            {
                expectEquals (groups[0].name, juce::String ("Discord"));
                expectEquals (groups[0].channelId, juce::String ("chat"));
                expectEquals (groups[0].patterns.size(), 4);
                expectEquals (groups[1].name, juce::String ("Spotify"));
                expectEquals (groups[1].channelId, juce::String ("music"));
                expectEquals (groups[2].name, juce::String ("Windows games (Steam / Proton)"));
                expectEquals (groups[2].channelId, juce::String ("game"));
                expectEquals (groups[2].patterns.size(), 4);
                expect (groups[0].isSeeded && groups[2].isSeeded);
            }

            int total = 0;

            for (auto& g : groups)
                total += g.patterns.size();

            expectEquals (total, (int) rules.size());          // every pattern is in exactly one row
        }

        beginTest ("user rules get a readable name, seeded ones stay grouped");
        {
            std::vector<RoutingRule> rules { { "firefox", "system" }, { "discord", "chat" }, { "cyberpunk2077.exe", "game" },
                                             { "vesktop", "chat" }, { "obs-studio", "music" }, { "*.exe", "game" } };
            const auto groups = groupRules (rules);

            expectEquals ((int) groups.size(), 5);          // firefox, Discord (2 rules), cyberpunk2077.exe is its own, obs, *.exe

            if (groups.size() == 5)
            {
                expectEquals (groups[0].name, juce::String ("Firefox"));
                expectEquals (groups[1].name, juce::String ("Discord"));
                expectEquals (groups[1].patterns.size(), 2);
                expect (groups[1].patterns.contains ("vesktop"));
                expectEquals (groups[2].name, juce::String ("Cyberpunk2077"));
                expect (! groups[2].isSeeded);
                expectEquals (groups[3].name, juce::String ("Obs Studio"));
            }
        }

        beginTest ("friendly names");
        {
            expectEquals (friendlyAppName ("spotify"), juce::String ("Spotify"));
            expectEquals (friendlyAppName ("steam_app_*"), juce::String ("Steam App"));
            expectEquals (friendlyAppName ("*wine*"), juce::String ("Wine"));
            expectEquals (friendlyAppName ("*"), juce::String ("*"));               // nothing to work with: show it as it is
            expectEquals (friendlyAppName ("  vlc "), juce::String ("Vlc"));
            expectEquals (describePatterns (juce::StringArray ("a", "b")), juce::String ("a, b"));
        }
    }

    void testPresetLabel()
    {
        AppController::Options options;
        options.demo = true;
        options.persist = false;
        options.startBackend = false;

        AppController controller (options);
        controller.start();
        auto& state = controller.getState();

        beginTest ("the mic starts on the Clean voice preset");
        expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("clean"));

        beginTest ("applying a preset records it");
        {
            controller.applyMicPreset ("broadcast");
            expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("broadcast"));
            controller.applyMicPreset ("off");
            expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("off"));
        }

        beginTest ("adding, moving and removing mic effects makes it custom; other channels do not");
        {
            controller.applyMicPreset ("clean");
            expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("clean"));

            controller.addBuiltinFx ("system", "gate");
            expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("clean"));

            const auto uid = controller.addBuiltinFx (values::micChannelId, "gate");
            expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("custom"));

            controller.applyMicPreset ("clean");
            controller.moveFx (values::micChannelId, uid, 0);           // the gate is gone with the preset: nothing to move
            controller.moveFx (values::micChannelId, AppState::getFx (state.getMicChannel()).getChild (2)[ids::uid].toString(), 0);
            expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("custom"));

            controller.applyMicPreset ("clean");
            controller.removeFx (values::micChannelId, AppState::getFx (state.getMicChannel()).getChild (0)[ids::uid].toString());
            expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("custom"));

            controller.setFxBypassed (values::micChannelId, AppState::getFx (state.getMicChannel()).getChild (0)[ids::uid].toString(), true);
            controller.applyMicPreset ("clean");
            controller.setFxBypassed (values::micChannelId, AppState::getFx (state.getMicChannel()).getChild (0)[ids::uid].toString(), true);
            expectEquals (state.getSetting (ids::micPreset).toString(), juce::String ("clean"));       // bypass is not a structural edit
        }

        beginTest ("the UI size setting defaults to automatic");
        expect (UiScale::isAutomatic (state.getSetting (ids::uiScale)));
    }
};

static UiSupportTests uiSupportTests;
}
