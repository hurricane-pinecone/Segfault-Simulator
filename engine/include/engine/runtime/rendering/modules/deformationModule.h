#pragma once

#include "engine/core/ecs/entity.h"
#include "engine/core/rendering/deformation.h"
#include "engine/runtime/rendering/commands.h"
#include "engine/runtime/rendering/modules/renderModule.h"
#include "engine/runtime/rendering/renderContext.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace sfs
{

class AssetStore;
class IRenderer;
struct DeformerComponent;
class MeshLibrary;
class Registry;
struct MeshMaterial;
struct TerrainMaterial;

/**
 * Render module that presses deformable terrains in (a MeshRendererComponent
 * whose first material, as drawn, is a terrain with TerrainMaterial::
 * deformable) where things go over them, and draws it.
 *
 * Each such entity keeps a DeformationMap (deformation.h): how far its ground
 * is pressed in or pushed up, a few centimetres a texel, in world space.
 * What changes it:
 *
 * - DeformerComponent entities (with their children's meshes): as one
 *   moves, each few centimetres (not each frame, so a fast frame rate costs
 *   no more), its meshes' undersides near the ground (what's seen of them
 *   from below, lugs, tines and all) press it down to their sink depth below
 *   them, deeper on softer soil (setSoftness()), and the soil they push out
 *   rises alongside. One sliding rather than rolling (a tine's point) is
 *   pressed all along the way it went since it last pressed, so its
 *   furrow's unbroken; so is one rolling that went further in a frame than
 *   it presses at (the game going fast: Registry::timeScale()).
 * - stamp(): a footprint, a hoof, a dropped crate's corner.
 * - relax() and clearRegion(): weather softening tracks, tilling wiping them.
 *
 * It's drawn as real geometry near the camera: a patch of fine rings (4 cm
 * between vertices at its middle, coarser outwards, kPatchHalf either side)
 * around where the view meets the ground (at most kReach from the camera),
 * displaced on the GPU by a window of the map (kWindowTexels square,
 * wrapping, so moving it only uploads what comes into it), with the
 * terrain's own mesh cut away under it. Out to the window's edge, the rest of
 * the ground is parallax-mapped, cross-fading with the patch over a band
 * inside its edge. Normals, shadows and the puddles follow the pressed ground.
 *
 * Where the ground's pressed in, its puddles (PuddleModule) are found with it
 * (on the GPU, from its window), so ruts fill with water after rain, and grass
 * on it (GrassComponent) is worn away (unless a game wears it itself:
 * setWearsGrass()). A game can follow where each deformer touches the ground
 * (setContacts()) for its own rules: compacting soil, wearing paths.
 *
 * Its maps aren't saved with the scene (a game saves them itself, save()).
 */
class DeformationModule : public CommandModule<RenderContext, MeshDrawCommand>
{
public:
  explicit DeformationModule(IRenderer& renderer);
  ~DeformationModule() override;

  DeformationModule(const DeformationModule&) = delete;
  DeformationModule& operator=(const DeformationModule&) = delete;

  void init(const ModuleInit& m) override;
  void setHost(RenderModuleHost<RenderContext>& host) override
  {
    m_host = &host;
  }

  void update(double deltaTime) override;

  void computeCommands(const RenderContext& context) override;

  // --- a terrain's deformation, in world space
  // ------------------------------------

  /**
   * `ground`'s map, to read or change directly (changes show from the next
   * frame). Made for any valid entity when first asked for, so a game can
   * load one before the terrain's drawn.
   */
  DeformationMap* map(const Entity& ground);

  /** How far `ground` is pressed in (negative) or pushed up at a world XY. */
  float offsetAt(const Entity& ground, glm::vec2 point) const;

  /**
   * The height of `ground`'s surface at a world XY point, pressed in as it
   * is (e.g. to settle a vehicle into its ruts); nothing off it, or before
   * it's first drawn.
   */
  std::optional<float> heightAt(const Entity& ground, glm::vec2 point) const;

  /**
   * Press a shape into `ground` (see DeformationMap::stamp()): `size` across
   * (along `heading`, radians from the world's +X, and across it), `depth`
   * deep. `shape`: an image asset whose red is the imprint (1 deepest); empty,
   * a rounded oval.
   */
  void stamp(const Entity& ground,
             glm::vec2 center,
             float heading,
             glm::vec2 size,
             float depth,
             const std::string& shape = {});

  /** Weather `ground`'s tracks (see DeformationMap::relax()). */
  void relax(const Entity& ground, float smooth, float fade = 0.0f);
  /** Weather only some of `ground`'s tiles (see DeformationMap::relax()). */
  void relax(const Entity& ground,
             std::span<const glm::ivec2> tiles,
             std::span<const glm::vec4> corners);

  /** Flatten `ground` inside a world XY polygon, or rectangle. */
  void clearRegion(const Entity& ground, std::span<const glm::vec2> polygon);
  void clearRegion(const Entity& ground, glm::vec2 low, glm::vec2 high);

  /** `ground`'s map packed small to save, and back (false if not a map). */
  std::vector<uint8_t> save(const Entity& ground) const;
  bool load(const Entity& ground, std::span<const uint8_t> bytes);

  /**
   * How soft `ground`'s soil is, 0 (hard: deformers barely mark it) to 1
   * (the softest: they sink their full depth): everywhere, or by a function
   * of the world XY (e.g. a game's soil moisture: wet mud soft, dry ground
   * hard), asked where a deformer presses: at the corners of blocks of 4 x 4
   * texels (12 cm), and between them blended (so a step in it, e.g. at the
   * edge of a game's grid cell, slopes over a block). 1 until set.
   */
  using Softness = std::function<float(glm::vec2 point)>;
  void setSoftness(const Entity& ground, float softness);
  void setSoftness(const Entity& ground, Softness softness);

  /**
   * Whether grass on `ground` (its GrassComponent's maps) is worn away
   * where deformers press it in: on until set, so ruts are bare. Off, a game
   * decides what traffic does to it (e.g. through setContacts()).
   */
  void setWearsGrass(const Entity& ground, bool wears);

  /**
   * Where a deformer's mesh touched the ground as it moved, this frame: its
   * underside's rectangle (world XY), how far it moved since it last
   * pressed (world XY; zero the first time), how much it pressed in
   * (press()'s; 0 on ground too hard to mark, though it still went over it),
   * how deep it pressed the ground (its deepest, metres below the ground's
   * own surface: as deep as it's sat down, plus its sink), and how soft the
   * ground under it is on average (setSoftness()'s): e.g. for a game to
   * tell a wheel bogging down in mud.
   */
  struct Contact
  {
    Entity deformer; // the entity with the DeformerComponent
    Entity mesh;     // its mesh that touched (a wheel, a tine)
    glm::vec2 low{0.0f}, high{0.0f};
    glm::vec2 motion{0.0f};
    float sinkDepth = 0.0f; // the deformer's
    float pressed = 0.0f;
    float depth = 0.0f;
    float softness = 0.0f;
  };
  /**
   * Called for each Contact with `ground`, as deformers press it (during the
   * render system's update). Empty: none (as it starts). It mustn't call
   * this module (it's called from inside it): note what it needs.
   */
  using Contacts = std::function<void(const Contact& contact)>;
  void setContacts(const Entity& ground, Contacts contacts);

  // The patch: half its size; its finest spacing (doubling with each of
  // kPatchRings rings); the band inside its edge where it hands over to
  // parallax; and how far from the camera its centre goes, at most.
  static constexpr int kPatchCells = 64; // a ring's cells from its middle
  static constexpr int kPatchRings = 4;
  static constexpr float kPatchSpacing = 0.04f;
  static constexpr float kPatchHalf =
      kPatchSpacing * kPatchCells * (1 << (kPatchRings - 1));
  static constexpr float kPatchBand = 4.0f;
  static constexpr float kReach = 24.0f;
  // The window of the map on the GPU, texels square (2048 at 3 cm: 61 m).
  static constexpr int kWindowTexels = 2048;
  // The ground under the patch, as the GPU reads it: texels square, and
  // their size.
  static constexpr int kGroundTexels = 256;
  static constexpr float kGroundTexel = 0.5f;
  // Seconds between telling the puddles the ground's changed (they're found
  // again on the GPU).
  static constexpr double kPuddleInterval = 0.5;

protected:
  bool rebuildsEveryFrame() const override { return true; }

private:
  struct Field
  {
    Entity entity;
    DeformationMap map;
    float softness = 1.0f;
    Softness softnessAt;
    bool wearsGrass = true;
    Contacts contacts;
    // Its mesh as placed, to ask its height.
    uint32_t mesh = 0;
    glm::mat4 model{1.0f};
    std::shared_ptr<const GroundSurface> surface;
    // Drawn with (given its deformation), and its patch's copy of it.
    MaterialHandle material;
    MaterialHandle patch;
    std::shared_ptr<const MeshMaterial> patchMadeFrom;
    // What the GPU has: the window, and the ground under the patch.
    bool windowed = false;
    glm::ivec2 windowFrom{0};
    std::shared_ptr<const std::vector<glm::vec4>> ground;
    glm::vec2 groundOrigin{0.0f};
    glm::vec2 patchCenter{0.0f};
    TerrainDeformation sent;
    // Its puddles want finding again.
    bool reshaped = false;
    double sincePuddles = 0.0;
    bool seen = false; // this frame
  };

  // A deformable terrain drawn this frame: its field, and its material as
  // drawn.
  struct Seen
  {
    Field* field = nullptr;
    MaterialHandle material;
    const MeshMaterial* drawn = nullptr;
  };
  static uint64_t keyOf(const Entity& entity);
  Field* fieldOf(const Entity& entity);
  const Field* fieldOf(const Entity& entity) const;
  // A deformable terrain this frame: its mesh as placed (to press into),
  // then its patch and window (to draw).
  void prepare(const Entity& entity, Field& field);
  void draw(const RenderContext& context,
            MaterialHandle material,
            const MeshMaterial& drawn,
            Field& field);
  // Press the frame's deformers into the fields they're over.
  void deform();
  void pressMesh(const Entity& mesh,
                 const glm::mat4& model,
                 const Entity& owner,
                 const DeformerComponent& deformer,
                 glm::vec2 motion,
                 bool rolled,
                 float stride,
                 float farthest);
  // The ground's height and softness under each texel of `bottom`
  // (m_groundHeight, NaN off it; m_groundSoftness).
  void groundUnder(const Field& field, const Underside& bottom);
  // A deformer mesh's underside as placed.
  Underside undersideOf(const Entity& mesh,
                        MeshHandle handle,
                        const glm::mat4& model,
                        float below,
                        float texel);
  // How low a deformer mesh reaches as placed (world Z): its lowest point
  // (a turning wheel's tyre's, wherever it's turned to; its bounds' corners
  // swing up to 1.4 x its radius below its axle), or `bound` where that
  // isn't known (posed by an animator).
  float lowestPointOf(const Entity& mesh,
                      MeshHandle handle,
                      const glm::mat4& model,
                      float bound);
  // A mesh made ready for undersideOf() (again if it's changed).
  const UndersideMesh& readyMesh(MeshHandle handle);
  void upload(Field& field, glm::ivec2 tile);
  // Where the patch goes this frame (world XY), from the camera.
  static glm::vec2 patchCenterFor(const Camera& camera, float ground);
  void buildPatchMesh();

  IRenderer& m_renderer;
  Registry* m_registry = nullptr;
  AssetStore* m_assets = nullptr;
  RenderModuleHost<RenderContext>* m_host = nullptr;
  MeshLibrary* m_meshes = nullptr; // the frame's
  std::unordered_map<uint64_t, Field> m_fields;
  // Each deformer mesh's world matrix when it last pressed, and whether it
  // rolled there (and this frame's, made as they're looked at).
  struct Placed
  {
    glm::mat4 model{1.0f};
    bool rolled = false;
  };
  std::unordered_map<uint64_t, Placed> m_placed;
  std::unordered_map<uint64_t, Placed> m_stillPlaced;
  float m_frameSeconds = 0.0f; // the frame's (real time)
  // Each deformer mesh made ready to find its underside (by its mesh id),
  // and what from (made again if that changes).
  struct Ready
  {
    std::unique_ptr<const UndersideMesh> mesh;
    const glm::vec3* positions = nullptr;
    std::size_t vertices = 0;
    const uint32_t* indices = nullptr;
    std::size_t corners = 0;
  };
  std::unordered_map<uint32_t, Ready> m_ready;
  // Scratch: a press's underside, all of it along the way it went, a
  // press's share of that, the places along it (texels back), and the
  // footprint.
  Underside m_bottom;
  Underside m_swept;
  Underside m_piece;
  std::vector<glm::ivec2> m_shifts;
  Footprint m_footprint;
  struct Corner
  {
    bool asked = false;
    bool on = false;
    uint32_t triangle = 0;
    float softness = 0.0f;
  };
  std::vector<Corner> m_corners;
  std::vector<float> m_groundHeight;
  std::vector<float> m_groundSoftness;
  MeshHandle m_patchMesh;
  std::vector<uint16_t> m_halves; // scratch: a tile's
  // Scratch: the frame's deformable terrains, and the meshes that may be a
  // deformer's, their own or an ancestor's, in id order.
  std::vector<Seen> m_seen;
  std::vector<Entity> m_mayDeform;
};

} // namespace sfs
