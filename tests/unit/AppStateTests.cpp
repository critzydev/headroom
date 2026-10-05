// Unit tests for AppState: first-run defaults, channel helpers, save/load round trip, corruption handling.
#include "Juce.h"

#include "app/AppState.h"
#include "backend/RuleMatcher.h"

#include <set>

namespace hr
{
class AppStateTests final : public juce::UnitTest
{
public:
    AppStateTests() : juce::UnitTest ("AppState", "headroom") {}

    void runTest() override
    {
        beginTest ("first-run defaults");
        testDefaults();

        beginTest ("seeded routing rules");
        testSeededRules();

        beginTest ("channel helpers");
        testChannelHelpers();

        beginTest ("save / load round trip");
        testRoundTrip();

        beginTest ("corrupt or newer files fall back to defaults and keep a .bak");
        testCorruptFiles();

        beginTest ("loaded models are repaired");
        testRepair();

        beginTest ("keepDefault: on by default, persisted, a missing attribute means on");
        testKeepDefault();
    }

private:
    static juce::File makeTempDir()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("headroom-test-" + juce::String::toHexString (juce::Random::getSystemRandom().nextInt64()));
        dir.createDirectory();
        return dir;
    }

    void testDefaults()
    {
        AppState state;
        const auto channels = state.getChannelInfos();

        expectEquals ((int) channels.size(), 5);

        if (channels.size() != 5)
            return;

        expect (channels[0].isMic());
        expectEquals (channels[0].id, juce::String ("mic"));
        expectEquals (channels[0].slot, -1);
        expectEquals (channels[0].monitorDb, kMinusInfDb);
        expect (! channels[0].monitorMuted);   // fader at -inf, not muted: a red mute icon reads as "mic muted"
        expect (! channels[0].streamMuted);

        const char* expectedIds[]     { "system", "game",    "music",  "chat" };
        const char* expectedNames[]   { "System", "Game",    "Music",  "Chat" };
        const char* expectedColours[] { "8C95A3", "5B9BE6",  "3FB8AF", "E0708F" };
        const char* expectedIcons[]   { "monitor", "gamepad", "music", "message" };

        std::set<int> slots;

        for (int i = 0; i < 4; ++i)
        {
            const auto& c = channels[(size_t) i + 1];
            expect (! c.isMic());
            expectEquals (c.id, juce::String (expectedIds[i]));
            expectEquals (c.name, juce::String (expectedNames[i]));
            expectEquals (c.colour, juce::String (expectedColours[i]));
            expectEquals (c.icon, juce::String (expectedIcons[i]));
            expectEquals (c.slot, i);
            slots.insert (c.slot);
            expectEquals (c.monitorDb, 0.0f);
            expect (! c.monitorMuted);
        }

        expectEquals ((int) slots.size(), 4);       // unique

        expectEquals (state.getLatency(), 256);
        expect (state.getMakeDefault());
        expect (state.getKeepDefault());                                // the lock is on unless the user turns it off
        expectEquals (state.getFallbackChannel(), juce::String ("system"));
        expect (state.getMix (Mix::Monitor).isValid() && state.getMix (Mix::Stream).isValid());
        expectEquals ((int) state.getMix (Mix::Monitor)[ids::limiter], 1);

        // Default mic chain: noise -> eq -> comp -> limit, empty state, unique uids and crash ids.
        auto fx = AppState::getFx (state.getMicChannel());
        expectEquals (fx.getNumChildren(), 4);

        const char* chain[] { "noise", "eq", "comp", "limit" };
        std::set<juce::String> uids;
        std::set<uint32_t> crashIds;

        for (int i = 0; i < fx.getNumChildren(); ++i)
        {
            const auto slot = AppState::describeSlot (fx.getChild (i));
            expect (slot.isBuiltin());
            expectEquals (slot.builtinId, juce::String (chain[i]));
            expect (slot.stateBase64.isEmpty());
            expect (! slot.bypassed && ! slot.disabled);
            expect (slot.crashId != 0);
            uids.insert (slot.uid);
            crashIds.insert (slot.crashId);
        }

        expectEquals ((int) uids.size(), 4);
        expectEquals ((int) crashIds.size(), 4);

        // App channels start with an empty chain.
        expectEquals (AppState::getFx (state.findChannel ("chat")).getNumChildren(), 0);

        expect (state.nextCrashId() > *crashIds.rbegin());
        expectEquals (state.nextFreeSlot(), 4);
        expectEquals (state.nextFreeColour(), juce::String ("9D7BE0"));
    }

