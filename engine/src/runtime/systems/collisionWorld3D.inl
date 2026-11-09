// CollisionWorld3D: a frozen copy of solid bodies, queried from any thread.
// Included by physicsSystem3D.cpp, the one unit that includes Jolt (its build
// options are that file's: see engine/CMakeLists.txt), after Jolt.h and the
// headers below. Needs only Jolt and glm.

#include "engine/runtime/systems/collisionWorld3D.h"

#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <algorithm>
#include <mutex>

namespace sfs
{

namespace collisionWorld
{

void ready()
{
  static std::once_flag once;
  std::call_once(once,
                 []
                 {
                   JPH::RegisterDefaultAllocator();
                   JPH::Factory::sInstance = new JPH::Factory();
                   JPH::RegisterTypes();
                 });
}

// One layer for everything: a frozen world has no moving bodies to keep apart,
// and queries filter by the bodies' own layers (BodyFilter) instead.
class OneLayer final : public JPH::BroadPhaseLayerInterface
{
public:
  JPH::uint GetNumBroadPhaseLayers() const override { return 1; }
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer) const override
  {
    return JPH::BroadPhaseLayer(0);
  }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
  const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override
  {
    return "all";
  }
#endif
};
class AnyVsLayer final : public JPH::ObjectVsBroadPhaseLayerFilter
{
public:
  bool ShouldCollide(JPH::ObjectLayer, JPH::BroadPhaseLayer) const override
  {
    return true;
  }
};
class AnyPair final : public JPH::ObjectLayerPairFilter
{
public:
  bool ShouldCollide(JPH::ObjectLayer, JPH::ObjectLayer) const override
  {
    return true;
  }
};

// Exact shapes: no rounded corners (a planner's centimetres are real ones).
JPH::RefConst<JPH::Shape> build(const CollisionShape3D& shape)
{
  constexpr float kMin = 0.005f;
  JPH::ShapeSettings::ShapeResult result;
  JPH::Quat turn(
      shape.rotation.x, shape.rotation.y, shape.rotation.z, shape.rotation.w);
  switch (shape.kind)
  {
  case CollisionShape3D::Kind::Box:
  {
    const glm::vec3 half = glm::max(shape.size * 0.5f, glm::vec3(kMin));
    result =
        JPH::BoxShapeSettings(JPH::Vec3(half.x, half.y, half.z), 0.0f).Create();
    break;
  }
  case CollisionShape3D::Kind::Sphere:
    result = JPH::SphereShapeSettings(std::max(shape.size.x, kMin)).Create();
    break;
  case CollisionShape3D::Kind::Capsule:
  {
    const float radius = std::max(shape.size.x, kMin);
    const float halfCylinder = std::max(shape.size.y * 0.5f - radius, kMin);
    result = JPH::CapsuleShapeSettings(halfCylinder, radius).Create();
    // (Jolt's stand along Y; ours along Z.)
    turn = turn * JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f * JPH::JPH_PI);
    break;
  }
  }
  if (result.HasError())
    return nullptr;
  if (shape.offset == glm::vec3(0.0f) && turn == JPH::Quat::sIdentity())
    return result.Get();
  JPH::ShapeSettings::ShapeResult placed =
      JPH::RotatedTranslatedShapeSettings(
          JPH::Vec3(shape.offset.x, shape.offset.y, shape.offset.z),
          turn,
          result.Get())
          .Create();
  return placed.HasError() ? nullptr : placed.Get();
}

JPH::RefConst<JPH::Shape> build(std::span<const CollisionShape3D> shapes)
{
  if (shapes.empty())
    return nullptr;
  if (shapes.size() == 1)
    return build(shapes.front());
  JPH::StaticCompoundShapeSettings compound;
  for (const CollisionShape3D& shape : shapes)
    if (JPH::RefConst<JPH::Shape> part = build(shape))
      compound.AddShape(JPH::Vec3::sZero(), JPH::Quat::sIdentity(), part);
  JPH::ShapeSettings::ShapeResult result = compound.Create();
  return result.HasError() ? nullptr : result.Get();
}

} // namespace collisionWorld

