#include "gs/sequence_tracker.hpp"

namespace gs {

SeqVerdict SequenceTracker::observe(uint32_t seq) {
    if (!started_) {
        started_ = true;
        highest_ = seq;
        mark(seq);
        ++stats_.received;
        return SeqVerdict::First;
    }

    // Signed distance handles uint32 wrap-around.
    const int32_t delta = static_cast<int32_t>(seq - highest_);

    if (delta > 0) {
        // Advance: clear the slots we skip over so stale bits from a previous lap
        // don't look like "seen".
        const uint32_t skipped = static_cast<uint32_t>(delta) - 1;
        const uint32_t to_clear = delta >= static_cast<int32_t>(kWindow) ? kWindow : static_cast<uint32_t>(delta);
        for (uint32_t i = 1; i <= to_clear; ++i) window_[(highest_ + i) % kWindow] = false;
        highest_ = seq;
        mark(seq);
        ++stats_.received;
        if (skipped == 0) return SeqVerdict::InOrder;
        stats_.lost += skipped;
        return SeqVerdict::Gap;
    }

    if (delta == 0 || (-delta < static_cast<int32_t>(kWindow) && seen(seq))) {
        ++stats_.duplicates;
        return SeqVerdict::Duplicate;
    }

    if (-delta >= static_cast<int32_t>(kWindow)) {
        ++stats_.too_old;
        return SeqVerdict::TooOld;
    }

    // Behind the head, inside the window, not seen yet: it was counted as lost.
    mark(seq);
    ++stats_.received;
    ++stats_.reordered;
    if (stats_.lost > 0) --stats_.lost;
    return SeqVerdict::Reordered;
}

}  // namespace gs
