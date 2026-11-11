#include "engine/runtime/systems/physicsSystem3D.h"

#include "engine/runtime/sceneManager/scene.h"

#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/capsuleCollider3DComponent.h"
#include "engine/core/components/contactReportComponent.h"
#include "engine/core/components/parentComponent.h"
#include "engine/core/components/rigidBody3DComponent.h"
#include "engine/core/components/sphereCollider3DComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/cpuTiming.h"
#include "glm/glm/gtc/quaternion.hpp"
#include "glm/glm/vector_relational.hpp"

// Jolt.h must come first.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayerInterfaceTable.h>
#include <Jolt/Physics/Collision/BroadPhase/ObjectVsBroadPhaseLayerFilterTable.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayerPairFilterTable.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

// CollisionWorld3D (frozen worlds for queries off the main thread) lives in
// this unit too: the only one that includes Jolt.
#include "collisionWorld3D.inl"

#include <algorithm>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace sfs
{

namespace
{

constexpr float kStep = 1.0f / 60.0f;
// Steps per update at most, so a long frame (a breakpoint, loading) doesn't
// snowball into ever longer catch-up frames.
constexpr int kMaxStepsPerUpdate = 4;

// With no dynamic bodies there's nothing to simulate, only contacts to find:
// it steps only when something moved, appeared or went, once at least kStep
// of real time has passed (or sooner when something has gone far), in one
// step for all the time since, split so that no moving body goes further in
// one than three quarters of its own width along the way (so it can't pass
// anything unnoticed, however thin), nor kMaxTravel.
constexpr float kWidthPerStep = 0.75f;
constexpr float kMaxTravel = 1.0f;
constexpr float kMinTravel = 0.01f;
constexpr int kMostSubsteps = 16;

// The nearest ContactReportComponent at or above a body's entity, or an
// invalid entity when there is none.
Entity reportOf(Entity entity)
{
  while (entity.isValid() && entity.isAlive())
  {
    if (entity.hasComponent<ContactReportComponent>())
      return entity;
    entity = hierarchy::parentOf(entity);
  }
  return {};
}

// Hands each contact to the reports of both sides, `a` being the report's own
// collider. Contacts are few, so the parents are walked for each one.
void reportContacts(const std::vector<Contact3D>& contacts,
                    std::vector<Contact3D> ContactReportComponent::*list)
{
  for (const Contact3D& contact : contacts)
  {
    const Entity first = reportOf(contact.a);
    const Entity second = reportOf(contact.b);
    if (first)
      (first.getComponent<ContactReportComponent>().*list).push_back(contact);
    if (second && second != first)
    {
      Contact3D swapped = contact;
      std::swap(swapped.a, swapped.b);
      (second.getComponent<ContactReportComponent>().*list).push_back(swapped);
    }
  }
}

// Static bodies only need testing against moving ones.
constexpr JPH::ObjectLayer kStaticLayer = 0;
constexpr JPH::ObjectLayer kMovingLayer = 1;
constexpr JPH::uint kLayerCount = 2;

// --- Jolt setup, once per process --------------------------------------------

void joltTrace(const char* format, ...)
{
  char buffer[1024];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buffer, sizeof(buffer), format, args);
  va_end(args);
  LOG_INFO(std::string("Jolt: ") + buffer);
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssert(const char* expression,
                const char* message,
                const char* file,
                JPH::uint line)
{
  LOG_ERROR(std::string("Jolt assert: ") + expression +
            (message ? std::string(" ") + message : "") + " (" + file + ":" +
            std::to_string(line) + ")");
  return false; // carry on rather than break
}
#endif

void initJolt()
{
  static std::once_flag once;
  std::call_once(once,
                 []
                 {
                   JPH::Trace = joltTrace;
                   JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssert;)
                 });
  collisionWorld::ready(); // (the allocator, the factory and the types: once)
}

// --- conversions
// --------------------------------------------------------------

JPH::Vec3 toJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::Quat toJolt(const glm::quat& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
glm::vec3 toGlm(JPH::Vec3Arg v) { return {v.GetX(), v.GetY(), v.GetZ()}; }
glm::quat toGlm(JPH::QuatArg q)
{
  return glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ());
}

// --- what a body is made of
// -----------------------------------------------------

struct ColliderDesc
{
  enum class Kind
  {
    Box,
    Sphere,
    Capsule,
  };
  Kind kind = Kind::Box;
  glm::vec3 size{0.0f}; // box: full size; sphere: x = radius; capsule: x =
                        // radius, y = height
  glm::vec3 offset{0.0f};
  bool trigger = false;
  int layer = 0;

  bool operator==(const ColliderDesc&) const = default;
};

// Everything a body is built from. A change means rebuilding the body; a move
// (position, rotation) doesn't.
struct BodyDesc
{
  std::vector<ColliderDesc>
      colliders; // already scaled by the entity's world scale
  glm::vec3 scale{1.0f};
  JPH::EMotionType motion = JPH::EMotionType::Static;
  RigidBody3DComponent body;
  bool sensor = false;

  bool operator==(const BodyDesc& other) const
  {
    const RigidBody3DComponent& a = body;
    const RigidBody3DComponent& b = other.body;
    return colliders == other.colliders && scale == other.scale &&
           motion == other.motion && sensor == other.sensor &&
           a.mass == b.mass && a.kinematic == b.kinematic &&
           a.friction == b.friction && a.bounciness == b.bounciness &&
           a.linearDamping == b.linearDamping &&
           a.angularDamping == b.angularDamping &&
           a.gravityScale == b.gravityScale &&
           a.lockRotation == b.lockRotation && a.touchStatic == b.touchStatic;
  }
};

glm::vec3 worldScale(const glm::mat4& world)
{
  return {glm::length(glm::vec3(world[0])),
          glm::length(glm::vec3(world[1])),
          glm::length(glm::vec3(world[2]))};
}

// The components an entity's body is made from (null: it hasn't that one).
// Kept with the body: a component stays where it is until it's removed.
struct Parts
{
  const BoxCollider3DComponent* box = nullptr;
  const SphereCollider3DComponent* sphere = nullptr;
  const CapsuleCollider3DComponent* capsule = nullptr;
  const RigidBody3DComponent* rigid = nullptr;

  static Parts of(const Entity& entity)
  {
    Parts parts;
    if (entity.hasComponent<BoxCollider3DComponent>())
      parts.box = &entity.getComponent<BoxCollider3DComponent>();
    if (entity.hasComponent<SphereCollider3DComponent>())
      parts.sphere = &entity.getComponent<SphereCollider3DComponent>();
    if (entity.hasComponent<CapsuleCollider3DComponent>())
      parts.capsule = &entity.getComponent<CapsuleCollider3DComponent>();
    if (entity.hasComponent<RigidBody3DComponent>())
      parts.rigid = &entity.getComponent<RigidBody3DComponent>();
    return parts;
  }

  bool anyCollider() const { return box || sphere || capsule; }
  bool operator==(const Parts&) const = default;
};

// Calls `fn` with each collider, scaled by `s`, in a fixed order (box,
// sphere, capsule).
template <typename Fn>
void forEachCollider(const Parts& parts, const glm::vec3& s, Fn&& fn)
{
  if (const BoxCollider3DComponent* box = parts.box)
    fn(ColliderDesc{ColliderDesc::Kind::Box,
                    box->size * s,
                    box->offset * s,
                    box->trigger,
                    box->layer});
  if (const SphereCollider3DComponent* sphere = parts.sphere)
    fn(ColliderDesc{ColliderDesc::Kind::Sphere,
                    glm::vec3(sphere->radius * std::max({s.x, s.y, s.z}), 0, 0),
                    sphere->offset * s,
                    sphere->trigger,
                    sphere->layer});
  if (const CapsuleCollider3DComponent* capsule = parts.capsule)
    fn(ColliderDesc{
        ColliderDesc::Kind::Capsule,
        glm::vec3(
            capsule->radius * std::max(s.x, s.y), capsule->height * s.z, 0),
        capsule->offset * s,
        capsule->trigger,
        capsule->layer});
}

bool sameScale(const glm::vec3& a, const glm::vec3& b)
{
  const glm::vec3 tolerance = 1e-4f * glm::max(glm::abs(b), glm::vec3(1.0f));
  return glm::all(glm::lessThanEqual(glm::abs(a - b), tolerance));
}

std::optional<BodyDesc> describe(const Parts& parts, const glm::mat4& world)
{
  BodyDesc desc;
  desc.scale = worldScale(world);
  forEachCollider(parts,
                  desc.scale,
                  [&](const ColliderDesc& collider)
                  { desc.colliders.push_back(collider); });
  if (desc.colliders.empty())
    return std::nullopt;

  // A trigger body detects rather than blocks; a mix of triggers and solid
  // colliders on one entity stays solid.
  desc.sensor = std::all_of(desc.colliders.begin(),
                            desc.colliders.end(),
                            [](const ColliderDesc& c) { return c.trigger; });

  if (parts.rigid)
  {
    desc.body = *parts.rigid;
    desc.motion = desc.body.kinematic ? JPH::EMotionType::Kinematic
                                      : JPH::EMotionType::Dynamic;
  }
  return desc;
}

// Whether describe() would still give `desc` at the same scale: checked every
// update without building anything.
bool matches(const Parts& parts, const BodyDesc& desc)
{
  std::size_t count = 0;
  bool same = true;
  forEachCollider(parts,
                  desc.scale,
                  [&](const ColliderDesc& collider)
                  {
                    same = same && count < desc.colliders.size() &&
                           desc.colliders[count] == collider;
                    ++count;
                  });
  if (!same || count != desc.colliders.size())
    return false;

  if (!parts.rigid)
    return desc.motion == JPH::EMotionType::Static;
  const RigidBody3DComponent& a = desc.body;
  const RigidBody3DComponent& b = *parts.rigid;
  const JPH::EMotionType motion =
      b.kinematic ? JPH::EMotionType::Kinematic : JPH::EMotionType::Dynamic;
  return desc.motion == motion && a.mass == b.mass &&
         a.friction == b.friction && a.bounciness == b.bounciness &&
         a.linearDamping == b.linearDamping &&
         a.angularDamping == b.angularDamping &&
         a.gravityScale == b.gravityScale && a.lockRotation == b.lockRotation &&
         a.touchStatic == b.touchStatic;
}

bool sameTransform(const TransformComponent& a, const TransformComponent& b)
{
  return a.position == b.position && a.rotation == b.rotation &&
         a.scale == b.scale;
}

// Whether two components' bytes are the same, a word at a time (they're
// made of floats and ints, so their sizes are whole words).
bool sameWords(const std::byte* a, const std::byte* b, std::uint32_t size)
{
  for (std::uint32_t i = 0; i < size; i += 4)
  {
    std::uint32_t x, y;
    std::memcpy(&x, a + i, 4);
    std::memcpy(&y, b + i, 4);
    if (x != y)
      return false;
  }
  return true;
}
static_assert(sizeof(TransformComponent) % 4 == 0 &&
              sizeof(ParentComponent) % 4 == 0 &&
              sizeof(BoxCollider3DComponent) % 4 == 0 &&
              sizeof(SphereCollider3DComponent) % 4 == 0 &&
              sizeof(CapsuleCollider3DComponent) % 4 == 0 &&
              sizeof(RigidBody3DComponent) % 4 == 0);

// An entity and its ancestors as its world matrix was last worked out from:
// unchanged, so is the matrix.
struct Chain
{
  struct Link
  {
    const TransformComponent* transform = nullptr; // (none: identity)
    TransformComponent value;
    const ParentComponent* parent = nullptr;
    Entity up; // the parent then
  };
  std::vector<Link> links;

  enum class State
  {
    Same,
    Moved,    // the same entities, some transform changed
    Relinked, // a parent changed
  };

  void cache(const Entity& entity)
  {
    links.clear();
    Entity at = entity;
    for (int depth = 0; at.isValid() && depth < 64; ++depth)
    {
      Link link;
      if (at.hasComponent<TransformComponent>())
      {
        link.transform = &at.getComponent<TransformComponent>();
        link.value = *link.transform;
      }
      if (at.hasComponent<ParentComponent>())
      {
        link.parent = &at.getComponent<ParentComponent>();
        link.up = link.parent->parent;
      }
      links.push_back(link);
      at = hierarchy::parentOf(at);
    }
  }

  State state() const
  {
    State result = State::Same;
    for (const Link& link : links)
    {
      if (link.parent && !(link.parent->parent == link.up))
        return State::Relinked;
      if (link.transform && !sameTransform(*link.transform, link.value))
        result = State::Moved;
    }
    return result;
  }

  // Whether `other` is made of the same components (whatever their values).
  bool sameLinks(const Chain& other) const
  {
    if (links.size() != other.links.size())
      return false;
    for (std::size_t i = 0; i < links.size(); ++i)
      if (links[i].transform != other.links[i].transform ||
          links[i].parent != other.links[i].parent)
        return false;
    return true;
  }

  void refresh()
  {
    for (Link& link : links)
      if (link.transform)
        link.value = *link.transform;
  }
};

JPH::RefConst<JPH::Shape> createShape(const ColliderDesc& collider)
{
  JPH::ShapeSettings::ShapeResult result;
  JPH::Quat rotation = JPH::Quat::sIdentity();
  constexpr float kMin = 0.005f;
  switch (collider.kind)
  {
  case ColliderDesc::Kind::Box:
  {
    const glm::vec3 half = glm::max(collider.size * 0.5f, glm::vec3(kMin));
    const float convexRadius = std::min(
        JPH::cDefaultConvexRadius, std::min({half.x, half.y, half.z}) * 0.5f);
    result = JPH::BoxShapeSettings(toJolt(half), convexRadius).Create();
    break;
  }
  case ColliderDesc::Kind::Sphere:
    result = JPH::SphereShapeSettings(std::max(collider.size.x, kMin)).Create();
    break;
  case ColliderDesc::Kind::Capsule:
  {
    const float radius = std::max(collider.size.x, kMin);
    const float halfCylinder = std::max(collider.size.y * 0.5f - radius, kMin);
    result = JPH::CapsuleShapeSettings(halfCylinder, radius).Create();
    // Jolt's capsules stand along Y; ours along Z.
    rotation = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI);
    break;
  }
  }
  if (result.HasError())
  {
    LOG_ERROR(std::string("PhysicsSystem3D: can't build a collider: ") +
              result.GetError().c_str());
    return nullptr;
  }

  if (collider.offset == glm::vec3(0.0f) && rotation == JPH::Quat::sIdentity())
    return result.Get();
  JPH::ShapeSettings::ShapeResult placed =
      JPH::RotatedTranslatedShapeSettings(
          toJolt(collider.offset), rotation, result.Get())
          .Create();
  return placed.HasError() ? nullptr : placed.Get();
}

