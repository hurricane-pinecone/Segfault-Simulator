#include "engine/runtime/systems/physicsSystem3D.h"

#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/capsuleCollider3DComponent.h"
#include "engine/core/components/rigidBody3DComponent.h"
#include "engine/core/components/sphereCollider3DComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/ecs/registry.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/logger/logger.h"
#include "glm/glm/gtc/quaternion.hpp"

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

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace sfs
{

namespace
{

constexpr float kStep = 1.0f / 60.0f;
// Steps per update at most, so a long frame (a breakpoint, loading) doesn't
// snowball into ever longer catch-up frames.
constexpr int kMaxStepsPerUpdate = 4;

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
                   JPH::RegisterDefaultAllocator();
                   JPH::Trace = joltTrace;
                   JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = joltAssert;)
                   JPH::Factory::sInstance = new JPH::Factory();
                   JPH::RegisterTypes();
                 });
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
           a.gravityScale == b.gravityScale && a.lockRotation == b.lockRotation;
  }
};

glm::vec3 worldScale(const glm::mat4& world)
{
  return {glm::length(glm::vec3(world[0])),
          glm::length(glm::vec3(world[1])),
          glm::length(glm::vec3(world[2]))};
}

std::optional<BodyDesc> describe(const Entity& entity, const glm::mat4& world)
{
  BodyDesc desc;
  desc.scale = worldScale(world);
  const glm::vec3& s = desc.scale;
  const float largest = std::max({s.x, s.y, s.z});

  if (entity.hasComponent<BoxCollider3DComponent>())
  {
    const auto& box = entity.getComponent<BoxCollider3DComponent>();
    desc.colliders.push_back(
        {ColliderDesc::Kind::Box, box.size * s, box.offset * s, box.trigger});
  }
  if (entity.hasComponent<SphereCollider3DComponent>())
  {
    const auto& sphere = entity.getComponent<SphereCollider3DComponent>();
    desc.colliders.push_back({ColliderDesc::Kind::Sphere,
                              glm::vec3(sphere.radius * largest, 0, 0),
                              sphere.offset * s,
                              sphere.trigger});
  }
  if (entity.hasComponent<CapsuleCollider3DComponent>())
  {
    const auto& capsule = entity.getComponent<CapsuleCollider3DComponent>();
    desc.colliders.push_back(
        {ColliderDesc::Kind::Capsule,
         glm::vec3(
             capsule.radius * std::max(s.x, s.y), capsule.height * s.z, 0),
         capsule.offset * s,
         capsule.trigger});
  }
  if (desc.colliders.empty())
    return std::nullopt;

  // A trigger body detects rather than blocks; a mix of triggers and solid
  // colliders on one entity stays solid.
  desc.sensor = std::all_of(desc.colliders.begin(),
                            desc.colliders.end(),
                            [](const ColliderDesc& c) { return c.trigger; });

  if (entity.hasComponent<RigidBody3DComponent>())
  {
    desc.body = entity.getComponent<RigidBody3DComponent>();
    desc.motion = desc.body.kinematic ? JPH::EMotionType::Kinematic
                                      : JPH::EMotionType::Dynamic;
  }
  return desc;
}

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
// system is single-threaded).
class ContactCollector : public JPH::ContactListener
{
public:
  struct Change
  {
    BodyPair pair;
    bool trigger = false;
    bool added = false;
  };
  std::vector<Change> changes;

  void OnContactAdded(const JPH::Body& a,
                      const JPH::Body& b,
                      const JPH::ContactManifold&,
                      JPH::ContactSettings&) override
  {
    changes.push_back(
        {makePair(a.GetID(), b.GetID()), a.IsSensor() || b.IsSensor(), true});
  }

  void OnContactRemoved(const JPH::SubShapeIDPair& pair) override
  {
    changes.push_back(
        {makePair(pair.GetBody1ID(), pair.GetBody2ID()), false, false});
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
  };

  struct ActivePair
  {
    int count = 0;
    bool trigger = false;
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

  std::unordered_map<Entity::EntityId, Record> records;
  std::unordered_map<JPH::uint32, Entity> entityByBody;
  std::map<BodyPair, ActivePair> activePairs;
  std::vector<Contact3D> begun;
  std::vector<Contact3D> ended;
  double accumulator = 0.0;
  bool optimised = false;

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
    for (const auto& [id, record] : records)
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

