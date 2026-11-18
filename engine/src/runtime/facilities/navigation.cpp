#include "engine/runtime/facilities/navigation.h"

#include "absentBackend.h"
#include "engine/core/ecs/entity.h"
#include "engine/core/ecs/registry.h"
#include "engine/runtime/navigation/agentKind.h"
#include "engine/runtime/navigation/coarseGraph.h"
#include "engine/runtime/navigation/costLayer.h"
#include "engine/runtime/navigation/fleet.h"
#include "engine/runtime/navigation/geometry.h"
#include "engine/runtime/navigation/laneGraph.h"
#include "engine/runtime/navigation/planScheduler.h"
#include "engine/runtime/navigation/space.h"
#include "engine/runtime/navigation/traffic.h"
#include "engine/runtime/navigation/vehicle/vehicleKind.h"
#include "engine/runtime/systems/navigationSystem.h"

#include <glm/glm/ext/vector_float2.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sfs::facility
{

namespace
{
std::atomic<bool> s_warned{false};

} // namespace

std::optional<nav::AgentPlan> Navigation::ahead(const Entity& agent) const
{
  NavigationSystem* navigation = backend();
  return navigation ? navigation->ahead(agent) : std::nullopt;
}

float Navigation::along(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().along(idOf(agent)) : 0.0f;
}

NavigationSystem* Navigation::backend() const
{
  if (!m_navigation)
    warnAbsent(s_warned, "navigation", "NavigationSystem");

  return m_navigation;
}

void Navigation::cancel(const Entity& agent) const
{
  if (NavigationSystem* navigation = backend())
    navigation->cancel(agent);
}

const std::vector<nav::CoarseGraph::Edge>& Navigation::coarseEdges() const
{
  static const std::vector<nav::CoarseGraph::Edge> none;
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->coarseEdges() : none;
}

std::shared_ptr<const nav::CoarseGraph> Navigation::coarseGraph() const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().world().graph : nullptr;
}

void Navigation::cooperate(const Entity& a, const Entity& b, bool on) const
{
  if (NavigationSystem* navigation = backend())
    navigation->cooperate(a, b, on);
}

nav::Fleet::Counters Navigation::counters(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().counters(idOf(agent))
                    : nav::Fleet::Counters{};
}

float Navigation::curvature(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().curvature(idOf(agent)) : 0.0f;
}

std::vector<nav::Footprint> Navigation::footprint(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().footprint(idOf(agent))
                    : std::vector<nav::Footprint>{};
}

void Navigation::handBack(const Entity& agent) const
{
  if (NavigationSystem* navigation = backend())
    navigation->handBack(agent);
}

bool Navigation::handDriven(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation && navigation->handDriven(agent);
}

float Navigation::hitchAngle(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().hitch(idOf(agent)) : 0.0f;
}

nav::Fleet::AgentId Navigation::idOf(const Entity& agent)
{
  return static_cast<nav::Fleet::AgentId>(agent.getId());
}

Navigation Navigation::in(Registry* registry)
{
  return Navigation(registry ? registry->tryGetSystem<NavigationSystem>()
                             : nullptr);
}

bool Navigation::isAgent(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation && navigation->fleet().has(idOf(agent));
}

std::shared_ptr<const nav::LaneGraph>
Navigation::laneGraph(const std::string& name) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->laneGraph(name) : nullptr;
}

const nav::AgentPath* Navigation::path(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().path(idOf(agent)) : nullptr;
}

bool Navigation::planAhead(const Entity& agent, nav::PlanAsk ask) const
{
  NavigationSystem* navigation = backend();
  return navigation && navigation->planAhead(agent, std::move(ask));
}

nav::AgentPlan Navigation::planNow(const Entity& agent, nav::PlanAsk ask) const
{
  NavigationSystem* navigation = backend();
  return navigation ? navigation->planNow(agent, std::move(ask))
                    : nav::AgentPlan{};
}

nav::Pose2 Navigation::pose(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().pose(idOf(agent)) : nav::Pose2{};
}

bool Navigation::ranksAbove(const Entity& a, const Entity& b) const
{
  const NavigationSystem* navigation = backend();
  return navigation &&
         navigation->fleet().traffic().ranksAbove(idOf(a), idOf(b));
}

void Navigation::refresh(const Entity& agent) const
{
  if (NavigationSystem* navigation = backend())
    navigation->refresh(agent);
}

uint64_t Navigation::requestGoal(const Entity& agent,
                                 const nav::AgentGoal& goal) const
{
  NavigationSystem* navigation = backend();
  return navigation ? navigation->requestGoal(agent, goal) : 0;
}

uint64_t
Navigation::requestGoals(const Entity& agent,
                         std::vector<nav::AgentGoal> goals,
                         std::function<bool(nav::Pose2)> goalTest) const
{
  NavigationSystem* navigation = backend();
  return navigation ? navigation->requestGoals(
                          agent, std::move(goals), std::move(goalTest))
                    : 0;
}

nav::Traffic::Schedule Navigation::schedule(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().traffic().scheduleOf(idOf(agent))
                    : nav::Traffic::Schedule{};
}

std::shared_ptr<nav::PlanScheduler> Navigation::scheduler() const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->sharedScheduler() : nullptr;
}

void Navigation::setCoarseEdges(std::vector<nav::CoarseGraph::Edge> edges) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setCoarseEdges(std::move(edges));
}

void Navigation::setControls(const Entity& agent,
                             const nav::Controls& controls) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setControls(agent, controls);
}

void Navigation::setCostLayer(const std::string& name,
                              std::shared_ptr<const nav::CostLayer> layer) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setCostLayer(name, std::move(layer));
}

void Navigation::setGround(std::function<float(glm::vec2)> ground) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setGround(std::move(ground));
}

void Navigation::setLaneGraph(const std::string& name,
                              std::shared_ptr<const nav::LaneGraph> graph) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setLaneGraph(name, std::move(graph));
}

void Navigation::setPath(const Entity& agent,
                         std::shared_ptr<const nav::AgentPath> path) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setPath(agent, std::move(path));
}

void Navigation::setPlanning(const Entity& agent, bool planning) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setPlanning(agent, planning);
}

void Navigation::setPriorityProvider(
    std::function<int(const Entity&)> priority) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setPriorityProvider(std::move(priority));
}

void Navigation::setRigProvider(
    std::function<nav::vehicle::Rig(const Entity&)> rig) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setRigProvider(std::move(rig));
}

void Navigation::setSpace(std::shared_ptr<const nav::Space> space) const
{
  if (NavigationSystem* navigation = backend())
    navigation->setSpace(std::move(space));
}

const nav::Fleet::Settings& Navigation::settings() const
{
  static const nav::Fleet::Settings defaults;
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().settings() : defaults;
}

bool Navigation::solid(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation && navigation->fleet().solid(idOf(agent));
}

const nav::AgentSpec* Navigation::spec(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().spec(idOf(agent)) : nullptr;
}

float Navigation::speed(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->fleet().speed(idOf(agent)) : 0.0f;
}

nav::Fleet::State Navigation::state(const Entity& agent) const
{
  const NavigationSystem* navigation = backend();
  return navigation ? navigation->state(agent) : nav::Fleet::State::Idle;
}

bool Navigation::takeWheel(const Entity& agent) const
{
  NavigationSystem* navigation = backend();
  return navigation && navigation->takeWheel(agent);
}

} // namespace sfs::facility