JPH::RefConst<JPH::Shape> createShape(const BodyDesc& desc)
{
  if (desc.colliders.size() == 1)
    return createShape(desc.colliders.front());

  JPH::StaticCompoundShapeSettings compound;
  for (const ColliderDesc& collider : desc.colliders)
    if (JPH::RefConst<JPH::Shape> shape = createShape(collider))
      compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), shape);
  JPH::ShapeSettings::ShapeResult result = compound.Create();
  return result.HasError() ? nullptr : result.Get();
}

// Contacts arrive per pair of sub-shapes; a pair of bodies may have several.
// Bodies are keyed by their ids, smaller first.
using BodyPair = std::pair<JPH::uint32, JPH::uint32>;

BodyPair makePair(const JPH::BodyID& a, const JPH::BodyID& b)
{
  const JPH::uint32 x = a.GetIndexAndSequenceNumber();
  const JPH::uint32 y = b.GetIndexAndSequenceNumber();
  return x < y ? BodyPair{x, y} : BodyPair{y, x};
}

// Collects contact changes during a step (on the stepping thread: the job
// system is single-threaded). A contact begins when the shapes touch: Jolt
// also reports speculative contacts (shapes up to its speculative distance,
// 2 cm, apart, not touching), and those only count once they touch.
class ContactCollector : public JPH::ContactListener
{
public:
  struct Change
  {
    BodyPair pair;
    bool trigger = false;
    bool added = false;
    glm::vec3 point{0.0f};
    float depth = 0.0f;
    bool deeper = false; // (not added: went deeper rather than ended)
  };
  // (Deeper by less than this isn't told: Jolt's depths wobble that much.)
  static constexpr float kDeeperStep = 0.001f;
  std::vector<Change> changes;

