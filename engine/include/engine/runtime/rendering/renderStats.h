#pragma once

namespace sfs
{

// Per-frame counters for the isometric render path, surfaced in the debug
// overlay. Reset at the start of IsometricRenderSystem::render and incremented
// as items are queued and flushed. Defined in isometricRenderSystem.cpp.
extern int gTerrainShadowItems;
extern int gTerrainShadowFlushes;
extern int gRenderItemCount;
extern int gTerrainShadowEdgesProcessed;
extern int gTileRenderItems;
extern int gSpriteRenderItems;
extern int gSpriteProjectedShadowItems;
extern int gTerrainShadowBatchCount;

} // namespace sfs
