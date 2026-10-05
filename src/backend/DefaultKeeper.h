// The "keep Headroom as the default device" decision, free of PipeWire so it can be tested anywhere.
#pragma once

#include <cstdint>
#include <deque>
#include <string>

namespace hr
{
/**
    One instance per default device (output, input). The PipeWire backend tells it, on every pass, whether the
    configured default is something other than Headroom's own node, and it says when to switch back:

      - not before the value has been the same for kDebounceMs (a device that is plugged in often changes
        the default twice in a row, and the second value is the one that counts);
      - at most kMaxReverts times per kWindowMs. The eleventh time is a program that is fighting Headroom, and
        going on would only make things worse: it gives up (once, so the caller can say so) and stays quiet.
        It tries again when the other program has left the value alone for a whole window, and the "given up"
        state ends when the default is Headroom's again and nothing has been reverted for a whole window.

    All times are milliseconds on any steady clock.
*/
class DefaultKeeper
{
public:
    static constexpr int64_t kDebounceMs = 300;
    static constexpr int kMaxReverts = 10;
    static constexpr int64_t kWindowMs = 60000;

    enum class Action
    {
        None,       // nothing to do (yet)
        Revert,     // put Headroom's device back, then call reverted()
        GiveUp      // the limit was just reached: log it. Reverting stops (gaveUp() is true)
    };

    /** Call on every pass. `foreign` is false while the configured default is Headroom's own node (or is not
        being kept); otherwise `what` names the other value ("" = it was cleared). */
    Action update (bool foreign, const std::string& what, int64_t nowMs);

    /** The revert was written. Counts against the limit. */
    void reverted (int64_t nowMs);

    /** True from the moment the limit was hit until the situation has calmed down. */
    bool gaveUp() const noexcept    { return limited; }

    /** Forget everything (the setting was switched, the metadata was recreated, ...). */
    void reset();

private:
    std::deque<int64_t> history;        // when the recent reverts happened
    bool waiting = false;               // a foreign value has been seen and is being debounced
    std::string waitingFor;
    int64_t waitingSince = 0;
    bool limited = false;
};
}