  void OnContactAdded(const JPH::Body& a,
                      const JPH::Body& b,
                      const JPH::ContactManifold& manifold,
                      JPH::ContactSettings&) override
  {
    touch(a, b, manifold);
  }

  void OnContactPersisted(const JPH::Body& a,
                          const JPH::Body& b,
                          const JPH::ContactManifold& manifold,
                          JPH::ContactSettings&) override
  {
    touch(a, b, manifold);
  }

  void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
  {
    if (touching.erase(keyOf(pair.GetBody1ID(),
                             pair.GetSubShapeID1(),
                             pair.GetBody2ID(),
                             pair.GetSubShapeID2())) > 0)
      changes.push_back(
          {makePair(pair.GetBody1ID(), pair.GetBody2ID()), false, false});
  }

  // A body gone: its contacts with it (ended by the caller).
  void forget(JPH::uint32 body)
  {
    std::erase_if(touching,
                  [&](const auto& entry)
                  {
                    const Key& k = entry.first;
                    return std::get<0>(k) == body || std::get<2>(k) == body;
                  });
  }

private:
  // (Body 1, its sub-shape, body 2, its sub-shape.)
  using Key = std::tuple<JPH::uint32, JPH::uint32, JPH::uint32, JPH::uint32>;
  // (Each touching sub-shape pair's deepest overlap so far.)
  std::map<Key, float> touching;

  static Key keyOf(const JPH::BodyID& a,
                   const JPH::SubShapeID& sa,
                   const JPH::BodyID& b,
                   const JPH::SubShapeID& sb)
  {
    return {a.GetIndexAndSequenceNumber(),
            sa.GetValue(),
            b.GetIndexAndSequenceNumber(),
            sb.GetValue()};
  }

  void touch(const JPH::Body& a,
             const JPH::Body& b,
             const JPH::ContactManifold& manifold)
  {
    const bool trigger = a.IsSensor() || b.IsSensor();
    // (Speculative: not touching yet.)
    if (!trigger && manifold.mPenetrationDepth < 0.0f)
      return;
    const float depth =
        trigger ? 0.0f : std::max(0.0f, manifold.mPenetrationDepth);
    const auto [entry, added] = touching.try_emplace(
        keyOf(
            a.GetID(), manifold.mSubShapeID1, b.GetID(), manifold.mSubShapeID2),
        depth);
    if (!added)
    {
      if (trigger || depth < entry->second + kDeeperStep)
        return;
      entry->second = depth;
      changes.push_back(
          {makePair(a.GetID(), b.GetID()), false, false, {}, depth, true});
      return;
    }
    const JPH::RVec3 at = manifold.GetWorldSpaceContactPointOn1(0);
    changes.push_back({makePair(a.GetID(), b.GetID()),
                       trigger,
                       true,
                       glm::vec3(at.GetX(), at.GetY(), at.GetZ()),
                       depth});
  }
};

} // namespace

