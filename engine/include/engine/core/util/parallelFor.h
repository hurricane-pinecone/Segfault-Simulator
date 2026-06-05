#pragma once

#include <cstddef>
#include <functional>

namespace sfs
{

// Number of contiguous ranges parallelFor() will split `count` into (i.e. the
// number of participating threads, including the caller). Call this first when
// you need a per-range scratch buffer, then index it by the rangeIndex passed
// to the callback.
std::size_t parallelForRangeCount(std::size_t count);

// Splits [0, count) into parallelForRangeCount(count) contiguous ranges and
// runs `fn(begin, end, rangeIndex)` for each, in parallel across a persistent
// worker pool (the calling thread runs one range too) and blocks until all
// finish. `fn` must be safe to run concurrently on disjoint ranges. Falls back
// to a single serial range for small counts or single-core machines.
void parallelFor(
    std::size_t count,
    const std::function<void(std::size_t begin, std::size_t end, std::size_t rangeIndex)>&
        fn);

} // namespace sfs
