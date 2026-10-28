#pragma once

#include "engine/core/rendering/3d/camera3D.h"
#include "engine/runtime/rendering3d/scene3D.h"

#include <string>

// The scene the editor edits: the engine's standard 3D scene (exactly what an
// exported game plays) plus helpers for building it from the hierarchy.
class EditorScene : public sfs::Scene3D
{
public:
  using Primitive = sfs::Scene3D::BuiltinMesh;

  using sfs::Scene3D::Scene3D;

  /** A named entity with only a transform. */
  sfs::Entity createEmpty(const std::string& name);

  /** A named entity drawing one of the built-in primitives. */
  sfs::Entity createPrimitive(Primitive primitive, const std::string& name);

  /** A named entity carrying a point light. */
  sfs::Entity createPointLight(const std::string& name);

  /** A named camera entity looking the way `view` does (e.g. the editor's view). */
  sfs::Entity createCamera(const std::string& name, const sfs::Camera3D& view);

  /** Destroy an entity and everything below it (applied immediately). */
  void destroyBranch(const sfs::Entity& entity);

  /** Remove every entity (applied immediately). */
  void clear();

  /** Replace the contents with a starting layout: ground, a cube, a light. */
  void resetToDefault();

protected:
  void onInit() override;
};