// --- the system
// ------------------------------------------------------------------

struct PhysicsSystem3D::Impl
{
  struct Record
  {
    Entity entity;
    JPH::BodyID body;
    BodyDesc desc;
    glm::mat4 lastWorld{1.0f}; // the world matrix last read or written
    // What it's made from, and what lastWorld was worked out from: so an
    // unchanged entity costs a compare.
    Parts parts;
    Chain chain;
    // Kinematic, with no dynamic bodies about: where Jolt has it, whether
    // it's been moved since, and whether it still has its last move's speed.
    glm::mat4 joltWorld{1.0f};
    bool pendingMove = false;
    bool coasting = false;
    // Its shape's half size (body space), and its farthest point's distance.
    glm::vec3 halfExtent{0.0f};
    float reach = 0.0f;
  };

  struct ActivePair
  {
    int count = 0;
    bool trigger = false;
    float depth = 0.0f; // the deepest any of its sub-shape pairs has gone
  };

  // First: Jolt must be set up (its allocator, above all) before any member
  // below is constructed, since they allocate through it.
  struct JoltReady
  {
    JoltReady() { initJolt(); }
  } joltReady;

  JPH::BroadPhaseLayerInterfaceTable broadPhaseLayers{kLayerCount, kLayerCount};
  JPH::ObjectLayerPairFilterTable layerPairs{kLayerCount};
  std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilterTable> layerVsBroadPhase;
  std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
  std::unique_ptr<JPH::JobSystemSingleThreaded> jobs;
  JPH::PhysicsSystem physics;
  ContactCollector contacts;

  // The bodies, packed, and where each entity's is (by id; -1: none).
  std::vector<Record> records;
  std::vector<int> slots;
  std::unordered_map<JPH::uint32, Entity> entityByBody;
  std::map<BodyPair, ActivePair> activePairs;
  std::vector<Contact3D> begun;
  std::vector<Contact3D> ended;
  std::vector<Contact3D> deepened;
  double accumulator = 0.0;
  bool optimised = false;
  uint64_t revision = 1; // (bodies added, removed or rebuilt, statics moved)

  // The registry's component changes as of the last update, and whether any
  // collider was on an entity without a transform when it last looked for
  // new bodies (which gaining one would make a body).
  struct Seen
  {
    uint64_t box = ~0ull, sphere = ~0ull, capsule = ~0ull, rigid = ~0ull,
             transform = ~0ull, parent = ~0ull;
    bool operator==(const Seen&) const = default;
  } seen;
  bool orphans = false;
  bool lookAgain = false; // a body couldn't be made: try again next update

  // Every component the bodies are made from or placed by (their own, and
  // their ancestors' transforms and parent links), each once, with a copy of
  // its bytes as last seen and the bodies it's part of: what the updates
  // compare, all in one go, while no entity has gained or lost one.
  struct Watch
  {
    const std::byte* at = nullptr;
    std::uint32_t size = 0;
    std::uint32_t copy = 0;  // where its copy is in `watchedBytes`
    std::uint32_t first = 0; // its bodies' slots, in `watchers`
    std::uint32_t last = 0;
  };
  std::vector<Watch> watches;
  std::vector<std::byte> watchedBytes;
  std::vector<int> watchers;
  bool watchesStale = true; // bodies or their parts changed: list them again
  std::vector<char> dirty;  // by slot
  Chain scratch;
  std::vector<int> dirtySlots;

  int dynamicCount = 0;
  int pendingCount = 0;   // kinematic bodies moved since the last step
  bool changed = false;   // bodies added, removed or teleported since then
  double sinceStep = 0.0; // game time since the last step (no dynamics)
  UpdateStats stats;

  Impl()
  {
    broadPhaseLayers.MapObjectToBroadPhaseLayer(
        kStaticLayer, JPH::BroadPhaseLayer(kStaticLayer));
    broadPhaseLayers.MapObjectToBroadPhaseLayer(
        kMovingLayer, JPH::BroadPhaseLayer(kMovingLayer));
    layerPairs.EnableCollision(kMovingLayer, kStaticLayer);
    layerPairs.EnableCollision(kMovingLayer, kMovingLayer);
    layerVsBroadPhase =
        std::make_unique<JPH::ObjectVsBroadPhaseLayerFilterTable>(
            broadPhaseLayers, kLayerCount, layerPairs, kLayerCount);
    tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(16 * 1024 * 1024);
    jobs = std::make_unique<JPH::JobSystemSingleThreaded>(JPH::cMaxPhysicsJobs);

    physics.Init(/*maxBodies*/ 65536,
                 /*bodyMutexes*/ 0,
                 /*maxBodyPairs*/ 65536,
                 /*maxContactConstraints*/ 16384,
                 broadPhaseLayers,
                 *layerVsBroadPhase,
                 layerPairs);
    physics.SetGravity(JPH::Vec3(0.0f, 0.0f, -9.81f));
    physics.SetContactListener(&contacts);
  }

  ~Impl()
  {
    for (const Record& record : records)
      destroyBody(record.body);
  }

  JPH::BodyInterface& bodies() { return physics.GetBodyInterface(); }
  const JPH::BodyInterface& bodies() const
  {
    return physics.GetBodyInterface();
  }

  void destroyBody(const JPH::BodyID& body)
  {
    bodies().RemoveBody(body);
    bodies().DestroyBody(body);
  }

  int slotOf(Entity::EntityId id) const
  {
    return id < slots.size() ? slots[id] : -1;
  }

  const Record* find(const Entity& entity) const
  {
    const int slot = slotOf(entity.getId());
    return slot >= 0 && records[slot].entity == entity ? &records[slot]
                                                       : nullptr;
  }

  // A dynamic body's id, if the entity has one.
  std::optional<JPH::BodyID> dynamicBody(const Entity& entity) const
  {
    const Record* record = find(entity);
    if (!record || record->desc.motion != JPH::EMotionType::Dynamic)
      return std::nullopt;
    return record->body;
  }

