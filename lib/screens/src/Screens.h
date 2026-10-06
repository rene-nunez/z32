#pragma once

#include <cstdint>

// The menu chrome. The state machine stays in game: this owns the screen identity, the item
// tables and the incremental-repaint bookkeeping, so a caller cannot get the "paint once on
// entry, then only the cursor" order wrong.
class screens {
  public:
    enum class id : uint8_t { logo, team, menu, mode, scores, playing, pause, game_over, waiting };

    // items in that screen's list, 0 when it has none. The menu wrap uses it, so adding an
    // item cannot leave a stale hardcoded modulus behind
    static uint8_t count(id scr);

    // full paint on screen entry, then just the two lines whose '>' moved. playing has no
    // chrome at all and returns immediately
    static void paint(id scr, uint8_t sel);

    // force the next paint to be a full one. Anything entering playing must call this: the
    // playing paint is a no-op, so nothing else clears the cache and the next pause would
    // match the stored screen id and skip its chrome
    static void invalidate();
};