  const Record* find(const Entity& entity) const
  {
    const auto it = records.find(entity.getId());
    return it != records.end() && it->second.entity == entity ? &it->second
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

  bool createBody(Record& record, const glm::mat4& world)
  {
    JPH::RefConst<JPH::Shape> shape = createShape(record.desc);
    if (!shape)
      return false;

    const TransformComponent pose = TransformComponent::fromMatrix(world);
    const bool moving = record.desc.motion != JPH::EMotionType::Static;
    JPH::BodyCreationSettings settings(shape,
                                       toJolt(pose.position),
                                       toJolt(pose.rotation),
                                       record.desc.motion,
                                       moving ? kMovingLayer : kStaticLayer);
    const RigidBody3DComponent& body = record.desc.body;
    settings.mIsSensor = record.desc.sensor;
    settings.mFriction = body.friction;
    settings.mRestitution = body.bounciness;
    settings.mLinearDamping = body.linearDamping;
    settings.mAngularDamping = body.angularDamping;
    settings.mGravityFactor = body.gravityScale;
    if (record.desc.motion == JPH::EMotionType::Dynamic)
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
    // bodies too.
    settings.mCollideKinematicVsNonDynamic = record.desc.sensor;

    record.body = bodies().CreateAndAddBody(
        settings,
        moving ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
    if (record.body.IsInvalid())
    {
      LOG_ERROR("PhysicsSystem3D: out of bodies");
      return false;
    }
    record.lastWorld = world;
    entityByBody[record.body.GetIndexAndSequenceNumber()] = record.entity;
    return true;
  }

  void removeRecord(std::unordered_map<Entity::EntityId, Record>::iterator it)
  {
    const JPH::uint32 key = it->second.body.GetIndexAndSequenceNumber();
    // Its contacts end with it.
    for (auto pair = activePairs.begin(); pair != activePairs.end();)
      if (pair->first.first == key || pair->first.second == key)
      {
        reportEnded(pair->first, pair->second.trigger);
        pair = activePairs.erase(pair);
      }
      else
        ++pair;
    destroyBody(it->second.body);
    entityByBody.erase(key);
    records.erase(it);
  }

  Entity entityOf(JPH::uint32 body) const
  {
    const auto it = entityByBody.find(body);
    return it == entityByBody.end() ? Entity() : it->second;
  }

  void reportEnded(const BodyPair& pair, bool trigger)
  {
    ended.push_back({entityOf(pair.first), entityOf(pair.second), trigger});
  }

  // Bring the bodies in line with the components: new, removed, rebuilt and
  // moved-by-code bodies.
  void sync(Registry& registry, float frameTime)
  {
    std::unordered_set<Entity::EntityId> seen;
    bool created = false;

    const auto visit = [&](const Entity& entity)
    {
      if (!seen.insert(entity.getId()).second)
        return;
      const glm::mat4 world = hierarchy::worldMatrix(entity);
      std::optional<BodyDesc> desc = describe(entity, world);
      if (!desc)
        return;

      auto it = records.find(entity.getId());
      if (it != records.end() &&
          (it->second.entity != entity || !(it->second.desc == *desc)))
      {
        removeRecord(it); // a different entity now, or rebuilt below
        it = records.end();
      }

      if (it == records.end())
      {
        Record record{entity, JPH::BodyID(), std::move(*desc), world};
        if (createBody(record, world))
        {
          records.emplace(entity.getId(), std::move(record));
          created = true;
        }
        return;
      }

      Record& record = it->second;
      if (world == record.lastWorld)
        return;

      // Moved by code since the last update.
      const TransformComponent pose = TransformComponent::fromMatrix(world);
      switch (record.desc.motion)
      {
      case JPH::EMotionType::Static:
        bodies().SetPositionAndRotation(record.body,
                                        toJolt(pose.position),
                                        toJolt(pose.rotation),
                                        JPH::EActivation::DontActivate);
        break;
      case JPH::EMotionType::Kinematic:
        // Moves through the world (pushing what's in the way) over this frame.
        bodies().MoveKinematic(record.body,
                               toJolt(pose.position),
                               toJolt(pose.rotation),
                               std::max(frameTime, kStep));
        break;
      case JPH::EMotionType::Dynamic:
        bodies().SetPositionAndRotation(record.body,
                                        toJolt(pose.position),
                                        toJolt(pose.rotation),
                                        JPH::EActivation::Activate);
        break;
      }
      record.lastWorld = world;
    };

    for (const Entity& entity :
         registry.view<TransformComponent, BoxCollider3DComponent>())
      visit(entity);
    for (const Entity& entity :
         registry.view<TransformComponent, SphereCollider3DComponent>())
      visit(entity);
    for (const Entity& entity :
         registry.view<TransformComponent, CapsuleCollider3DComponent>())
      visit(entity);

    // Bodies whose entity is gone, or lost its colliders.
    for (auto it = records.begin(); it != records.end();)
    {
      auto next = std::next(it);
      if (!seen.contains(it->first) || !it->second.entity.isAlive())
        removeRecord(it);
      it = next;
    }

    // A big batch (a scene's first update) makes the broad phase lopsided.
    if (created && !optimised)
    {
      physics.OptimizeBroadPhase();
      optimised = true;
    }
  }

  // Dynamic bodies' new poses into their transforms.
  void writeBack()
  {
    for (auto& [id, record] : records)
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
    }
  }

  void collectContacts()
  {
    for (const ContactCollector::Change& change : contacts.changes)
    {
      if (change.added)
      {
        ActivePair& active = activePairs[change.pair];
        if (active.count++ == 0)
        {
          active.trigger = change.trigger;
          begun.push_back({entityOf(change.pair.first),
                           entityOf(change.pair.second),
                           change.trigger});
        }
        continue;
      }
      const auto it = activePairs.find(change.pair);
      if (it == activePairs.end())
        continue;
      if (--it->second.count == 0)
      {
        reportEnded(change.pair, it->second.trigger);
        activePairs.erase(it);
      }
    }
    contacts.changes.clear();
  }
};

PhysicsSystem3D::PhysicsSystem3D() : m(std::make_unique<Impl>()) {}

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
  if (!registry)
    return;

  m->sync(*registry, static_cast<float>(deltaTime));

  m->accumulator += deltaTime;
  int steps = 0;
  while (m->accumulator >= kStep && steps < kMaxStepsPerUpdate)
  {
    m->physics.Update(kStep, 1, m->tempAllocator.get(), m->jobs.get());
    m->collectContacts();
    m->accumulator -= kStep;
    ++steps;
  }
  if (steps == kMaxStepsPerUpdate)
    m->accumulator = 0.0; // fell behind: drop the rest rather than catch up

  if (steps > 0)
    m->writeBack();
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

bool PhysicsSystem3D::hasBody(const Entity& entity) const
{
  return m->find(entity) != nullptr;
}

const std::vector<Contact3D>& PhysicsSystem3D::contactsBegun() const
{
  return m->begun;
}

const std::vector<Contact3D>& PhysicsSystem3D::contactsEnded() const
{
  return m->ended;
}

} // namespace sfs