  // Makes the entity's body and adds its record; false if it can't.
  bool addRecord(const Entity& entity,
                 const Parts& parts,
                 BodyDesc desc,
                 const glm::mat4& world)
  {
    JPH::RefConst<JPH::Shape> shape = createShape(desc);
    if (!shape)
    {
      lookAgain = true;
      return false;
    }

    const TransformComponent pose = TransformComponent::fromMatrix(world);
    const bool moving = desc.motion != JPH::EMotionType::Static;
    JPH::BodyCreationSettings settings(shape,
                                       toJolt(pose.position),
                                       toJolt(pose.rotation),
                                       desc.motion,
                                       moving ? kMovingLayer : kStaticLayer);
    const RigidBody3DComponent& body = desc.body;
    settings.mIsSensor = desc.sensor;
    settings.mFriction = body.friction;
    settings.mRestitution = body.bounciness;
    settings.mLinearDamping = body.linearDamping;
    settings.mAngularDamping = body.angularDamping;
    settings.mGravityFactor = body.gravityScale;
    if (desc.motion == JPH::EMotionType::Dynamic)
    {
      settings.mOverrideMassProperties =
          JPH::EOverrideMassProperties::CalculateInertia;
      settings.mMassPropertiesOverride.mMass = std::max(body.mass, 0.001f);
      if (body.lockRotation)
        settings.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX |
                                JPH::EAllowedDOFs::TranslationY |
                                JPH::EAllowedDOFs::TranslationZ;
    }
    // A kinematic trigger (e.g. a moving pickup zone) should notice static
    // bodies too; so should a kinematic body that asks to (touchStatic).
    settings.mCollideKinematicVsNonDynamic = desc.sensor || body.touchStatic;

    const JPH::BodyID id = bodies().CreateAndAddBody(
        settings,
        moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (id.IsInvalid())
    {
      LOG_ERROR("PhysicsSystem3D: out of bodies");
      lookAgain = true;
      return false;
    }

    Record record;
    record.entity = entity;
    record.body = id;
    record.desc = std::move(desc);
    record.lastWorld = world;
    record.joltWorld = world;
    record.parts = parts;
    record.chain.cache(entity);
    const JPH::AABox bounds = shape->GetLocalBounds();
    record.halfExtent = toGlm(bounds.GetExtent());
    record.reach = glm::length(
        glm::max(toGlm(bounds.mMin.Abs()), toGlm(bounds.mMax.Abs())));
    if (record.desc.motion == JPH::EMotionType::Dynamic)
      ++dynamicCount;

    const Entity::EntityId key = entity.getId();
    if (key >= slots.size())
      slots.resize(key + 1, -1);
    slots[key] = static_cast<int>(records.size());
    records.push_back(std::move(record));
    watchesStale = true;
    entityByBody[id.GetIndexAndSequenceNumber()] = entity;
    ++revision;
    changed = true;
    return true;
  }

  // Removes the body at `slot`; the last record takes its place.
  void removeAt(int slot)
  {
    Record& record = records[slot];
    const JPH::uint32 key = record.body.GetIndexAndSequenceNumber();
    // Its contacts end with it.
    contacts.forget(key);
    for (auto pair = activePairs.begin(); pair != activePairs.end();)
      if (pair->first.first == key || pair->first.second == key)
      {
        reportEnded(pair->first, pair->second);
        pair = activePairs.erase(pair);
      }
      else
        ++pair;
    destroyBody(record.body);
    entityByBody.erase(key);
    if (record.desc.motion == JPH::EMotionType::Dynamic)
      --dynamicCount;
    if (record.pendingMove)
      --pendingCount;
    slots[record.entity.getId()] = -1;
    if (slot + 1 != static_cast<int>(records.size()))
    {
      record = std::move(records.back());
      slots[record.entity.getId()] = slot;
    }
    records.pop_back();
    ++revision;
    changed = true;
    watchesStale = true;
  }

  Entity entityOf(JPH::uint32 body) const
  {
    const auto it = entityByBody.find(body);
    return it == entityByBody.end() ? Entity() : it->second;
  }

  void reportEnded(const BodyPair& pair, const ActivePair& active)
  {
    ended.push_back({entityOf(pair.first),
                     entityOf(pair.second),
                     active.trigger,
                     glm::vec3(0.0f),
                     active.depth});
  }

  // Bring the bodies in line with the components: new, removed, rebuilt and
  // moved-by-code bodies. Each body is checked every update: while no entity
  // has gained or lost the components a body is made from (or a transform or
  // parent), through what it found last time, a compare when nothing's
  // changed. The scene is searched for new bodies only when an entity has
  // gained or lost a collider (or, if it matters, a transform).
  void sync(Registry& registry, float frameTime)
  {
    stats.lookedForBodies = false;
    const bool fixedSteps = dynamicCount > 0;
    const Seen now{registry.componentChanges<BoxCollider3DComponent>(),
                   registry.componentChanges<SphereCollider3DComponent>(),
                   registry.componentChanges<CapsuleCollider3DComponent>(),
                   registry.componentChanges<RigidBody3DComponent>(),
                   registry.componentChanges<TransformComponent>(),
                   registry.componentChanges<ParentComponent>()};
    const bool settled = now == seen;

    // Backwards: a removed body's place is taken by one already checked.
    if (!settled || watchesStale)
    {
      for (int slot = static_cast<int>(records.size()) - 1; slot >= 0; --slot)
        check(slot, frameTime, fixedSteps, settled);
    }
    else
    {
      // Just the bodies something of theirs changed for.
      findChanged();
      for (const int slot : dirtySlots)
        check(slot, frameTime, fixedSteps, settled);
    }

    const bool look = now.box != seen.box || now.sphere != seen.sphere ||
                      now.capsule != seen.capsule ||
                      (orphans && now.transform != seen.transform) || lookAgain;
    seen = now;
    if (look)
    {
      lookAgain = false;
      lookForBodies(registry);
    }
    if (watchesStale)
      listWatches();
  }

  // The slots of the bodies a watched component changed for, last first.
  void findChanged()
  {
    dirty.assign(records.size(), 0);
    dirtySlots.clear();
    for (const Watch& watch : watches)
    {
      std::byte* copy = watchedBytes.data() + watch.copy;
      if (sameWords(watch.at, copy, watch.size))
        continue;
      std::memcpy(copy, watch.at, watch.size);
      for (std::uint32_t i = watch.first; i < watch.last; ++i)
        if (!dirty[watchers[i]])
        {
          dirty[watchers[i]] = 1;
          dirtySlots.push_back(watchers[i]);
        }
    }
    std::sort(dirtySlots.begin(), dirtySlots.end(), std::greater<int>());
  }

  void listWatches()
  {
    // In memory order (a component type's are stored by entity id), so the
    // compares run through memory rather than jump about it.
    struct Entry
    {
      const std::byte* at;
      std::size_t size;
      int slot;
    };
    std::vector<Entry> entries;
    const auto watch = [&](const void* at, std::size_t size, int slot)
    {
      if (at)
        entries.push_back({static_cast<const std::byte*>(at), size, slot});
    };
    for (int slot = 0; slot < static_cast<int>(records.size()); ++slot)
    {
      const Record& record = records[slot];
      for (const Chain::Link& link : record.chain.links)
      {
        watch(link.transform, sizeof(TransformComponent), slot);
        watch(link.parent, sizeof(ParentComponent), slot);
      }
      watch(record.parts.box, sizeof(BoxCollider3DComponent), slot);
      watch(record.parts.sphere, sizeof(SphereCollider3DComponent), slot);
      watch(record.parts.capsule, sizeof(CapsuleCollider3DComponent), slot);
      watch(record.parts.rigid, sizeof(RigidBody3DComponent), slot);
    }
    std::sort(entries.begin(),
              entries.end(),
              [](const Entry& a, const Entry& b)
              { return std::less<const std::byte*>()(a.at, b.at); });

    // One watch a component, however many bodies it's part of.
    watches.clear();
    watchedBytes.clear();
    watchers.clear();
    for (const Entry& entry : entries)
    {
      if (watches.empty() || watches.back().at != entry.at)
      {
        Watch added;
        added.at = entry.at;
        added.size = static_cast<std::uint32_t>(entry.size);
        added.copy = static_cast<std::uint32_t>(watchedBytes.size());
        added.first = static_cast<std::uint32_t>(watchers.size());
        watchedBytes.insert(
            watchedBytes.end(), entry.at, entry.at + entry.size);
        watches.push_back(added);
      }
      watchers.push_back(entry.slot);
      watches.back().last = static_cast<std::uint32_t>(watchers.size());
    }
    watchesStale = false;
  }

  // Whether the body at `slot` still matches its entity: removes, rebuilds
  // or moves it if not. `settled`: no entity has gained or lost a component
  // the body's made from since the last check.
  void check(int slot, float frameTime, bool fixedSteps, bool settled)
  {
    Record& record = records[slot];
    const Entity entity = record.entity;
    if (!settled)
    {
      if (!entity.isAlive() || !entity.hasComponent<TransformComponent>())
      {
        removeAt(slot);
        return;
      }
      const Parts parts = Parts::of(entity);
      if (!parts.anyCollider())
      {
        removeAt(slot);
        return;
      }
      // (A component stays put while the entity has it: its parts and
      // ancestors are watched where they were unless some came or went.)
      scratch.cache(entity);
      if (!(parts == record.parts) || !scratch.sameLinks(record.chain))
        watchesStale = true;
      record.parts = parts;
      std::swap(record.chain.links, scratch.links);
    }

    glm::mat4 world = record.lastWorld;
    switch (settled ? record.chain.state() : Chain::State::Moved)
    {
    case Chain::State::Same:
      break;
    case Chain::State::Moved:
      record.chain.refresh();
      world = hierarchy::worldMatrix(entity);
      break;
    case Chain::State::Relinked:
      record.chain.cache(entity);
      world = hierarchy::worldMatrix(entity);
      watchesStale = true;
      break;
    }
    const bool moved = world != record.lastWorld;

    // (A turned matrix's scale comes out a little different each time: only
    // a real change of scale rebuilds the body.)
    if (!matches(record.parts, record.desc) ||
        (moved && !sameScale(worldScale(world), record.desc.scale)))
    {
      std::optional<BodyDesc> desc = describe(record.parts, world);
      if (!desc || !(*desc == record.desc))
      {
        const Parts parts = record.parts;
        removeAt(slot); // a different body now: made again
        if (desc)
          addRecord(entity, parts, std::move(*desc), world);
        return;
      }
    }
    if (moved || (fixedSteps && record.pendingMove))
      move(record, world, frameTime, fixedSteps);
  }

  // Moved by code since the last update.
  void
  move(Record& record, const glm::mat4& world, float frameTime, bool fixedSteps)
  {
    const TransformComponent pose = TransformComponent::fromMatrix(world);
    switch (record.desc.motion)
    {
    case JPH::EMotionType::Static:
      bodies().SetPositionAndRotation(record.body,
                                      toJolt(pose.position),
                                      toJolt(pose.rotation),
                                      JPH::EActivation::DontActivate);
      ++revision;
      changed = true;
      break;
    case JPH::EMotionType::Kinematic:
      if (fixedSteps)
      {
        // Moves through the world (pushing what's in the way) over this
        // frame.
        bodies().MoveKinematic(record.body,
                               toJolt(pose.position),
                               toJolt(pose.rotation),
                               std::max(frameTime, kStep));
        record.joltWorld = world;
        record.coasting = true;
        if (record.pendingMove)
        {
          record.pendingMove = false;
          --pendingCount;
        }
      }
      else if (!record.pendingMove)
      {
        // Moved there at the next step.
        record.pendingMove = true;
        ++pendingCount;
      }
      break;
    case JPH::EMotionType::Dynamic:
      bodies().SetPositionAndRotation(record.body,
                                      toJolt(pose.position),
                                      toJolt(pose.rotation),
                                      JPH::EActivation::Activate);
      changed = true;
      break;
    }
    record.lastWorld = world;
  }

  // New bodies: entities with a transform and a collider but no body yet.
  void lookForBodies(Registry& registry)
  {
    stats.lookedForBodies = true;
    orphans = false;
    bool created = false;
    const auto visit = [&](const Entity& entity)
    {
      if (find(entity))
        return;
      if (!entity.hasComponent<TransformComponent>())
      {
        orphans = true;
        return;
      }
      const glm::mat4 world = hierarchy::worldMatrix(entity);
      const Parts parts = Parts::of(entity);
      if (std::optional<BodyDesc> desc = describe(parts, world))
        created = addRecord(entity, parts, std::move(*desc), world) || created;
    };
    for (const Entity& entity : registry.query<BoxCollider3DComponent>())
      visit(entity);
    for (const Entity& entity : registry.query<SphereCollider3DComponent>())
      visit(entity);
    for (const Entity& entity : registry.query<CapsuleCollider3DComponent>())
      visit(entity);

    // A big batch (a scene's first update) makes the broad phase lopsided.
    if (created && !optimised)
    {
      physics.OptimizeBroadPhase();
      optimised = true;
    }
  }

  void step(float seconds)
  {
    {
      SFS_CPU_SCOPE("physics3D/step");
      physics.Update(seconds, 1, tempAllocator.get(), jobs.get());
    }
    collectContacts();
    ++stats.steps;
  }

  // With dynamic bodies: fixed steps of kStep, and their poses read back.
  void stepFixed(double deltaTime)
  {
    sinceStep = 0.0;
    changed = false;
    accumulator += deltaTime;
    int steps = 0;
    while (accumulator >= kStep && steps < kMaxStepsPerUpdate)
    {
      step(kStep);
      accumulator -= kStep;
      ++steps;
    }
    if (steps == kMaxStepsPerUpdate)
      accumulator = 0.0; // fell behind: drop the rest rather than catch up

    if (steps > 0)
      writeBack();
  }

  // How many steps the kinematic bodies' moves since the last one need
  // (none: not yet).
  int stepsNeeded() const
  {
    float most = 0.0f; // the furthest any went, in steps' worth
    for (const Record& record : records)
    {
      if (!record.pendingMove)
        continue;
      const TransformComponent from =
          TransformComponent::fromMatrix(record.joltWorld);
      const TransformComponent to =
          TransformComponent::fromMatrix(record.lastWorld);
      const glm::vec3 offset = to.position - from.position;
      const float distance = glm::length(offset);
      // Its width along the way: how far it may go between looks.
      float along = 0.0f;
      if (distance > 0.0f)
      {
        const glm::vec3 way =
            glm::abs(glm::inverse(to.rotation) * (offset / distance));
        along = 2.0f * kWidthPerStep * glm::dot(way, record.halfExtent);
      }
      const float narrowest =
          2.0f * kWidthPerStep *
          std::min(
              {record.halfExtent.x, record.halfExtent.y, record.halfExtent.z});
      const float turned =
          2.0f * std::acos(std::min(
                     std::abs(glm::dot(from.rotation, to.rotation)), 1.0f));
      const float steps =
          distance / std::clamp(along, kMinTravel, kMaxTravel) +
          turned * record.reach / std::clamp(narrowest, kMinTravel, kMaxTravel);
      most = std::max(most, steps);
    }
    // (Just past a whole number of steps' worth: that many.)
    return most >= 1.0f ? static_cast<int>(std::ceil(most - 0.01f)) : 0;
  }

  // With no dynamic bodies: only when something has moved, appeared or
  // gone, in as few steps as finding every contact takes.
  void stepKinematic(double deltaTime, double timeScale)
  {
    accumulator = 0.0;
    if (pendingCount == 0 && !changed)
    {
      sinceStep = 0.0;
      return;
    }
    sinceStep += deltaTime;
    if (sinceStep <= 0.0)
      return; // (paused)
    const int needed = pendingCount > 0 ? stepsNeeded() : 0;
    if (!changed && needed == 0 &&
        sinceStep < kStep * std::max(timeScale, 1.0) * 0.999)
      return; // not far enough yet

    const float seconds = static_cast<float>(sinceStep);
    for (Record& record : records)
    {
      if (record.desc.motion != JPH::EMotionType::Kinematic)
        continue;
      if (record.pendingMove)
      {
        const TransformComponent pose =
            TransformComponent::fromMatrix(record.lastWorld);
        bodies().MoveKinematic(
            record.body, toJolt(pose.position), toJolt(pose.rotation), seconds);
        record.joltWorld = record.lastWorld;
        record.pendingMove = false;
        record.coasting = true;
      }
      else if (record.coasting)
      {
        // Stopped: it mustn't carry on at its last move's speed.
        bodies().SetLinearAndAngularVelocity(
            record.body, JPH::Vec3::sZero(), JPH::Vec3::sZero());
        record.coasting = false;
      }
    }
    pendingCount = 0;
    changed = false;
    sinceStep = 0.0;

    const int steps = std::clamp(needed, 1, kMostSubsteps);
    for (int i = 0; i < steps; ++i)
      step(seconds / static_cast<float>(steps));
  }

  // Dynamic bodies' new poses into their transforms.
  void writeBack()
  {
    for (Record& record : records)
    {
      if (record.desc.motion != JPH::EMotionType::Dynamic ||
          !bodies().IsActive(record.body))
        continue;
      JPH::RVec3 position;
      JPH::Quat rotation;
      bodies().GetPositionAndRotation(record.body, position, rotation);
      const glm::mat4 world =
          TransformComponent(
              toGlm(position), toGlm(rotation), record.desc.scale)
              .matrix();
      hierarchy::setWorldMatrix(record.entity, world);
      // As it reads back, so it doesn't look moved-by-code next time.
      record.lastWorld = hierarchy::worldMatrix(record.entity);
      record.chain.refresh();
    }
  }

  void collectContacts()
  {
    SFS_CPU_SCOPE("physics3D/contacts");
    // (Pairs that went deeper this update, and where each is in `deepened`.)
    std::map<BodyPair, size_t> deeper;
    const auto deepen =
        [&](const BodyPair& pair, ActivePair& active, float depth)
    {
      if (active.trigger || depth <= active.depth)
        return;
      active.depth = depth;
      const auto [at, first] = deeper.try_emplace(pair, deepened.size());
      if (first)
        deepened.push_back({entityOf(pair.first),
                            entityOf(pair.second),
                            false,
                            glm::vec3(0.0f),
                            depth});
      else
        deepened[at->second].depth = depth;
    };
    for (const ContactCollector::Change& change : contacts.changes)
    {
      if (change.added)
      {
        ActivePair& active = activePairs[change.pair];
        if (active.count++ == 0)
        {
          active.trigger = change.trigger;
          active.depth = change.depth;
          begun.push_back({entityOf(change.pair.first),
                           entityOf(change.pair.second),
                           change.trigger,
                           change.point,
                           change.depth});
        }
        else
          deepen(change.pair, active, change.depth);
        continue;
      }
      const auto it = activePairs.find(change.pair);
      if (it == activePairs.end())
        continue;
      if (change.deeper)
      {
        deepen(change.pair, it->second, change.depth);
        continue;
      }
      if (--it->second.count == 0)
      {
        reportEnded(change.pair, it->second);
        activePairs.erase(it);
      }
    }
    contacts.changes.clear();
  }
};

PhysicsSystem3D::PhysicsSystem3D(Scene* scene)
    : m(std::make_unique<Impl>()), m_scene(scene)
{
}

PhysicsSystem3D::~PhysicsSystem3D() = default;

void PhysicsSystem3D::setGravity(const glm::vec3& gravity)
{
  m->physics.SetGravity(toJolt(gravity));
}

glm::vec3 PhysicsSystem3D::gravity() const
{
  return toGlm(m->physics.GetGravity());
}

void PhysicsSystem3D::update(double deltaTime)
{
  m->begun.clear();
  m->ended.clear();
  m->deepened.clear();
  m->stats.steps = 0;
  if (!registry)
    return;
  const std::vector<Entity>& reports =
      registry->query<ContactReportComponent>();
  for (const Entity& entity : reports)
  {
    auto& report = entity.getComponent<ContactReportComponent>();
    report.begun.clear();
    report.ended.clear();
    report.deepened.clear();
  }

  {
    SFS_CPU_SCOPE("physics3D/sync");
    m->sync(*registry, static_cast<float>(deltaTime));
  }
  if (m->dynamicCount > 0)
  {
    // (Kinematic bodies moved before there were dynamic ones.)
    if (m->pendingCount > 0)
      for (Impl::Record& record : m->records)
        if (record.pendingMove)
          m->move(record,
                  record.lastWorld,
                  static_cast<float>(deltaTime),
                  /*fixedSteps*/ true);
    m->stepFixed(deltaTime);
  }
  else
    m->stepKinematic(deltaTime, registry->timeScale());

  if (!reports.empty())
  {
    reportContacts(m->begun, &ContactReportComponent::begun);
    reportContacts(m->ended, &ContactReportComponent::ended);
    reportContacts(m->deepened, &ContactReportComponent::deepened);
  }

  if (!m_scene)
    return;
  // Copied: a handler may destroy entities or switch scenes.
  const std::vector<Contact3D> begun = m->begun;
  const std::vector<Contact3D> ended = m->ended;
  for (const Contact3D& contact : begun)
    m_scene->notifyContact(contact, true);
  for (const Contact3D& contact : ended)
    m_scene->notifyContact(contact, false);
}

std::optional<RaycastHit3D> PhysicsSystem3D::raycast(const Ray& ray,
                                                     float maxDistance) const
{
  class SolidOnly : public JPH::BodyFilter
  {
  public:
    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
      return !body.IsSensor();
    }
  };