    void testSeededRules()
    {
        AppState state;
        const auto rules = state.getRoutingRules();
        expectEquals ((int) rules.size(), 9);

        auto route = [&] (const char* binary, const char* appName)
        {
            return matchRoutingRule (rules, binary, appName);
        };

        expectEquals (route ("discord", "Discord"), juce::String ("chat"));
        expectEquals (route ("vesktop", ""), juce::String ("chat"));
        expectEquals (route ("webcord", ""), juce::String ("chat"));
        expectEquals (route ("chrome", "WEBRTC VoiceEngine"), juce::String ("chat"));
        expectEquals (route ("spotify", "Spotify"), juce::String ("music"));
        expectEquals (route ("cyberpunk2077.exe", ""), juce::String ("game"));
        expectEquals (route ("wine64-preloader", ""), juce::String ("game"));
        expectEquals (route ("proton", ""), juce::String ("game"));
        expectEquals (route ("steam_app_1091500", ""), juce::String ("game"));
        expectEquals (route ("firefox", "Firefox"), juce::String());    // no rule: stays on the default channel

        // A user's rule beats the seeded wildcards even though it was added later.
        state.setRule ("Cyberpunk2077.exe", "music");
        expectEquals (matchRoutingRule (state.getRoutingRules(), "cyberpunk2077.exe", ""), juce::String ("music"));
        expectEquals ((int) state.getRoutingRules().size(), 10);

        state.setRule ("cyberpunk2077.exe", "chat");            // upsert, not a duplicate
        expectEquals ((int) state.getRoutingRules().size(), 10);
        expectEquals (matchRoutingRule (state.getRoutingRules(), "cyberpunk2077.exe", ""), juce::String ("chat"));

        state.removeRule ("cyberpunk2077.exe");
        expectEquals (matchRoutingRule (state.getRoutingRules(), "cyberpunk2077.exe", ""), juce::String ("game"));
    }

    void testChannelHelpers()
    {
        AppState state;

        auto added = state.addAppChannel ("Stream Deck");
        expect (added.isValid());
        expectEquals (added[ids::id].toString(), juce::String ("stream-deck"));
        expectEquals ((int) added[ids::slot], 4);
        expectEquals (added[ids::colour].toString(), juce::String ("9D7BE0"));   // first palette colour nobody uses

        auto unnamed = state.addAppChannel();
        expect (unnamed.isValid());
        expectEquals (unnamed[ids::name].toString(), juce::String ("Channel 6"));
        expectEquals ((int) unnamed[ids::slot], 5);

        auto duplicate = state.addAppChannel ("Stream Deck");
        expectEquals (duplicate[ids::id].toString(), juce::String ("stream-deck-2"));

        // Fill every slot; the 17th channel cannot be added.
        int guard = 0;

        while (state.addAppChannel().isValid() && ++guard < 32)
        {}

        expectEquals (state.getChannels().getNumChildren(), 1 + kMaxAppChannels);
        expect (! state.addAppChannel().isValid());

        // Removing frees the slot and cleans up rules / fallback.
        expect (! state.removeChannel ("mic"));
        expect (state.removeChannel ("game"));
        expectEquals (state.nextFreeSlot(), 1);

        for (const auto& rule : state.getRoutingRules())
            expect (rule.channelId != "game");

        expect (state.removeChannel ("system"));
        expect (state.getFallbackChannel() != "system" && state.findChannel (state.getFallbackChannel()).isValid());

        // FX slots
        auto slot = state.addBuiltinSlot ("music", "gate");
        expect (slot.isValid());
        expectEquals (slot[ids::name].toString(), juce::String ("Noise Gate"));
        expect (state.findSlot (slot[ids::uid].toString()).isValid());

        state.addBuiltinSlot ("music", "deess");
        auto fx = AppState::getFx (state.findChannel ("music"));
        expectEquals (fx.getNumChildren(), 2);
        expect (state.moveSlot (slot[ids::uid].toString(), 1));
        expectEquals (fx.getChild (1)[ids::uid].toString(), slot[ids::uid].toString());
        expect (state.removeSlot (slot[ids::uid].toString()));
        expectEquals (fx.getNumChildren(), 1);
        expect (! state.addBuiltinSlot ("nope", "gate").isValid());
    }

