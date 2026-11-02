#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/puddles.h"
#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"
#include "glm/glm/ext/matrix_float4x4.hpp"

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace sfs
{

class GroundSurface;
class IRenderer;
class MeshLibrary;
class Registry;
class WorkerPool;
struct TerrainMaterial;
struct TextureData;

/**
 * Render module that has the renderer find where rain water collects on
 * terrains (a MeshRendererComponent whose first material, as drawn, is a
 * terrain with TerrainMaterial::puddles), for the terrain shader to fill as
 * the weather's puddles rise (SceneWeather::puddles). It draws nothing
 * itself, and the water is only drawn: nothing on the CPU knows where it
 * stands.
 *
 * Each such entity's puddles are worked out on the GPU (see puddles.h) in a
 * window kPuddleWindow across, ahead of the camera, from the mesh's surface
 * under it, the terrain's micro relief and how far the ground's pressed in
 * (DeformationModule). This module places the window, lays the mesh's
 * surface under it in the background each time it moves (the puddles show a
 * moment after the scene loads), and has them found again when the ground's
 * pressed in (reshaped()) or the terrain's puddle settings change.
 */
class PuddleModule : public IRenderModule<RenderContext>
{
public:
  explicit PuddleModule(IRenderer& renderer);
  ~PuddleModule() override;

  PuddleModule(const PuddleModule&) = delete;
  PuddleModule& operator=(const PuddleModule&) = delete;

  void init(const ModuleInit& m) override { m_registry = m.registry; }

  /** Places the windows and hands the renderer their puddles; emits no
   * commands. */
  void emit(const RenderContext& context,
            std::vector<AnyRenderCommand>& out) override;

  /** `ground` was pressed in (DeformationModule): find its puddles again. */
  void reshaped(const Entity& ground);

  /**
   * How fast water rises in `ground`'s puddles, over its mesh's UVs (see
   * TerrainPuddles::fillRate: red 0 none stands .. 1 as fast as the
   * weather's), e.g. a game's soil: loose tilled earth soaks rain up, packed
   * tracks pond at once. Null: 1 everywhere (as it starts). Its puddles are
   * found again with it, so set it when it's changed, not every frame.
   */
  void setFillRate(const Entity& ground,
                   std::shared_ptr<const TextureData> fillRate);

  // --- for tests and tools
  // ----------------------------------------------------

  /** `ground`'s puddles as last handed to the renderer; null if none yet. */
  const TerrainPuddles* puddles(const Entity& ground) const;

  /** Wait for any ground being laid in the background, and use it. */
  void finishBakes();

private:
  struct Field
  {
    uint32_t mesh = 0;
    glm::mat4 model{1.0f};
    std::shared_ptr<const GroundSurface> surface;
    std::shared_ptr<const TerrainMaterial> terrain; // what it was laid for
    MaterialHandle material;                        // drawn with
    TerrainPuddles sent;
    glm::vec2 origin{0.0f}; // the window being laid (or last laid)
    std::future<std::shared_ptr<const std::vector<glm::vec4>>> next;
    bool stale = true; // its ground wants laying again
    bool seen = false; // this frame
  };

  static uint64_t keyOf(const Entity& entity);
  // A terrain's puddle settings, and what its layers' are drawn from.
  static bool samePuddles(const TerrainMaterial& a, const TerrainMaterial& b);
  void refresh(const Entity& entity, Field& field);
  void use(Field& field, std::shared_ptr<const std::vector<glm::vec4>> ground);
  void send(Field& field);

  IRenderer& m_renderer;
  Registry* m_registry = nullptr;
  MeshLibrary* m_meshes = nullptr; // the last frame's
  std::optional<glm::vec3> m_eye;  // the last frame's camera
  glm::vec3 m_view{1.0f, 0.0f, 0.0f};
  std::unordered_map<uint64_t, Field> m_fields;
  // setFillRate()'s, by entity (kept for terrains not drawn yet).
  std::unordered_map<uint64_t, std::shared_ptr<const TextureData>> m_fillRates;
  std::unique_ptr<WorkerPool> m_worker;
};

} // namespace sfs