  const JPH::RRayCast cast{
      toJolt(ray.origin), toJolt(ray.direction * maxDistance)};
  JPH::RayCastResult hit;
  if (!m->physics.GetNarrowPhaseQuery().CastRay(cast, hit, {}, {}, SolidOnly()))
    return std::nullopt;

  RaycastHit3D result;
  const JPH::RVec3 point = cast.GetPointOnRay(hit.mFraction);
  result.entity = m->entityOf(hit.mBodyID.GetIndexAndSequenceNumber());
  result.point = toGlm(point);
  result.distance = hit.mFraction * maxDistance;
  JPH::BodyLockRead lock(m->physics.GetBodyLockInterface(), hit.mBodyID);
  if (lock.Succeeded())
    result.normal = toGlm(
        lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, point));
  return result;
}

glm::vec3 PhysicsSystem3D::velocity(const Entity& entity) const
{
  const auto body = m->dynamicBody(entity);
  return body ? toGlm(m->bodies().GetLinearVelocity(*body)) : glm::vec3(0.0f);
}

void PhysicsSystem3D::setVelocity(const Entity& entity,
                                  const glm::vec3& velocity)
{
  if (const auto body = m->dynamicBody(entity))
    m->bodies().SetLinearVelocity(*body, toJolt(velocity));
}

