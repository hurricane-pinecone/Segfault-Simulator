#pragma once

#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace sfs
{

class Entity;
class Registry;
struct MeshLodComponent;
struct MeshRendererComponent;

/**
 * Render module that draws every entity with a TransformComponent and a
 * MeshRendererComponent. Entities sharing a mesh merge into one
 * MeshDrawCommand (one instanced draw). Each instance is culled twice: against
 * the camera frustum for the lit pass, and against the sun's shadow volume for
 * the shadow pass. An entity with a MeshLodComponent draws the level for its
 * distance from the camera (two, cross-fading, near where they change), and
 * past them its impostor turned to the camera.
 */
class MeshModule : public CommandModule<RenderContext, MeshDrawCommand>
{
public:
  void init(const ModuleInit& m) override { m_registry = m.registry; }

  void computeCommands(const RenderContext& context) override;

private:
  // One entity's instance, into the command for its mesh and materials.
  void addEntity(const RenderContext& context,
                 const Entity& entity,
                 const MeshRendererComponent& renderer);

  // Where each of an instance's draws goes.
  enum Pass : uint8_t
  {
    Lit = 1,
    SunShadow = 2,
    LightShadow = 4,
    Feed = 8, // seen by the frame's camera feed (RenderContext::feedOn)
  };

  // An instance of `mesh` (with the materials in m_materials) into its
  // command's lists for `passes`.
  void
  addInstance(MeshHandle mesh, const MeshInstance& instance, uint8_t passes);

  // An entity with levels of detail: its instances for the level(s) its
  // distance from the camera picks, in the passes it's in (`seen`: Lit and,
  // or, Feed).
  void addLevels(const RenderContext& context,
                 uint32_t id,
                 const MeshLodComponent& lod,
                 MeshHandle mesh,
                 const glm::mat4& model,
                 const MeshInstance& instance,
                 uint8_t seen,
                 bool castsShadow,
                 bool castsLightShadow);

  Registry* m_registry = nullptr;

  // Each entity with levels of detail (by id): the level it's at, the one
  // it's crossing over from (the same once done) and since when (seconds),
  // and the frame it was last drawn in.
  struct LodState
  {
    uint8_t level = 0;
    uint8_t previous = 0;
    bool started = false;
    double since = 0.0;
    uint64_t seen = 0;
  };
  // Forget entities' levels not drawn lately.
  void forgetLevels();
  std::unordered_map<uint32_t, LodState> m_lods;
  uint64_t m_frame = 0;
  double m_now = 0.0; // seconds, this frame

  // Hash of (mesh, materials) -> index into m_commands, rebuilt each frame
  // (kept to reuse its buckets).
  std::unordered_multimap<std::size_t, std::size_t> m_commandIndex;
  std::vector<MaterialHandle> m_materials; // scratch: one entity's
};

} // namespace sfs
