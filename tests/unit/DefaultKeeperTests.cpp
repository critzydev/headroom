// Unit tests for DefaultKeeper: the debounce and the revert limit behind "keep Headroom as the default device".
#include "Juce.h"

#include "backend/DefaultKeeper.h"

#include <string>

namespace hr
{
class DefaultKeeperTests final : public juce::UnitTest
{
public:
    DefaultKeeperTests() : juce::UnitTest ("DefaultKeeper", "headroom") {}

    void runTest() override
    {
        using Action = DefaultKeeper::Action;

        beginTest ("nothing happens while the default is Headroom's");
        {
            DefaultKeeper keeper;

            for (int64_t t = 0; t < 5000; t += 100)
                expect (keeper.update (false, {}, t) == Action::None);

            expect (! keeper.gaveUp());
        }

        beginTest ("another device is put back once it has been there for the debounce time");
        {
            DefaultKeeper keeper;
            expect (keeper.update (true, "alsa_output.usb-headset", 1000) == Action::None);      // just appeared
            expect (keeper.update (true, "alsa_output.usb-headset", 1100) == Action::None);
            expect (keeper.update (true, "alsa_output.usb-headset", 1299) == Action::None);
            expect (keeper.update (true, "alsa_output.usb-headset", 1300) == Action::Revert);    // 300 ms
            expect (keeper.update (true, "alsa_output.usb-headset", 1300) == Action::Revert);    // still asked for until it is written

            keeper.reverted (1300);
            expect (keeper.update (false, {}, 1400) == Action::None);                            // it worked: Headroom's again
            expect (! keeper.gaveUp());
        }

        beginTest ("a value that keeps changing is not put back until it settles");
        {
            DefaultKeeper keeper;
            expect (keeper.update (true, "a", 0) == Action::None);
            expect (keeper.update (true, "b", 200) == Action::None);        // changed again: the clock starts over
            expect (keeper.update (true, "b", 400) == Action::None);
            expect (keeper.update (true, "b", 499) == Action::None);
            expect (keeper.update (true, "b", 500) == Action::Revert);
        }

        beginTest ("a value that goes away by itself is never touched");
        {
            DefaultKeeper keeper;
            expect (keeper.update (true, "a", 0) == Action::None);
            expect (keeper.update (true, "a", 200) == Action::None);
            expect (keeper.update (false, {}, 250) == Action::None);        // the user (or the other program) put it back
            expect (keeper.update (true, "a", 300) == Action::None);        // and changed it again: a new debounce
            expect (keeper.update (true, "a", 599) == Action::None);
            expect (keeper.update (true, "a", 600) == Action::Revert);
        }

        beginTest ("a cleared default counts like any other change");
        {
            DefaultKeeper keeper;
            expect (keeper.update (true, {}, 0) == Action::None);
            expect (keeper.update (true, {}, 299) == Action::None);
            expect (keeper.update (true, {}, 300) == Action::Revert);
        }

        beginTest ("after a revert the next change has its own debounce");
        {
            DefaultKeeper keeper;
            keeper.update (true, "a", 0);
            expect (keeper.update (true, "a", 300) == Action::Revert);
            keeper.reverted (300);

            // The metadata echo has not arrived yet: the same stale value must not be reverted a second time at once.
            expect (keeper.update (true, "a", 400) == Action::None);
            expect (keeper.update (true, "a", 699) == Action::None);
            expect (keeper.update (true, "a", 700) == Action::Revert);
        }

        beginTest ("ten reverts a minute, then it gives up once and says so");
        {
            DefaultKeeper keeper;
            int64_t now = 0;

            for (int i = 0; i < DefaultKeeper::kMaxReverts; ++i)
            {
                expect (keeper.update (true, "fighter", now) == Action::None);
                now += DefaultKeeper::kDebounceMs;
                expect (keeper.update (true, "fighter", now) == Action::Revert);
                keeper.reverted (now);
                expect (keeper.update (false, {}, now + 50) == Action::None);
                now += 100;
                expect (! keeper.gaveUp());
            }

            // The eleventh within the minute.
            expect (keeper.update (true, "fighter", now) == Action::None);
            now += DefaultKeeper::kDebounceMs;
            expect (keeper.update (true, "fighter", now) == Action::GiveUp);
            expect (keeper.gaveUp());

            // ... and from then on it stays out of it (GiveUp is reported once only).
            for (int i = 0; i < 20; ++i)
            {
                now += 500;
                expect (keeper.update (true, "fighter", now) == Action::None);
                expect (keeper.gaveUp());
            }
        }

        beginTest ("slow changes never reach the limit");
        {
            DefaultKeeper keeper;
            int64_t now = 0;

            for (int i = 0; i < 50; ++i)                // one plug-in every 10 s, for over eight minutes
            {
                keeper.update (true, "usb", now);
                expect (keeper.update (true, "usb", now + DefaultKeeper::kDebounceMs) == Action::Revert);
                keeper.reverted (now + DefaultKeeper::kDebounceMs);
                keeper.update (false, {}, now + 1000);
                now += 10000;
            }

            expect (! keeper.gaveUp());
        }

        beginTest ("having given up ends when the other program stops for a whole minute");
        {
            DefaultKeeper keeper;
            int64_t now = 0;
            giveUp (keeper, now);
            expect (keeper.gaveUp());

            // It keeps flipping between two devices: still no revert, however long that goes on.
            for (int i = 0; i < 100; ++i)
            {
                now += 5000;
                expect (keeper.update (true, i % 2 == 0 ? "x" : "y", now) == Action::None);
            }

            expect (keeper.gaveUp());

            // Then it settles on one value. Nothing for a minute ...
            const auto settled = now + 1000;
            keeper.update (true, "x", settled);
            expect (keeper.update (true, "x", settled + DefaultKeeper::kWindowMs - 1) == Action::None);
            expect (keeper.gaveUp());

            // ... and after that Headroom puts its device back and everything is as it was.
            expect (keeper.update (true, "x", settled + DefaultKeeper::kWindowMs) == Action::Revert);
            expect (! keeper.gaveUp());
            keeper.reverted (settled + DefaultKeeper::kWindowMs);
            expect (keeper.update (false, {}, settled + DefaultKeeper::kWindowMs + 100) == Action::None);

            // The count started again: a single plug-in is put back like before.
            const auto later = settled + DefaultKeeper::kWindowMs + 10000;
            keeper.update (true, "z", later);
            expect (keeper.update (true, "z", later + DefaultKeeper::kDebounceMs) == Action::Revert);
        }

        beginTest ("having given up is over when the default is Headroom's and a minute has passed");
        {
            DefaultKeeper keeper;
            int64_t now = 0;
            giveUp (keeper, now);
            expect (keeper.gaveUp());

            expect (keeper.update (false, {}, now + 1000) == Action::None);          // e.g. the user chose it by hand
            expect (keeper.gaveUp());                                                // too soon to say it is over
            expect (keeper.update (false, {}, now + DefaultKeeper::kWindowMs + 1000) == Action::None);
            expect (! keeper.gaveUp());
        }

        beginTest ("reset forgets everything");
        {
            DefaultKeeper keeper;
            int64_t now = 0;
            giveUp (keeper, now);
            expect (keeper.gaveUp());

            keeper.reset();
            expect (! keeper.gaveUp());
            expect (keeper.update (true, "usb", now) == Action::None);
            expect (keeper.update (true, "usb", now + DefaultKeeper::kDebounceMs) == Action::Revert);
        }
    }

private:
    // Drives a keeper into the "given up" state; `now` ends up a little after that moment.
    void giveUp (DefaultKeeper& keeper, int64_t& now)
    {
        for (int i = 0; i < DefaultKeeper::kMaxReverts; ++i)
        {
            keeper.update (true, "fighter", now);
            now += DefaultKeeper::kDebounceMs;
            keeper.reverted (now);
            keeper.update (false, {}, now + 50);
            now += 100;
        }

        keeper.update (true, "fighter", now);
        now += DefaultKeeper::kDebounceMs;
        expect (keeper.update (true, "fighter", now) == DefaultKeeper::Action::GiveUp);
    }
};

static DefaultKeeperTests defaultKeeperTests;
}