glm::vec3 PhysicsSystem3D::angularVelocity(const Entity& entity) const
{
  const auto body = m->dynamicBody(entity);
  return body ? toGlm(m->bodies().GetAngularVelocity(*body)) : glm::vec3(0.0f);
}

void PhysicsSystem3D::setAngularVelocity(const Entity& entity,
                                         const glm::vec3& radiansPerSecond)
{
  if (const auto body = m->dynamicBody(entity))
    m->bodies().SetAngularVelocity(*body, toJolt(radiansPerSecond));
}

void PhysicsSystem3D::addImpulse(const Entity& entity, const glm::vec3& impulse)
{
  if (const auto body = m->dynamicBody(entity))
    m->bodies().AddImpulse(*body, toJolt(impulse));
}

void PhysicsSystem3D::addForce(const Entity& entity, const glm::vec3& force)
{
  if (const auto body = m->dynamicBody(entity))
    m->bodies().AddForce(*body, toJolt(force));
}

const PhysicsSystem3D::UpdateStats& PhysicsSystem3D::lastUpdateStats() const
{
  return m->stats;
}

bool PhysicsSystem3D::hasBody(const Entity& entity) const
{
  return m->find(entity) != nullptr;
}

uint64_t PhysicsSystem3D::revision() const { return m->revision; }

