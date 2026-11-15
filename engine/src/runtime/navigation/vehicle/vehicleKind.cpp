#include "engine/runtime/navigation/vehicle/vehicleKind.h"

#include "engine/runtime/navigation/vehicle/follower.h"
#include "engine/runtime/navigation/vehicle/steering.h"
#include "engine/runtime/navigation/vehicle/vehicleSpec.h"

#include <algorithm>
#include <cmath>
#include <mutex>

namespace sfs::nav::vehicle
{

void applySpec(AgentSpec& agent, const VehicleSpec& spec, const Rig& rig)
{
  constexpr float kG = 9.81f, kDegrees = 3.14159265358979f / 180.0f;
  const float own = std::max(spec.mass, 1.0f);
  const float rigMass = own + std::max(rig.extraMass, 0.0f);
  const float grip = std::max(rig.grip, 0.01f);
  const float effort = std::clamp(rig.effort, 0.01f, 1.0f);
  const float steer = std::clamp(spec.maxSteerAngle, 1.0f, 80.0f) * kDegrees;

  agent.wheelbase =
      std::max(spec.wheelbase > 0.0f ? spec.wheelbase : rig.wheelbase, 0.05f);
  agent.minRadius = agent.wheelbase / std::tan(steer);
  agent.steerRate = std::max(spec.steerRate, 0.1f) * kDegrees;
  agent.topSpeed =
      std::clamp(spec.roadSpeed, 0.1f, std::max(spec.topSpeed, 0.1f));
  agent.reverseSpeed = std::max(spec.reverseSpeed, 0.05f);
  agent.mayReverse = spec.mayReverse;
  agent.shuttleSeconds = std::max(spec.shuttleSeconds, 0.0f);
  // The driven wheels carry the machine's own weight, not what it tows.
  const float traction = grip * kG * own;
  const float drive = 2.0f * spec.power * 1000.0f / agent.topSpeed;
  agent.accel = effort * std::min(drive, traction) / rigMass;
  agent.brake =
      effort * std::min(spec.brakeForce * 1000.0f, traction) / rigMass;
  agent.lateralAccel = effort * grip * kG;
}

namespace
{

std::vector<AgentPath::Sample> samplesOf(const VehiclePath& path)
{
  std::vector<AgentPath::Sample> out;
  out.reserve(path.points.size());
  for (const PathPoint& p : path.points)
    out.push_back({p.s, p.pose, p.hitch, p.time, p.gear});
  return out;
}

AgentPlan toPlan(const PlanResult& r)
{
  AgentPlan out;
  out.state = r.state;
  if (!r.path.empty())
    out.path = std::make_shared<VehicleAgentPath>(r.path);
  out.seconds = r.seconds;
  out.goal = r.goal;
  out.blockers = r.blockers;
  out.stats = r.stats;
  out.why = r.why;
  return out;
}

// Follower behind AgentMotion.
class VehicleMotion : public AgentMotion
{
public:
  explicit VehicleMotion(const AgentSpec& spec) { setSpec(spec); }
  void setSpec(const AgentSpec& spec) override
  {
    m_drive = {spec.shuttleSeconds, spec.accel, spec.brake};
    m_follower.setDrive(m_drive);
    m_hand.setModel(modelOf(spec)); // (the same model its paths are driven by)
  }

  void setPath(std::shared_ptr<const AgentPath> path,
               float speed,
               float fromS) override
  {
    m_path = std::move(path);
    const auto* v = dynamic_cast<const VehicleAgentPath*>(m_path.get());
    if (!v)
    {
      m_follower.setPath({}, 0.0f);
      return;
    }
    if (fromS > 1.0e-4f)
    {
      // (Its own copy of the rest: the follower drives from its start.)
      auto rest = std::make_shared<VehicleAgentPath>(tail(v->path, fromS));
      m_path = rest;
      v = rest.get();
    }
    m_follower.setPath(v->path, speed);
  }
  const AgentPath* path() const override { return m_path.get(); }
  Step step(float dt, float speedCap, float stopAt) override
  {
    if (m_handOn)
    {
      // (By hand: the controls, no path; the caller's cap still holds.)
      m_hand.step(m_controls, dt, speedCap);
      return {};
    }
    const Follower::State st = m_follower.step(dt, speedCap, stopAt);
    return {st.done, st.waiting, st.hardStopFrom};
  }
  float s() const override { return m_follower.s(); }
  float speed() const override
  {
    return m_handOn ? m_hand.speed() : m_follower.speed();
  }
  float stoppingDistance(float dt) const override
  {
    return Follower::stoppingDistance(std::abs(speed()), m_drive.brake, dt);
  }
  float curvature() const override
  {
    return m_handOn ? m_hand.curvature() : m_follower.curvature();
  }
  Pose2 pose() const override
  {
    return m_handOn ? m_hand.pose() : m_follower.pose();
  }
  float hitch() const override
  {
    return m_handOn ? m_hand.hitch() : m_follower.hitch();
  }
  std::size_t index() const override { return m_follower.index(); }

