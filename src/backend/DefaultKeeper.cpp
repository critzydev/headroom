// Implementation of DefaultKeeper (debounce and revert limit for "keep Headroom as the default").
#include "backend/DefaultKeeper.h"

namespace hr
{

DefaultKeeper::Action DefaultKeeper::update (bool foreign, const std::string& what, int64_t now)
{
    while (! history.empty() && now - history.front() >= kWindowMs)
        history.pop_front();

    if (! foreign)
    {
        waiting = false;

        if (limited && history.empty())
            limited = false;            // Headroom's again, and quiet for a whole window: as good as new

        return Action::None;
    }

    if (! waiting || what != waitingFor)
    {
        waiting = true;
        waitingFor = what;
        waitingSince = now;
        return Action::None;
    }

    if (now - waitingSince < kDebounceMs)
        return Action::None;

    if (limited)
    {
        // Given up: only a value that stays put for a whole window (the other program has stopped) is put back.
        if (now - waitingSince < kWindowMs)
            return Action::None;

        history.clear();
        limited = false;
        return Action::Revert;
    }

    if ((int) history.size() >= kMaxReverts)
    {
        limited = true;
        return Action::GiveUp;
    }

    return Action::Revert;
}

void DefaultKeeper::reverted (int64_t now)
{
    history.push_back (now);
    waiting = false;            // whatever is there next is a new change, with a debounce of its own
}

void DefaultKeeper::reset()
{
    history.clear();
    waiting = false;
    waitingFor.clear();
    waitingSince = 0;
    limited = false;
}

} // namespace hr
