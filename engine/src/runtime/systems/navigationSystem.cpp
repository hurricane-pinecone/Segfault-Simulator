#include "engine/runtime/systems/navigationSystem.h"

#include "engine/core/components/boxCollider3DComponent.h"
#include "engine/core/components/navAgentComponent.h"
#include "engine/core/components/transformComponent.h"
#include "engine/core/components/vehicleComponent.h"
#include "engine/core/hierarchy/hierarchy.h"
#include "engine/core/jobs/workerPool.h"
#include "engine/core/logger/logger.h"
#include "engine/core/util/debugFeatures.h"
#include "engine/runtime/navigation/coarseGraph.h"
#include "engine/runtime/navigation/collisionSpace.h"
#include "engine/runtime/navigation/vehicle/vehicleKind.h"
#include "engine/runtime/navigation/vehicle/vehicleSpec.h"
#include "engine/runtime/rendering/renderSystem.h"
#include "engine/runtime/systems/physicsSystem3D.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <mutex>
#include <tuple>

#include <glm/glm/gtc/matrix_transform.hpp>
#include <glm/glm/matrix.hpp>

namespace sfs
{

namespace
{

constexpr float kDegrees = 3.14159265358979f / 180.0f;

// An agent's frame: its reference point (the axle) on the ground, facing
// the entity's +X.
glm::mat4 frameOf(const Entity& e, const glm::vec3& axle)
{
  return hierarchy::worldMatrix(e) * glm::translate(glm::mat4(1.0f), axle);
}

// A box collider's corners in `frame`, as a Box3 (its bounds there).
std::optional<nav::Box3> boxIn(const glm::mat4& frameInverse,
                               const Entity& e,
                               const BoxCollider3DComponent& c,
                               float heightOffset)
{
  const glm::mat4 m = frameInverse * hierarchy::worldMatrix(e);
  glm::vec3 lo(1e9f), hi(-1e9f);
  for (int i = 0; i < 8; ++i)
  {
    const glm::vec3 corner = c.offset + 0.5f * c.size *
                                            glm::vec3(i & 1 ? 1.0f : -1.0f,
                                                      i & 2 ? 1.0f : -1.0f,
                                                      i & 4 ? 1.0f : -1.0f);
    const glm::vec3 p = glm::vec3(m * glm::vec4(corner, 1.0f));
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  if (hi.x <= lo.x || hi.y <= lo.y)
    return std::nullopt;
  nav::Box3 b;
  b.back = lo.x;
  b.front = hi.x;
  b.half = 0.5f * (hi.y - lo.y);
  b.shift = 0.5f * (hi.y + lo.y);
  b.bottom = lo.z + heightOffset;
  b.top = hi.z + heightOffset;
  return b;
}

// Whether two agents plan the same (all but their boxes and priority).
bool sameParams(const nav::AgentSpec& a, const nav::AgentSpec& b)
{
  const auto tie = [](const nav::AgentSpec& c)
  {
    return std::tie(c.kind,
                    c.search,
                    c.minRadius,
                    c.wheelbase,
                    c.steerRate,
                    c.topSpeed,
                    c.reverseSpeed,
                    c.accel,
                    c.brake,
                    c.lateralAccel,
                    c.mayReverse);
  };
  return tie(a) == tie(b);
}

bool sameHitch(const std::optional<NavigationSystem::Hitch>& a,
               const std::optional<NavigationSystem::Hitch>& b)
{
  if (!a || !b)
    return !a && !b;
  return a->towed == b->towed && a->hitchBehind == b->hitchBehind &&
         a->drawbar == b->drawbar && a->forwardLimit == b->forwardLimit &&
         a->reverseLimit == b->reverseLimit && a->rigid == b->rigid &&
         a->rideDrop == b->rideDrop;
}

uint32_t color(float r, float g, float b, float a = 1.0f)
{
  return DebugLines::pack({r, g, b, a});
}

void drawFootprint(DebugLines& lines,
                   const nav::Footprint& f,
                   float z,
                   uint32_t c)
{
  const glm::vec2 fw = nav::unit(f.heading), l{-fw.y, fw.x};
  std::array<glm::vec2, 4> p{f.centre + fw * f.half.x + l * f.half.y,
                             f.centre - fw * f.half.x + l * f.half.y,
                             f.centre - fw * f.half.x - l * f.half.y,
                             f.centre + fw * f.half.x - l * f.half.y};
  for (int i = 0; i < 4; ++i)
    lines.line(glm::vec3(p[i], z), glm::vec3(p[(i + 1) % 4], z), c);
}

} // namespace

// What it knows of each agent's entity.
struct NavigationSystem::Tracked
{
  Entity entity;
  NavAgentComponent fields;
  nav::AgentSpec params; // (no boxes)
  glm::vec3 axle{0.0f};  // its reference point in its frame (its spec's)
  std::optional<Hitch> hitch;
  bool fresh = true;
};

// What an agent plans by, besides its boxes.
struct NavigationSystem::Params
{
  nav::AgentSpec spec;
  glm::vec3 axle{0.0f};
};

// The coarse graph built on a worker for one world.
struct NavigationSystem::GraphJob
{
  std::mutex mutex;
  std::shared_ptr<const nav::CoarseGraph> graph;
  std::shared_ptr<const nav::Space> space; // what it's for
  bool done = false;
};

NavigationSystem::NavigationSystem()
{
  registerComponent<NavAgentComponent>();
  registerComponent<TransformComponent>();
  nav::vehicle::addKind();
  m_fleet = std::make_unique<nav::Fleet>(nav::Fleet::Settings{});
  m_graphWorker = std::make_unique<WorkerPool>(1);
}

NavigationSystem::~NavigationSystem()
{
  m_graphWorker.reset();
  m_fleet.reset();
}

uint64_t NavigationSystem::requestGoal(const Entity& agent,
                                       const nav::AgentGoal& goal)
{
  return requestGoals(agent, {goal});
}

uint64_t NavigationSystem::requestGoals(const Entity& agent,
                                        std::vector<nav::AgentGoal> goals,
                                        std::function<bool(nav::Pose2)> test)
{
  syncAgents();
  return m_fleet->requestGoals(
      agent.getId(), std::move(goals), std::move(test));
}

void NavigationSystem::cancel(const Entity& agent)
{
  m_fleet->cancel(agent.getId());
}

void NavigationSystem::setPath(const Entity& agent,
                               std::shared_ptr<const nav::AgentPath> path)
{
  syncAgents();
  m_fleet->setPath(agent.getId(), std::move(path));
}

nav::AgentPlan NavigationSystem::planNow(const Entity& agent,
                                         nav::PlanAsk ask) const
{
  return m_fleet->planNow(agent.getId(), std::move(ask));
}

void NavigationSystem::setSpeedCap(const Entity& agent, float cap)
{
  m_speedCaps[agent.getId()] = cap;
  m_fleet->setSpeedCap(agent.getId(), cap);
}

void NavigationSystem::setPlanning(const Entity& agent, bool planning)
{
  m_fleet->setPlanning(agent.getId(), planning);
}

bool NavigationSystem::takeWheel(const Entity& agent)
{
  // (From where it stands now: put somewhere since the last update, it's
  // driven from there.)
  if (const auto it = m_tracked.find(agent.getId());
      it != m_tracked.end() && registry && !m_fleet->path(agent.getId()) &&
      !m_fleet->handDriven(agent.getId()))
  {
    const TransformComponent world = hierarchy::worldTransform(agent);
    const glm::vec3 axle = it->second->axle;
    const glm::vec2 at =
        glm::vec2(world.position) +
        glm::vec2(glm::vec3(world.rotation * glm::vec3(axle.x, axle.y, 0)));
    m_fleet->place(agent.getId(), {at, world.yaw()});
  }
  return m_fleet->takeWheel(agent.getId());
}

void NavigationSystem::setControls(const Entity& agent,
                                   const nav::Controls& controls)
{
  m_fleet->setControls(agent.getId(), controls);
}

void NavigationSystem::handBack(const Entity& agent)
{
  m_fleet->handBack(agent.getId());
}

bool NavigationSystem::handDriven(const Entity& agent) const
{
  return m_fleet->handDriven(agent.getId());
}

void NavigationSystem::setActive(const Entity& agent, bool active)
{
  if (active)
    m_inactive.erase(agent.getId());
  else
    m_inactive.insert(agent.getId());
}

void NavigationSystem::setCostLayer(const std::string& name,
                                    std::shared_ptr<const nav::CostLayer> layer)
{
  if (layer)
    m_costs[name] = std::move(layer);
  else
    m_costs.erase(name);
  m_worldDirty = true;
}

void NavigationSystem::setPriorityProvider(
    std::function<int(const Entity&)> priority)
{
  m_priority = std::move(priority);
}

void NavigationSystem::cooperate(const Entity& a, const Entity& b, bool on)
{
  m_fleet->cooperate(a.getId(), b.getId(), on);
}

void NavigationSystem::setGround(std::function<float(glm::vec2)> ground)
{
  m_ground = std::move(ground);
  m_revision = ~0ull; // (rebuilt)
}

void NavigationSystem::setSpace(std::shared_ptr<const nav::Space> space)
{
  m_givenSpace = std::move(space);
  m_revision = ~0ull;
}

void NavigationSystem::refresh(const Entity& agent)
{
  if (const auto it = m_tracked.find(agent.getId()); it != m_tracked.end())
    m_fleet->setSpec(agent.getId(), specOf(*it->second));
}

void NavigationSystem::hitch(const Entity& agent, const Hitch& hitch)
{
  m_hitches[agent.getId()] = hitch;
}

void NavigationSystem::unhitch(const Entity& agent)
{
  m_hitches.erase(agent.getId());
}

void NavigationSystem::setRigProvider(
    std::function<nav::vehicle::Rig(const Entity&)> rig)
{
  m_rig = std::move(rig);
}

nav::Fleet::State NavigationSystem::state(const Entity& agent) const
{
  return m_fleet->state(agent.getId());
}

bool NavigationSystem::partOfAgent(const Entity& e) const
{
  for (const auto& [id, t] : m_tracked)
  {
    if (e == t->entity || hierarchy::isAncestor(t->entity, e))
      return true;
    if (t->hitch && registry->isAlive(t->hitch->towed) &&
        (e == t->hitch->towed || hierarchy::isAncestor(t->hitch->towed, e)))
      return true;
  }
  return false;
}

std::optional<NavigationSystem::Params>
NavigationSystem::paramsOf(const Entity& agent)
{
  const NavAgentComponent& c = registry->getComponent<NavAgentComponent>(agent);
  const auto complain = [&](const std::string& why) -> std::optional<Params>
  {
    std::string& said = m_complaints[agent.getId()];
    if (said != why)
      LOG_ERROR("NavigationSystem: agent " + std::to_string(agent.getId()) +
                " " + why + ": left out");
    said = why;
    return std::nullopt;
  };
  Params p;
  p.spec.kind = c.kind;
  p.spec.search = c.search;
  if (c.kind == "vehicle")
  {
    // A vehicle moves as its VehicleSpec says, with what it tows and
    // carries and the grip under it.
    if (!registry->hasComponent<VehicleComponent>(agent))
      return complain("is a vehicle with no VehicleComponent");
    const VehicleComponent& v = registry->getComponent<VehicleComponent>(agent);
    const VehicleSpec* spec = v.spec.get();
    if (!spec)
      return complain(v.spec.empty() ? "has a VehicleComponent with no spec"
                                     : "has a VehicleComponent whose spec " +
                                           v.spec.asset + " can't be read");
    nav::vehicle::Rig rig = m_rig ? m_rig(agent) : nav::vehicle::Rig{};
    rig.effort = c.effort;
    nav::vehicle::applySpec(p.spec, *spec, rig);
    // (Its reference point: the spec's, else the axle measured (the rig's).)
    p.axle = spec->axle != glm::vec3(0.0f) ? spec->axle
                                           : glm::vec3(rig.axle, 0.0f, 0.0f);
  }
  else if (!nav::AgentKinds::find(c.kind))
    return complain("has an unknown kind '" + c.kind + "'");
  m_complaints.erase(agent.getId());
  return p;
}

nav::Box3
NavigationSystem::mountedBox(const glm::mat4& world,
                             const glm::vec3& lo,
                             const glm::vec3& hi,
                             const nav::Pose2& at,
                             const std::function<float(glm::vec2)>& ground,
                             float drop)
{
  const glm::vec2 f(std::cos(at.heading), std::sin(at.heading)), l(-f.y, f.x);
  const glm::vec3 middle(world * glm::vec4(0.5f * (lo + hi), 1.0f));
  const float under = ground ? ground(glm::vec2(middle)) : 0.0f;
  glm::vec3 a(std::numeric_limits<float>::infinity());
  glm::vec3 b(-std::numeric_limits<float>::infinity());
  for (int corner = 0; corner < 8; ++corner)
  {
    const glm::vec3 c((corner & 1) ? hi.x : lo.x,
                      (corner & 2) ? hi.y : lo.y,
                      (corner & 4) ? hi.z : lo.z);
    const glm::vec3 p(world * glm::vec4(c, 1.0f));
    const glm::vec2 d = glm::vec2(p) - at.position;
    const glm::vec3 q(glm::dot(d, f), glm::dot(d, l), p.z - under);
    a = glm::min(a, q);
    b = glm::max(b, q);
  }
  nav::Box3 box;
  box.back = a.x;
  box.front = b.x;
  box.half = 0.5f * (b.y - a.y);
  box.shift = 0.5f * (b.y + a.y);
  box.bottom = std::max(a.z - drop, 0.0f);
  box.top = b.z;
  return box;
}

nav::AgentSpec NavigationSystem::specOf(const Tracked& t) const
{
  const Entity& agent = t.entity;
  nav::AgentSpec s = t.params;
  s.boxes.clear();
  s.trailer.reset();

  Entity towed;
  if (t.hitch)
    towed = t.hitch->towed;
  const bool tows = t.hitch && registry->isAlive(towed);
  const glm::mat4 frame = frameOf(agent, t.axle);
  const glm::mat4 own = glm::inverse(frame);
  // (Its reference point on the ground, facing its heading; heights above
  // the ground there, or its own height with no ground set.)
  const nav::Pose2 reference{
      glm::vec2(frame[3]), hierarchy::worldTransform(agent).yaw()};
  const float referenceZ = frame[3].z;
  const std::function<float(glm::vec2)> ground =
      m_ground ? m_ground
               : std::function<float(glm::vec2)>([referenceZ](glm::vec2)
                                                 { return referenceZ; });
  const glm::mat4 trailerFrame =
      tows ? glm::inverse(hierarchy::worldMatrix(towed)) : glm::mat4(1.0f);
  std::optional<nav::Box3> trailerBox;
  registry->each<BoxCollider3DComponent>(
      [&](Entity e, BoxCollider3DComponent& box)
      {
        if (box.trigger)
          return;
        if (tows && t.hitch->rigid &&
            (e == towed || hierarchy::isAncestor(towed, e)))
        {
          // (Mounted: one of its own boxes, where it's held now, in the
          // agent's flat frame: mountedBox().)
          s.boxes.push_back(mountedBox(hierarchy::worldMatrix(e),
                                       box.offset - 0.5f * box.size,
                                       box.offset + 0.5f * box.size,
                                       reference,
                                       ground,
                                       t.hitch->rideDrop));
          return;
        }
        if (tows && (e == towed || hierarchy::isAncestor(towed, e)))
        {
          if (const auto b = boxIn(trailerFrame, e, box, 0.0f))
          {
            if (!trailerBox)
              trailerBox = *b;
            else
            {
              // (One box for the trailer: the bounds of its colliders.)
              nav::Box3& tb = *trailerBox;
              const float lo = std::min(tb.shift - tb.half, b->shift - b->half);
              const float hi = std::max(tb.shift + tb.half, b->shift + b->half);
              tb.back = std::min(tb.back, b->back);
              tb.front = std::max(tb.front, b->front);
              tb.half = 0.5f * (hi - lo);
              tb.shift = 0.5f * (hi + lo);
              tb.bottom = std::min(tb.bottom, b->bottom);
              tb.top = std::max(tb.top, b->top);
            }
          }
          return;
        }
        if (e == agent || hierarchy::isAncestor(agent, e))
          if (const auto b = boxIn(own, e, box, t.axle.z))
            s.boxes.push_back(*b);
      });
  if (tows && !t.hitch->rigid && trailerBox)
  {
    nav::AgentSpec::Towed tw;
    tw.hitchBehind = t.hitch->hitchBehind;
    tw.drawbar = t.hitch->drawbar;
    tw.box = *trailerBox;
    tw.forwardLimit = t.hitch->forwardLimit * kDegrees;
    tw.reverseLimit = t.hitch->reverseLimit * kDegrees;
    s.trailer = tw;
  }
  return s;
}

std::shared_ptr<const nav::CostLayer> NavigationSystem::combinedCost() const
{
  if (m_costs.empty())
    return nullptr;
  std::vector<std::shared_ptr<const nav::CostLayer>> layers;
  for (const auto& [name, layer] : m_costs)
    layers.push_back(layer);
  return std::make_shared<nav::CombinedCost>(std::move(layers));
}

void NavigationSystem::syncAgents()
{
  if (!registry)
    return;
  for (auto& [id, t] : m_tracked)
    t->fresh = false;
  for (const Entity& e : getEntities())
  {
    if (m_inactive.contains(e.getId()))
      continue; // (left out: dropped below if it was an agent)
    const NavAgentComponent& c = registry->getComponent<NavAgentComponent>(e);
    const std::optional<Params> params = paramsOf(e);
    if (!params)
      continue; // (logged; dropped below if it was an agent)
    std::optional<Hitch> hitch;
    if (const auto h = m_hitches.find(e.getId()); h != m_hitches.end())
      hitch = h->second;

    auto it = m_tracked.find(e.getId());
    const TransformComponent world = hierarchy::worldTransform(e);
    const float yaw = world.yaw();
    const glm::vec3 axle = params->axle;
    const glm::vec2 at =
        glm::vec2(world.position) +
        glm::vec2(glm::vec3(world.rotation * glm::vec3(axle.x, axle.y, 0)));
    if (it == m_tracked.end() || it->second->entity != e)
    {
      auto t = std::make_unique<Tracked>();
      t->entity = e;
      t->fields = c;
      t->params = params->spec;
      t->axle = axle;
      t->hitch = hitch;
      Tracked& tracked = *t;
      m_tracked[e.getId()] = std::move(t);
      m_fleet->add(e.getId(), specOf(tracked), {at, yaw});
      if (const auto cap = m_speedCaps.find(e.getId());
          cap != m_speedCaps.end())
        m_fleet->setSpeedCap(e.getId(), cap->second);
      m_revision = ~0ull; // (its parts leave the statics)
      it = m_tracked.find(e.getId());
    }
    else
    {
      Tracked& t = *it->second;
      const bool reshaped = t.axle != axle || !sameHitch(t.hitch, hitch) ||
                            t.fields.layers != c.layers;
      if (reshaped || !sameParams(t.params, params->spec))
      {
        // Re-derived (a hitch, a load, the grip, its spec or fields changed).
        t.fields = c;
        t.params = params->spec;
        t.axle = axle;
        t.hitch = hitch;
        m_fleet->setSpec(e.getId(), specOf(t));
        if (reshaped)
          m_revision = ~0ull;
      }
    }
    it->second->fresh = true;
    it->second->fields.priority = c.priority;
    m_fleet->setPriority(e.getId(), m_priority ? m_priority(e) : c.priority);
    // Moved by something else while it has no path (placed, teleported):
    // where it is now.
    if (!m_fleet->path(e.getId()) && !m_fleet->handDriven(e.getId()))
    {
      const nav::Pose2 p = m_fleet->pose(e.getId());
      if (glm::length(p.position - at) > 0.01f ||
          std::abs(nav::wrapAngle(p.heading - yaw)) > 0.002f)
        m_fleet->place(e.getId(), {at, yaw});
    }
  }
  for (auto it = m_tracked.begin(); it != m_tracked.end();)
    if (!it->second->fresh)
    {
      m_fleet->remove(it->first);
      it = m_tracked.erase(it);
      m_revision = ~0ull;
    }
    else
      ++it;
}

void NavigationSystem::syncWorld()
{
  PhysicsSystem3D* physics =
      registry ? registry->tryGetSystem<PhysicsSystem3D>() : nullptr;
  bool rebuilt = false;
  if (m_givenSpace)
  {
    if (m_space != m_givenSpace)
    {
      m_space = m_givenSpace;
      rebuilt = true;
    }
  }
  else if (physics && physics->revision() != m_revision)
  {
    m_revision = physics->revision();
    uint32_t layers = 0;
    for (const auto& [id, t] : m_tracked)
      layers |= t->fields.layers;
    nav::CollisionSpace::Options o;
    o.layers = m_tracked.empty() ? ~0u : layers;
    o.ground = m_ground;
    auto statics =
        physics->snapshot([this](const Entity& e) { return !partOfAgent(e); });
    m_space = std::make_shared<nav::CollisionSpace>(
        std::vector<std::shared_ptr<const CollisionWorld3D>>{statics}, o);
    rebuilt = true;
  }
  if (rebuilt && m_space)
  {
    // The coarse graph of the new statics, on a worker.
    auto job = std::make_shared<GraphJob>();
    job->space = m_space;
    m_graphJob = job;
    m_graphWorker->submit(
        [job]
        {
          const nav::Rect area = job->space->bounds();
          auto graph = nav::CoarseGraph::build(*job->space, area);
          std::lock_guard lock(job->mutex);
          job->graph = std::move(graph);
          job->done = true;
        });
    m_worldDirty = true;
  }
  std::shared_ptr<const nav::CoarseGraph> graph = m_fleet->world().graph;
  if (m_graphJob)
  {
    std::lock_guard lock(m_graphJob->mutex);
    if (m_graphJob->done && m_graphJob->space == m_space)
    {
      graph = m_graphJob->graph;
      m_graphJob = nullptr;
      m_worldDirty = true;
    }
  }
  if (m_worldDirty && m_space)
  {
    if (rebuilt)
      graph = nullptr; // (the old one's for the old statics)
    m_fleet->setWorld({m_space, combinedCost(), graph});
    m_worldDirty = false;
  }
}

void NavigationSystem::writeBack()
{
  for (const auto& [id, t] : m_tracked)
  {
    if (!m_fleet->path(id) && !m_fleet->handDriven(id))
      continue;
    const Entity& e = t->entity;
    const nav::Pose2 p = m_fleet->pose(id);
    TransformComponent world = hierarchy::worldTransform(e);
    world.setYaw(p.heading);
    const glm::vec3 axle =
        world.rotation * glm::vec3(t->axle.x, t->axle.y, 0.0f);
    world.position.x = p.position.x - axle.x;
    world.position.y = p.position.y - axle.y;
    hierarchy::setWorldTransform(e, world);
    if (!t->hitch || t->hitch->rigid)
      continue; // (a mounted one: the game holds it where it's mounted)
    const Entity towed = t->hitch->towed;
    const std::optional<nav::Pose2> tp = m_fleet->towedPose(id);
    if (!tp || !registry->isAlive(towed) ||
        !registry->hasComponent<TransformComponent>(towed))
      continue;
    TransformComponent tw = hierarchy::worldTransform(towed);
    tw.setYaw(tp->heading);
    tw.position.x = tp->position.x;
    tw.position.y = tp->position.y;
    hierarchy::setWorldTransform(towed, tw);
  }
}

void NavigationSystem::draw()
{
  if (!debugFeatures::enabled() ||
      !(m_drawPaths || m_drawClaims || m_drawBlocked))
    return;
  RenderSystem* render = registry->tryGetSystem<RenderSystem>();
  if (!render)
    return;
  DebugLines& lines = render->debugLines();
  const float z = 0.15f;
  for (const auto& [id, t] : m_tracked)
  {
    const nav::Fleet::State st = m_fleet->state(id);
    if (m_drawPaths)
    {
      const uint32_t forward = color(0.2f, 0.9f, 0.3f),
                     back = color(1.0f, 0.55f, 0.1f);
      if (const nav::AgentPath* path = m_fleet->path(id))
      {
        const auto& s = path->samples;
        float last = -1.0f;
        std::size_t from = 0;
        for (std::size_t k = 1; k < s.size(); ++k)
          if (s[k].s - last >= 0.5f || k + 1 == s.size())
          {
            lines.line(glm::vec3(s[from].pose.position, z),
                       glm::vec3(s[k].pose.position, z),
                       s[k].gear < 0 ? back : forward);
            from = k;
            last = s[k].s;
          }
      }
      for (const nav::AgentGoal& g : m_fleet->goals(id))
      {
        const glm::vec2 f = nav::unit(g.pose.heading);
        lines.line(glm::vec3(g.pose.position, z),
                   glm::vec3(g.pose.position + 2.0f * f, z),
                   color(0.3f, 0.6f, 1.0f));
      }
    }
    if (m_drawClaims)
    {
      const uint32_t mine = st == nav::Fleet::State::Yielding
                                ? color(1.0f, 0.9f, 0.1f)
                                : color(0.6f, 0.6f, 1.0f, 0.6f);
      for (const nav::Footprint& f : m_fleet->footprint(id))
        drawFootprint(lines, f, z, mine);
      if (const nav::AgentPath* path = m_fleet->path(id))
      {
        const float s0 = m_fleet->along(id);
        float last = -1e9f;
        const nav::AgentSpec* spec = m_fleet->spec(id);
        const auto kind = spec ? nav::AgentKinds::find(spec->kind) : nullptr;
        for (const auto& p : path->samples)
          if (kind && p.s > s0 && p.s - last >= 2.0f &&
              p.s - s0 < 8.0f * std::max(1.0f, std::abs(m_fleet->speed(id))))
          {
            last = p.s;
            for (const nav::Footprint& f :
                 kind->footprint(*spec, p.pose, p.hitch))
              drawFootprint(lines, f, z, color(0.6f, 0.6f, 1.0f, 0.3f));
          }
      }
    }
    if (m_drawBlocked && st == nav::Fleet::State::Blocked)
    {
      const uint32_t red = color(1.0f, 0.1f, 0.1f);
      for (const nav::Footprint& f : m_fleet->footprint(id))
        drawFootprint(lines, f, z + 0.05f, red);
      for (const nav::Space::Hit& h : m_fleet->blockers(id))
      {
        lines.line(glm::vec3(h.point + glm::vec2(-0.5f, -0.5f), z),
                   glm::vec3(h.point + glm::vec2(0.5f, 0.5f), z),
                   red);
        lines.line(glm::vec3(h.point + glm::vec2(-0.5f, 0.5f), z),
                   glm::vec3(h.point + glm::vec2(0.5f, -0.5f), z),
                   red);
        lines.line(glm::vec3(h.point, 0.0f), glm::vec3(h.point, 3.0f), red);
      }
    }
  }
}

void NavigationSystem::update(double deltaTime)
{
  m_events.clear();
  if (!registry)
    return;
  syncAgents();
  syncWorld();
  m_fleet->update(deltaTime);
  for (nav::Fleet::Event& e : m_fleet->takeEvents())
    if (const auto it = m_tracked.find(static_cast<Entity::EntityId>(e.agent));
        it != m_tracked.end())
      m_events.push_back({it->second->entity, std::move(e)});
  writeBack();
  draw();
}

std::vector<ModuleSetting> NavigationSystem::settings()
{
  std::vector<ModuleSetting> out;
  const bool debug = debugFeatures::enabled();
  const auto toggle = [&](const char* label, bool& value)
  {
    ModuleSetting s;
    s.label = label;
    s.type = ModuleSetting::Type::Bool;
    s.enabled = debug;
    s.getBool = [&value] { return value; };
    s.setBool = [&value](bool v) { value = v; };
    out.push_back(std::move(s));
  };
  toggle("Draw paths", m_drawPaths);
  toggle("Draw claims", m_drawClaims);
  toggle("Draw blocked ways", m_drawBlocked);
  {
    ModuleSetting s;
    s.label = "Agents";
    s.type = ModuleSetting::Type::Text;
    s.getText = [this]
    {
      int n[6] = {};
      for (const auto& [id, t] : m_tracked)
        ++n[static_cast<int>(m_fleet->state(id))];
      return std::to_string(m_tracked.size()) + " (" + std::to_string(n[2]) +
             " moving, " + std::to_string(n[1]) + " planning, " +
             std::to_string(n[3]) + " giving way, " + std::to_string(n[5]) +
             " blocked)";
    };
    out.push_back(std::move(s));
  }
  static const char* names[] = {
      "idle", "planning", "moving", "giving way", "arrived", "blocked"};
  for (const auto& [id, t] : m_tracked)
  {
    ModuleSetting s;
    s.label = "Agent " + std::to_string(id);
    s.type = ModuleSetting::Type::Text;
    s.getText = [this, id = id]
    {
      const nav::Fleet::Counters c = m_fleet->counters(id);
      char buf[160];
      std::snprintf(buf,
                    sizeof buf,
                    "%s, %.1f m/s, plans %d (for traffic %d)",
                    names[static_cast<int>(m_fleet->state(id))],
                    std::abs(m_fleet->speed(id)),
                    c.plans,
                    c.replans);
      return std::string(buf);
    };
    out.push_back(std::move(s));
  }
  return out;
}

} // namespace sfs