  bool takeWheel(Pose2 pose, float hitch, float speed) override
  {
    m_path = nullptr;
    m_follower.setPath({}, 0.0f);
    m_hand.start(pose, hitch, speed);
    m_controls = {};
    m_handOn = true;
    return true;
  }
  void setControls(const Controls& controls) override { m_controls = controls; }
  void handBack() override { m_handOn = false; }
  bool handDriven() const override { return m_handOn; }

private:
  std::shared_ptr<const AgentPath> m_path;
  Follower m_follower;
  Follower::Drive m_drive;
  HandDrive m_hand;
  Controls m_controls;
  bool m_handOn = false;
};

Footprint footprintOf(const Box3& b, Pose2 q)
{
  const glm::vec2 f = unit(q.heading), l{-f.y, f.x};
  return {q.position + f * (0.5f * (b.back + b.front)) + l * b.shift,
          {0.5f * (b.front - b.back), b.half},
          q.heading};
}

Trailer trailerOf(const AgentSpec::Towed& t)
{
  Trailer out;
  out.hitchBehind = t.hitchBehind;
  out.drawbar = t.drawbar;
  out.box = t.box;
  out.forwardLimit = t.forwardLimit;
  out.reverseLimit = t.reverseLimit;
  return out;
}

} // namespace

VehicleAgentPath::VehicleAgentPath(VehiclePath p) : path(std::move(p))
{
  samples = samplesOf(path);
}

VehicleModel modelOf(const AgentSpec& a)
{
  VehicleModel v;
  v.boxes = a.boxes;
  v.minRadius = a.minRadius;
  v.wheelbase = a.wheelbase;
  v.steerRate = a.steerRate;
  v.topSpeed = a.topSpeed;
  v.reverseSpeed = a.reverseSpeed;
  v.accel = a.accel;
  v.brake = a.brake;
  v.lateralAccel = a.lateralAccel;
  v.shuttleSeconds = a.shuttleSeconds;
  v.mayReverse = a.mayReverse;
  if (a.trailer)
    v.trailer = trailerOf(*a.trailer);
  for (const AgentSpec::Mode& m : a.modes)
    v.modes.push_back({m.boxes, m.minRadius, m.forwardOnly});
  return v;
}

PlanRequest requestOf(const AgentSpec& a, const PlanAsk& ask)
{
  PlanRequest q;
  q.vehicle = modelOf(a);
  q.search = a.search.empty() ? std::string("hybridAStar") : a.search;
  q.mode = ask.mode;
  q.space = ask.world.space;
  q.cost = ask.world.cost;
  q.graph = ask.world.graph;
  q.from = ask.from;
  q.hitch = ask.hitch;
  for (const AgentGoal& g : ask.goals)
  {
    Goal goal;
    goal.pose = g.pose;
    goal.hitch = g.hitch;
    goal.arriveGear = g.arriveGear;
    q.goals.push_back(goal);
    q.speedCap = std::min(q.speedCap, g.speedCap);
  }
  q.goalTest = ask.goalTest;
  q.nodeBudget = ask.nodeBudget;
  q.budgetCap = ask.budgetCap;
  q.cancel = ask.cancel;
  return q;
}

VehiclePath tail(const VehiclePath& path, float fromS)
{
  VehiclePath out;
  const std::vector<PathPoint>& p = path.points;
  if (p.empty())
    return out;
  std::size_t i = 0;
  while (i + 1 < p.size() && p[i + 1].s <= fromS)
    ++i;
  const float s0 = p[i].s, t0 = p[i].time;
  for (std::size_t k = i; k < p.size(); ++k)
  {
    PathPoint q = p[k];
    q.s -= s0;
    q.time -= t0;
    out.points.push_back(q);
  }
  out.start = out.points.front().pose;
  out.startHitch = out.points.front().hitch;
  // (The segments from there: the first one cut where it starts.)
  float at = 0.0f;
  for (const Segment& g : path.segments)
  {
    const float end = at + g.length;
    if (end > s0 + 1.0e-4f)
    {
      Segment cut = g;
      if (at < s0)
      {
        const float u = g.length > 0.0f ? (s0 - at) / g.length : 0.0f;
        cut.k0 = g.k0 + u * (g.k1 - g.k0);
        cut.length = end - s0;
      }
      out.segments.push_back(cut);
    }
    at = end;
  }
  return out;
}

AgentPlan
VehicleKind::plan(const AgentSpec& agent,
                  const PlanAsk& ask,
                  const std::function<void(const AgentPlan&)>& onPartial) const
{
  const PlanRequest q = requestOf(agent, ask);
  std::function<void(const PlanResult&)> partial;
  if (onPartial)
    partial = [&](const PlanResult& r) { onPartial(toPlan(r)); };
  return toPlan(planNow(q, partial));
}

std::unique_ptr<AgentMotion>
VehicleKind::makeFollower(const AgentSpec& agent) const
{
  return std::make_unique<VehicleMotion>(agent);
}

std::vector<Footprint>
VehicleKind::footprint(const AgentSpec& agent, Pose2 pose, float hitch) const
{
  std::vector<Footprint> out;
  for (const Box3& b : agent.boxes)
    out.push_back(footprintOf(b, pose));
  if (agent.trailer)
    out.push_back(
        footprintOf(agent.trailer->box,
                    trailerPose(trailerOf(*agent.trailer), pose, hitch)));
  return out;
}

std::optional<Pose2>
VehicleKind::towedPose(const AgentSpec& agent, Pose2 pose, float hitch) const
{
  if (!agent.trailer)
    return std::nullopt;
  return trailerPose(trailerOf(*agent.trailer), pose, hitch);
}

void addKind()
{
  static std::once_flag once;
  std::call_once(
      once,
      []
      { AgentKinds::add("vehicle", std::make_shared<const VehicleKind>()); });
}

} // namespace sfs::nav::vehicle
