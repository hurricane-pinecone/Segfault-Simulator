#pragma once

namespace sfs
{

class MeshLibrary;

/**
 * Provides meshes built in code (procedural models) to every scene, by name.
 * Derive from it in a game module's scripts and add the meshes in build():
 *
 *   class GameModels : public sfs::MeshSource
 *   {
 *   public:
 *     void build(sfs::MeshLibrary& library) override
 *     {
 *       library.add(towerModel(), "mygame:tower");
 *     }
 *   };
 *
 * Each StandardScene calls build() when it is created, so scenes refer to the
 * meshes by name (the editor lists them in mesh pickers) exactly like the
 * built-in primitives. Prefix names with the game's own namespace to avoid
 * clashes.
 */
class MeshSource
{
public:
  virtual ~MeshSource() = default;

  virtual void build(MeshLibrary& library) = 0;
};

} // namespace sfs