std::shared_ptr<const CollisionWorld3D> PhysicsSystem3D::snapshot(
    const std::function<bool(const Entity&)>& include) const
{
  std::vector<CollisionWorld3D::Body> bodies;
  bodies.reserve(m->records.size());
  for (const Impl::Record& record : m->records)
  {
    if (record.desc.sensor || record.desc.motion == JPH::EMotionType::Dynamic ||
        (include && !include(record.entity)))
      continue;
    CollisionWorld3D::Body body;
    body.id = record.entity.getId();
    body.layer = 31;
    const TransformComponent pose =
        TransformComponent::fromMatrix(record.lastWorld);
    body.position = pose.position;
    body.rotation = pose.rotation;
    for (const ColliderDesc& collider : record.desc.colliders)
    {
      if (collider.trigger)
        continue;
      body.layer = std::min(
          body.layer, static_cast<uint32_t>(std::clamp(collider.layer, 0, 31)));
      CollisionShape3D shape;
      shape.offset = collider.offset;
      shape.size = collider.size;
      switch (collider.kind)
      {
      case ColliderDesc::Kind::Box:
        shape.kind = CollisionShape3D::Kind::Box;
        break;
      case ColliderDesc::Kind::Sphere:
        shape.kind = CollisionShape3D::Kind::Sphere;
        break;
      case ColliderDesc::Kind::Capsule:
        shape.kind = CollisionShape3D::Kind::Capsule;
        break;
      }
      body.shapes.push_back(shape);
    }
    if (!body.shapes.empty())
      bodies.push_back(std::move(body));
  }
  return std::make_shared<const CollisionWorld3D>(bodies);
}

const std::vector<Contact3D>& PhysicsSystem3D::contactsBegun() const
{
  return m->begun;
}

const std::vector<Contact3D>& PhysicsSystem3D::contactsEnded() const
{
  return m->ended;
}

const std::vector<Contact3D>& PhysicsSystem3D::contactsDeepened() const
{
  return m->deepened;
}

} // namespace sfs
