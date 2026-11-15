#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/grass.h"
#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"
#include "nlohmann/json.hpp"

#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace sfs
{

class IRenderer;
struct GrassComponent;
struct TerrainMaterial;
class MeshLibrary;
class Registry;
class WorkerPool;

/**
 * Render module that grows grass (GrassComponent) on entities' meshes
 * (MeshRendererComponent), and flattens it where things move through it.
 *
 * Each entity's ground (GrassGround) is built from its mesh and how it's
 * turned and scaled, and uploaded once; the renderer grows the blades on it
 * (keeping them in tiles until what they grow from changes: see
 * GrassTileCache). Each update, its trample map springs back and is pressed
 * by the frame's benders: rigid bodies that bend grass (sized by their
 * colliders) and GrassBenderComponent entities.
 *
 * A big mesh (much wider than twice its grass's draw distance: the land out
 * to the horizon, say) grows grass only in a window around the camera, a
 * little wider than twice the draw distance: its ground and maps are over
 * that window alone, kept small. As the camera moves on, the window is built
 * again around it in the background and swapped in when ready, so there's no
 * hitch; its blades stay where they were (they're placed from the mesh's own
 * origin, not the window's). Its maps start from `paint` in each new window;
 * a game's changes to them (stamp() and the rest) last only as long as the
 * window does. Where two meshes meet (a field set into a hole in the land),
 * their windows' texels line up, so the two grasses meet without a seam.
 *
 * On a terrain (the mesh's first material, as drawn: a visual style's variant
 * may be one) whose layers keep grass off (TerrainLayer::grass), it grows
 * only as much as they let it, x its maps: bare where dirt is painted. The
 * maps themselves aren't changed by that.
 *
 * Its maps (GrassMaps: where it grows, how long, and data for its material)
 * start as the component's `paint`, and a game changes them as it plays
 * through maps(), stamp() and the rest, in world space: only what changed goes
 * up to the GPU, and only the blades over where coverage or length changed
 * are grown again. Change grass only through these (and terrainPainted()):
 * anything else isn't seen. Those changes aren't saved in the scene (it holds
 * where grass starts): a game saves them itself (GrassMaps::toJson()) if it
 * wants them kept. Setting the component's `paint` starts them over from it.
 */
class GrassModule : public CommandModule<RenderContext, GrassDrawCommand>
{
public:
  explicit GrassModule(IRenderer& renderer);
  ~GrassModule() override;

  GrassModule(const GrassModule&) = delete;
  GrassModule& operator=(const GrassModule&) = delete;

  void init(const ModuleInit& m) override { m_registry = m.registry; }

  void update(double deltaTime) override;

  void computeCommands(const RenderContext& context) override;

  // --- an entity's maps, in world space
  // ---------------------------------------

  /**
   * An entity's grass's maps as they are now, to read or change (changes show
   * from the next frame). Unpainted grass gets maps over all its ground here,
   * full. Null if it has no grass, or its mesh isn't loaded yet, or nothing
   * on it faces up enough to grow any.
   */
  GrassMaps* maps(const Entity& entity);

  /** A channel at a world point (straight down onto the grass's mesh). */
  float sample(const Entity& entity,
               GrassMaps::Channel channel,
               glm::vec3 point) const;

  /**
   * Brush a value into a channel over a world circle (see GrassMaps::stamp()),
   * `radius` world units: e.g. lower Coverage where wheels go, a little each
   * time, to wear a path.
   */
  void stamp(const Entity& entity,
             GrassMaps::Channel channel,
             glm::vec3 center,
             float radius,
             float value,
             GrassMaps::Blend blend = GrassMaps::Blend::Toward,
             float strength = 1.0f,
             float softness = 0.5f);

  /** A channel to `value` everywhere. */
  void fill(const Entity& entity, GrassMaps::Channel channel, float value);

  /**
   * The terrain under an entity's grass was painted (its splat map changed):
   * how much grows where, again, from the next frame.
   */
  void terrainPainted(const Entity& entity);

  // --- growth (see GrassMaps' growth)
  // ----------------------------------------

  /**
   * The clock grass grows by: days of growth (GrassMaps::setClock()), for all
   * its maps, from the next frame (a game runs it slower in a drought).
   */
  void setClock(double growthDays);

  /**
   * How an entity's grass grows by a field over the world (how wet the
   * ground is, say): its field (null: none; shared, so several grasses can
   * take one) and how (GrassVigourSettings). Set it as often as it changes
   * (its settings each frame, if they do: they cost nothing); the field goes
   * up to the GPU when it's another one.
   */
  void setVigour(const Entity& entity,
                 std::shared_ptr<const GrassVigourField> field,
                 const GrassVigourSettings& settings);

  /**
   * Mow a strip (GrassCut, in world space: `a` to `b` world XY, `width` world
   * units across): its blades cut to the line of its edges.
   */
  void cut(const Entity& entity, GrassCut cut);
  /**
   * Lay mown grass over the way it was rolled (GrassRoll, in world space, as
   * cut()'s): its blades leaning that way (a striping roller's stripes).
   */
  void roll(const Entity& entity, GrassRoll roll);

  /**
   * Lay grass flat over a world circle (GrassMaps::flatten()): no longer than
   * `to` of its length, standing up again over time.
   */
  void flatten(const Entity& entity,
               glm::vec3 center,
               float radius,
               float to,
               float softness = 0.5f);

  /**
   * How long an entity's blades are at a world point now, of their full
   * height (GrassComponent::height): as grown, and by its vigour.
   */
  float lengthAt(const Entity& entity, glm::vec3 point) const;

  // --- for tests and tools
  // ----------------------------------------------------

  /** The benders pressing grass this update. */
  std::span<const GrassBender> benders() const { return m_benders; }

  /** An entity's grass's ground, if it has some. */
  const GrassGround* ground(const Entity& entity) const;

  /** How flattened an entity's grass is at a world XY point, 0 to 1. */
  float flattened(const Entity& entity, glm::vec2 point) const;

  /**
   * How far a big mesh's grass window reaches either side of the camera
   * (world units), for grass drawn to `drawDistance`: a little past it, and
   * by windowSlack() more. (Past the window, the grass's ground carries on
   * over its terrain's meadow layers, see grassFarGround().)
   */
  static float windowReach(float drawDistance);
  /** How far the camera moves from the window's middle before it's built
   * again around it (world units). */
  static float windowSlack(float drawDistance);

  /** Whether an entity's grass grows in a window around the camera. */
  bool windowed(const Entity& entity) const;

  /** Wait for any window being built in the background, and use it. */
  void finishWindows();

private:
  // A big mesh's triangles, kept to build its windows from in the
  // background.
  struct Source
  {
    std::vector<glm::vec3> positions;
    std::vector<uint32_t> indices;
    std::vector<glm::vec2> uvs;
    glm::vec2 min{0.0f}, max{0.0f}; // its own XY
  };
  // All of a grid's maps as sent up (GrassMapsImage's shape, data, now and
  // cut), worked out with a window in the background so a new window costs
  // the frame that puts it in place only the upload.
  struct MapImages
  {
    GrassGrid grid;
    std::vector<uint8_t> shape, data, now, cut;
  };
  // A window of it, built.
  struct Window
  {
    glm::vec2 center{0.0f}; // its own XY
    float half = 0.0f;
    glm::mat4 model{1.0f};
    float trampleHeight = 0.0f;
    GrassGround ground;
    GrassTrample trample;
    GrassMaps maps;
    std::vector<uint8_t> cover; // with a terrain under it
    MapImages images;           // its maps', all of them
  };

  struct Patch
  {
    uint32_t mesh = 0;
    glm::mat3 turn{1.0f}; // the placement's turn and scale, the ground's
    glm::mat4 model{1.0f};
    GrassGround ground;
    GrassHandle gpu;

    nlohmann::json paint; // the component's, as last read into `maps`
    GrassMaps maps;
    GrassGrid uploadedGrid; // the maps' the GPU has
    bool sendMaps = true;   // all of them, next upload

    GrassTrample trample;
    glm::ivec2 uploadedTrample{0, 0}; // the size of the map the GPU has
    float trampleHeight = 0.0f; // what `trample` was made for, with `model`
    // A terrain under it keeping grass off some layers: how much grows on
    // each of the maps' texels (255 all), worked out for `coverGrid`.
    std::shared_ptr<const TerrainMaterial> terrain;
    std::vector<uint8_t> cover;
    GrassGrid coverGrid;
    bool coverMadeMaps = false; // the maps were made (full) for it
    bool coverStale = false;    // its terrain was painted: worked out again
    // Where it changed since the maps were last sent up (texels), and the
    // blades to grow again for it.
    glm::ivec2 coverFrom{0, 0}, coverTo{0, 0};
    std::vector<glm::ivec4> coverRegrow;
    float recovery = 3.0f;
    bool seen = false; // this frame

    // How it grows by a field over the world (setVigour()), and the field the
    // GPU has (for its handle `vigourGpu`).
    std::shared_ptr<const GrassVigourField> vigourField;
    GrassVigourSettings vigour;
    const GrassVigourField* vigourSent = nullptr;
    uint32_t vigourGpu = 0;
    int framesLogged = 0; // SFS_GRASS_GROWTH's
    std::size_t tilesSent = 0;

    // A big mesh's grass: in a window around the camera (its middle and
    // reach, the mesh's own XY), built again as the camera moves on.
    bool windowed = false;
    bool windowStale = false; // its paint, terrain or placement changed
    glm::vec2 windowCenter{0.0f};
    float windowHalf = 0.0f; // 0: none built yet
    std::shared_ptr<const Source> source;
    std::shared_ptr<const GrassMaps> painted; // `paint`, over all of it
    std::future<Window> nextWindow;           // being built
    MapImages ready; // the window's maps' images, till they're sent up
  };

  static uint64_t keyOf(const Entity& entity);
  void gatherBenders(float seconds);
  // The entity's patch, made or brought up to date (null if it can't have
  // one); and its maps sent up if they changed.
  Patch* refresh(const Entity& entity);
  void uploadMaps(Patch& patch);
  // What changed of its trample map sent up (all of it, for a new one).
  void uploadTrample(Patch& patch);
  // The terrain under a patch (null if none keeps grass off), and how much
  // it lets grow over the maps' grid.
  std::shared_ptr<const TerrainMaterial> terrainUnder(const Entity& entity);
  static void workOutCover(Patch& patch);
  // Again, after its terrain was painted: only where it changed is sent up,
  // and its blades grown again.
  static void coverRepainted(Patch& patch);
  static std::vector<uint8_t> coverOf(const GrassGround& ground,
                                      const GrassGrid& grid,
                                      const TerrainMaterial& terrain);
  // A window of a big mesh's ground (any thread).
  static Window buildWindow(const Source& source,
                            const glm::mat4& model,
                            glm::vec2 center,
                            float half,
                            float trampleHeight,
                            const GrassMaps* painted,
                            const TerrainMaterial* terrain,
                            std::optional<double> clock);
  // A windowed patch's window: built around the camera if it's moved on (in
  // the background, but for the first), and put in place once it's ready.
  void windowAround(Patch& patch, const GrassComponent& grass);
  void useWindow(Patch& patch, Window window);
  const Patch* find(const Entity& entity) const;

  // How it's now, of a rectangle of its maps (GrassMaps::nowPixels()), as
  // much as the terrain under it lets grow.
  static void nowOf(const GrassMaps& maps,
                    const std::vector<uint8_t>& cover,
                    glm::ivec2 from,
                    glm::ivec2 to,
                    std::vector<uint8_t>& now,
                    std::vector<uint8_t>& cut);
  // The cover a patch's maps are drawn with (empty: none).
  static const std::vector<uint8_t>& coverFor(const Patch& patch);
  // All of the maps' images (MapImages), their band kept (keepBand()).
  static MapImages imagesOf(GrassMaps& maps, const std::vector<uint8_t>& cover);
  void uploadVigour(Patch& patch);

  IRenderer& m_renderer;
  Registry* m_registry = nullptr;
  std::optional<double> m_clock;    // setClock()'s
  MeshLibrary* m_meshes = nullptr;  // the last frame's
  MaterialHandle m_defaultMaterial; // plain green, for grass without one
  std::unordered_map<uint64_t, Patch> m_patches;

  std::vector<GrassBender> m_benders;
  std::unordered_map<uint64_t, glm::vec3> m_lastPositions; // for velocities
  std::optional<glm::vec3> m_camera; // the last frame's, for windows

  // Builds windows (last: stopped before the rest goes).
  std::unique_ptr<WorkerPool> m_worker;
};

} // namespace sfs
