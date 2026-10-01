#pragma once
// Classifies each received sequence number as in-order, gap (with N lost),
// duplicate, or late/out-of-order, using a sliding bitmap of recently seen
// sequence numbers. A packet counted as lost that later arrives is reclassified
// as reordered and the loss counter is corrected.
#include <bitset>
#include <cstdint>

namespace gs {

enum class SeqVerdict { First, InOrder, Gap, Duplicate, Reordered, TooOld };

struct SeqStats {
    uint64_t received = 0;     // unique packets accepted
    uint64_t lost = 0;         // currently-missing sequence numbers
    uint64_t duplicates = 0;
    uint64_t reordered = 0;    // arrived after a higher seq
    uint64_t too_old = 0;      // fell outside the window; can't classify
};

class SequenceTracker {
public:
    static constexpr uint32_t kWindow = 1024;

    SeqVerdict observe(uint32_t seq);
    const SeqStats& stats() const { return stats_; }
    uint32_t highest() const { return highest_; }

private:
    bool seen(uint32_t seq) const { return window_[seq % kWindow]; }
    void mark(uint32_t seq) { window_[seq % kWindow] = true; }

    bool started_ = false;
    uint32_t highest_ = 0;
    std::bitset<kWindow> window_;
    SeqStats stats_;
};

}  // namespace gs