    void testRoundTrip()
    {
        auto dir = makeTempDir();
        const auto file = dir.getChildFile ("state.xml");

        juce::String savedXml;
        uint32_t maxCrashId = 0;

        {
            AppState a (file);
            expect (a.load() == AppState::LoadResult::CreatedDefaults);

            a.addAppChannel ("Alt");
            a.getMicChannel().setProperty (ids::streamDb, -6.5, nullptr);
            a.findChannel ("music").setProperty (ids::monitorMuted, 1, nullptr);
            a.addBuiltinSlot ("chat", "comp");
            a.setSlotState (AppState::getFx (a.getMicChannel()).getChild (0)[ids::uid].toString(), "AAECAw==", true);
            a.setRule ("foo.exe", "alt");
            a.setSetting (ids::latency, 512);
            a.setSetting (ids::pluginFolders, "/opt/plugins\n/home/me/vst3");

            expect (a.hasUnsavedChanges());
            expect (a.saveNow());
            expect (! a.hasUnsavedChanges());
            expect (file.existsAsFile());

            savedXml = a.getRoot().createXml()->toString();
            maxCrashId = a.nextCrashId();
        }

        AppState b (file);
        expect (b.load() == AppState::LoadResult::Loaded);
        expect (! b.hasUnsavedChanges());

        expectEquals (b.getRoot().createXml()->toString(), savedXml);
        expectEquals (b.getLatency(), 512);
        expectEquals (b.getChannelInfos().size(), (size_t) 6);
        expectEquals (b.describe (b.getMicChannel()).streamDb, -6.5f);
        expect (b.describe (b.findChannel ("music")).monitorMuted);
        expectEquals (AppState::getFx (b.findChannel ("chat")).getNumChildren(), 1);
        expectEquals (AppState::getFx (b.getMicChannel()).getChild (0)[ids::state].toString(), juce::String ("AAECAw=="));
        expectEquals (matchRoutingRule (b.getRoutingRules(), "foo.exe", ""), juce::String ("alt"));
        expectEquals (b.getPluginFolders().size(), 2);
        expect (b.nextCrashId() >= maxCrashId);     // counters never go backwards across a restart

        dir.deleteRecursively();
    }

    void testKeepDefault()
    {
        auto dir = makeTempDir();
        const auto file = dir.getChildFile ("state.xml");

        {
            AppState a (file);
            expect (a.load() == AppState::LoadResult::CreatedDefaults);
            expect (a.getKeepDefault());
            expectEquals ((int) a.getSetting (ids::keepDefault), 1);

            a.setSetting (ids::keepDefault, 0);
            expect (! a.getKeepDefault());
            expect (a.getMakeDefault());                                // independent of the switch it depends on in the UI
            expect (a.saveNow());
        }

        {
            AppState b (file);
            expect (b.load() == AppState::LoadResult::Loaded);
            expect (! b.getKeepDefault());                              // off survived the restart

            b.setSetting (ids::keepDefault, 1);
            expect (b.saveNow());
        }

        {
            AppState c (file);
            expect (c.load() == AppState::LoadResult::Loaded);
            expect (c.getKeepDefault());                                // ... and so did on
        }

        // A state.xml from before the setting existed (it has makeDefault but no keepDefault) means on, even when
        // makeDefault is off: the two are independent, and turning makeDefault back on then behaves as documented.
        file.replaceWithText (
            "<HEADROOM version=\"1\">"
              "<SETTINGS latency=\"256\" makeDefault=\"0\"/>"
            "</HEADROOM>");

        {
            AppState d (file);
            expect (d.load() == AppState::LoadResult::Loaded);
            expect (d.getKeepDefault());
            expect (! d.getMakeDefault());                              // what was there is untouched
        }

        // An explicit 0 in the file stays 0 (the repair only fills in what is missing).
        file.replaceWithText (
            "<HEADROOM version=\"1\">"
              "<SETTINGS keepDefault=\"0\"/>"
            "</HEADROOM>");

        {
            AppState e (file);
            expect (e.load() == AppState::LoadResult::Loaded);
            expect (! e.getKeepDefault());
            expect (e.getMakeDefault());
        }

        dir.deleteRecursively();
    }

