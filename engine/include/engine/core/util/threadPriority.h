#pragma once

namespace sfs
{

/**
 * Runs the calling thread below normal priority: background work (planning,
 * a pool's jobs) that may use every core but never takes a time slice the
 * game's or the renderer's thread is waiting for. Does nothing where the
 * platform has no such thing.
 */
void lowerThisThreadPriority();

} // namespace sfs