CollisionWorld3D::Probe::Probe(std::span<const CollisionShape3D> shapes)
{
  collisionWorld::ready();
  JPH::RefConst<JPH::Shape> shape = collisionWorld::build(shapes);
  if (!shape)
    return;
  // (Held by a shared_ptr that keeps the Jolt reference.)
  auto* held = new JPH::RefConst<JPH::Shape>(shape);
  m_shape = std::shared_ptr<const void>(
      held,
      [](const void* p)
      { delete static_cast<const JPH::RefConst<JPH::Shape>*>(p); });
}

struct CollisionWorld3D::Impl
{
  collisionWorld::OneLayer layers;
  collisionWorld::AnyVsLayer anyVsLayer;
  collisionWorld::AnyPair anyPair;
  JPH::PhysicsSystem physics;
  std::vector<Body> bodies;

  // Leaves out bodies not in `mask` and those whose ids are in `ignore`.
  class Filter final : public JPH::BodyFilter
  {
  public:
    Filter(const std::vector<Body>& bodies,
           uint32_t mask,
           std::span<const uint64_t> ignore)
        : m_bodies(bodies), m_mask(mask), m_ignore(ignore)
    {
    }
    bool ShouldCollideLocked(const JPH::Body& body) const override
    {
      const Body& b = m_bodies[static_cast<std::size_t>(body.GetUserData())];
      if ((m_mask & (1u << (b.layer & 31u))) == 0)
        return false;
      return std::find(m_ignore.begin(), m_ignore.end(), b.id) ==
             m_ignore.end();
    }

  private:
    const std::vector<Body>& m_bodies;
    uint32_t m_mask;
    std::span<const uint64_t> m_ignore;
  };

  // (Jolt places a query shape by its centre of mass, not its origin.)
  static JPH::RMat44 centreOfMass(const JPH::Shape* shape,
                                  const glm::vec3& position,
                                  const glm::quat& rotation)
  {
    return JPH::RMat44::sRotationTranslation(
               JPH::Quat(rotation.x, rotation.y, rotation.z, rotation.w)
                   .Normalized(),
               JPH::RVec3(position.x, position.y, position.z))
        .PreTranslated(shape->GetCenterOfMass());
  }

  static const JPH::Shape* shapeOf(const Probe& probe)
  {
    return probe.m_shape ? static_cast<const JPH::RefConst<JPH::Shape>*>(
                               probe.m_shape.get())
                               ->GetPtr()
                         : nullptr;
  }
};

CollisionWorld3D::CollisionWorld3D(std::span<const Body> bodies)
    : m(std::make_unique<Impl>())
{
  collisionWorld::ready();
  m->bodies.assign(bodies.begin(), bodies.end());
  const JPH::uint count = static_cast<JPH::uint>(m->bodies.size());
  // (No pairs or contacts ever: it's never stepped. Jolt's tables want a few
  // buckets all the same.)
  m->physics.Init(std::max<JPH::uint>(count, 1),
                  0,
                  64,
                  64,
                  m->layers,
                  m->anyVsLayer,
                  m->anyPair);
  JPH::BodyInterface& add = m->physics.GetBodyInterfaceNoLock();
  std::vector<JPH::BodyID> ids;
  ids.reserve(count);
  for (std::size_t i = 0; i < m->bodies.size(); ++i)
  {
    const Body& body = m->bodies[i];
    JPH::RefConst<JPH::Shape> shape = collisionWorld::build(body.shapes);
    if (!shape)
      continue;
    JPH::BodyCreationSettings settings(
        shape,
        JPH::RVec3(body.position.x, body.position.y, body.position.z),
        JPH::Quat(
            body.rotation.x, body.rotation.y, body.rotation.z, body.rotation.w)
            .Normalized(),
        JPH::EMotionType::Static,
        0);
    settings.mUserData = i;
    if (const JPH::Body* created = add.CreateBody(settings))
      ids.push_back(created->GetID());
  }
  if (!ids.empty())
  {
    JPH::BodyInterface::AddState state =
        add.AddBodiesPrepare(ids.data(), static_cast<int>(ids.size()));
    add.AddBodiesFinalize(ids.data(),
                          static_cast<int>(ids.size()),
                          state,
                          JPH::EActivation::DontActivate);
  }
  m->physics.OptimizeBroadPhase();
}

CollisionWorld3D::~CollisionWorld3D() = default;