    void testCorruptFiles()
    {
        auto dir = makeTempDir();
        const auto file = dir.getChildFile ("state.xml");
        const auto backup = dir.getChildFile ("state.xml.bak");

        // 1. Not XML at all.
        file.replaceWithText ("this is <<< not xml");
        {
            AppState s (file);
            expect (s.load() == AppState::LoadResult::RecoveredFromCorrupt);
            expectEquals ((int) s.getChannelInfos().size(), 5);
            expect (backup.existsAsFile());
            expectEquals (backup.loadFileAsString(), juce::String ("this is <<< not xml"));
            expect (s.hasUnsavedChanges());     // defaults will replace the broken file
        }

        // 2. XML with the wrong root element.
        backup.deleteFile();
        file.replaceWithText ("<SOMETHING_ELSE version=\"1\"/>");
        {
            AppState s (file);
            expect (s.load() == AppState::LoadResult::RecoveredFromCorrupt);
            expect (backup.existsAsFile());
        }

        // 3. Written by a newer Headroom.
        backup.deleteFile();
        file.replaceWithText ("<HEADROOM version=\"99\"><SETTINGS latency=\"128\"/></HEADROOM>");
        {
            AppState s (file);
            expect (s.load() == AppState::LoadResult::UnsupportedVersion);
            expectEquals (s.getLatency(), 256);     // the newer file's values were not used
            expect (backup.existsAsFile());
            expect (backup.loadFileAsString().contains ("version=\"99\""));
        }

        dir.deleteRecursively();
    }

    void testRepair()
    {
        auto dir = makeTempDir();
        const auto file = dir.getChildFile ("state.xml");

        // No mic, two channels fighting over slot 3, a slot without uid, garbage slot type, dangling fallback.
        file.replaceWithText (
            "<HEADROOM version=\"1\">"
              "<SETTINGS latency=\"128\" fallbackChannel=\"ghost\"/>"
              "<CHANNELS>"
                "<CHANNEL id=\"x\" name=\"X\" slot=\"3\"><FX>"
                  "<SLOT type=\"builtin\" builtinId=\"comp\" crashId=\"7\"/>"
                  "<SLOT uid=\"dup1\" type=\"builtin\" builtinId=\"eq\" crashId=\"7\"/>"
                  "<SLOT uid=\"dup1\" type=\"builtin\" builtinId=\"gate\" crashId=\"0\"/>"
                  "<SLOT uid=\"bad\" type=\"weird\"/>"
                "</FX></CHANNEL>"
                "<CHANNEL id=\"y\" name=\"Y\" slot=\"3\"/>"
              "</CHANNELS>"
            "</HEADROOM>");

        AppState state (file);
        expect (state.load() == AppState::LoadResult::Loaded);

        const auto channels = state.getChannelInfos();
        expectEquals ((int) channels.size(), 3);
        expect (channels[0].isMic());                                   // mic was created and put first
        expectEquals (channels[0].monitorDb, kMinusInfDb);

        std::set<int> slots;

        for (const auto& c : channels)
            if (! c.isMic())
                slots.insert (c.slot);

        expectEquals ((int) slots.size(), 2);                           // no shared slot any more
        expectEquals (state.getLatency(), 128);                         // what was there survives
        expectEquals ((int) state.getSetting (ids::makeDefault), 1);    // what was missing is filled in
        expect (state.getMix (Mix::Monitor).isValid() && state.getMix (Mix::Stream).isValid());
        expect (state.getRouting().isValid());
        expectEquals (state.getFallbackChannel(), juce::String ("x"));  // repointed at an existing app channel

        auto fx = AppState::getFx (state.findChannel ("x"));
        expectEquals (fx.getNumChildren(), 3);                          // the garbage slot type is dropped

        std::set<juce::String> uids;
        std::set<uint32_t> crashIds;

        for (int i = 0; i < fx.getNumChildren(); ++i)
        {
            const auto slot = AppState::describeSlot (fx.getChild (i));
            expect (slot.uid.isNotEmpty() && slot.crashId != 0);
            uids.insert (slot.uid);
            crashIds.insert (slot.crashId);
        }

        expectEquals ((int) uids.size(), 3);
        expectEquals ((int) crashIds.size(), 3);

        dir.deleteRecursively();
    }
};

static AppStateTests appStateTests;
}