bool CollisionWorld3D::overlaps(const Probe& probe,
                                const glm::vec3& position,
                                const glm::quat& rotation,
                                uint32_t layers,
                                std::span<const uint64_t> ignore) const
{
  const JPH::Shape* shape = Impl::shapeOf(probe);
  if (!shape || m->bodies.empty())
    return false;
  JPH::CollideShapeSettings settings;
  JPH::AnyHitCollisionCollector<JPH::CollideShapeCollector> hit;
  const Impl::Filter filter(m->bodies, layers, ignore);
  m->physics.GetNarrowPhaseQueryNoLock().CollideShape(
      shape,
      JPH::Vec3::sReplicate(1.0f),
      Impl::centreOfMass(shape, position, rotation),
      settings,
      JPH::RVec3::sZero(),
      hit,
      {},
      {},
      filter);
  return hit.HadHit();
}

std::optional<CollisionWorld3D::Hit>
CollisionWorld3D::overlap(const Probe& probe,
                          const glm::vec3& position,
                          const glm::quat& rotation,
                          uint32_t layers,
                          std::span<const uint64_t> ignore) const
{
  const JPH::Shape* shape = Impl::shapeOf(probe);
  if (!shape || m->bodies.empty())
    return std::nullopt;
  JPH::CollideShapeSettings settings;
  JPH::AllHitCollisionCollector<JPH::CollideShapeCollector> hits;
  const Impl::Filter filter(m->bodies, layers, ignore);
  m->physics.GetNarrowPhaseQueryNoLock().CollideShape(
      shape,
      JPH::Vec3::sReplicate(1.0f),
      Impl::centreOfMass(shape, position, rotation),
      settings,
      JPH::RVec3::sZero(),
      hits,
      {},
      {},
      filter);
  if (!hits.HadHit())
    return std::nullopt;
  const JPH::CollideShapeResult* deepest = &hits.mHits.front();
  for (const JPH::CollideShapeResult& each : hits.mHits)
    if (each.mPenetrationDepth > deepest->mPenetrationDepth)
      deepest = &each;
  const JPH::BodyLockInterfaceNoLock& locks =
      m->physics.GetBodyLockInterfaceNoLock();
  JPH::BodyLockRead lock(locks, deepest->mBodyID2);
  if (!lock.Succeeded())
    return std::nullopt;
  const Body& body =
      m->bodies[static_cast<std::size_t>(lock.GetBody().GetUserData())];
  const JPH::Vec3 point = deepest->mContactPointOn2;
  return Hit{body.id,
             body.layer,
             glm::vec3(point.GetX(), point.GetY(), point.GetZ()),
             deepest->mPenetrationDepth};
}

std::optional<float>
CollisionWorld3D::cast(const Probe& probe,
                       const glm::vec3& from,
                       const glm::quat& fromRotation,
                       const glm::vec3& to,
                       uint32_t layers,
                       std::span<const uint64_t> ignore) const
{
  const JPH::Shape* shape = Impl::shapeOf(probe);
  if (!shape || m->bodies.empty())
    return std::nullopt;
  const glm::vec3 d = to - from;
  const JPH::RShapeCast sweep = JPH::RShapeCast::sFromWorldTransform(
      shape,
      JPH::Vec3::sReplicate(1.0f),
      JPH::RMat44::sRotationTranslation(
          JPH::Quat(
              fromRotation.x, fromRotation.y, fromRotation.z, fromRotation.w)
              .Normalized(),
          JPH::RVec3(from.x, from.y, from.z)),
      JPH::Vec3(d.x, d.y, d.z));
  JPH::ShapeCastSettings settings;
  JPH::ClosestHitCollisionCollector<JPH::CastShapeCollector> hit;
  const Impl::Filter filter(m->bodies, layers, ignore);
  m->physics.GetNarrowPhaseQueryNoLock().CastShape(
      sweep, settings, JPH::RVec3::sZero(), hit, {}, {}, filter);
  if (!hit.HadHit())
    return std::nullopt;
  return std::clamp(hit.mHit.mFraction, 0.0f, 1.0f);
}

std::size_t CollisionWorld3D::size() const { return m->bodies.size(); }

const std::vector<CollisionWorld3D::Body>& CollisionWorld3D::bodies() const
{
  return m->bodies;
}

} // namespace sfs
