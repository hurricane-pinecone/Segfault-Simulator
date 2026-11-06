#pragma once

#include "engine/runtime/navigation/vehicle/vehicleModel.h"
#include "engine/runtime/navigation/vehicle/vehiclePath.h"

#include <span>
#include <vector>

namespace sfs::nav::vehicle
{

/**
 * Continuous-curvature steering: the ways a car-like vehicle gets from one
 * pose to another with its wheel turned no faster than it can be, so the path
 * is drivable exactly as laid (curvature continuous wherever it moves; at a
 * cusp, standing, the wheel may be turned). The textbook construction:
 *
 * - CC turns (Fraichard & Scheuer, "From Reeds and Shepp's to
 *   continuous-curvature paths", IEEE T-RO 2004): a clothoid from straight up
 *   to full lock, an arc, a clothoid back. Every CC turn from a pose starts
 *   and ends on one circle (centre Omega, radius r) at an angle mu to it, so
 *   the paths are Dubins / Reeds-Shepp words over those circles.
 * - HC turns at cusps (Banzhaf et al., "From C-Steer to HC-Steer", 2017):
 *   ending (or starting) at a cusp the turn keeps full lock, the wheel turned
 *   standing; such an end lies on the arc's own circle (radius 1/kappa).
 *
 * Words: CSC and CCC in one gear (forward, or backing up all the way), and
 * with cusps C|C|C (the three-point turn), C|CSC and CSC|C. Each comes back
 * as the Segments it is, ending exactly at the goal. Turns shorter than their
 * clothoids allow (a few degrees) aren't CC turns: those words are skipped,
 * the search covering what's left.
 */
class Steering
{
public:
  /** `kappa` the largest curvature, `sigma` the clothoids' sharpness. */
  Steering(float kappa, float sigma);

  float kappa() const { return m_kappa; }
  float sigma() const { return m_sigma; }
  /** A clothoid's length and the heading it turns through. */
  float clothoidLength() const { return m_kappa / m_sigma; }
  float clothoidTurn() const { return m_thetaC; }
  /** The CC circle: its radius and the angle mu a pose on it makes. */
  float ccRadius() const { return m_r; }
  float ccAngle() const { return m_mu; }

  /** Which words to try. */
  struct Words
  {
    bool forward = true;  // all-forward words
    bool reverse = true;  // all-backing words
    bool cusps = true;    // words with cusps (needs both gears)
    int cuspSamples = 10; // C|CSC: first turns tried (0: none)
    bool verify = false;  // integrate each and drop any that misses (tests)
  };
  /** Every word it knows from `from` to `to` (each ends there exactly). */
  std::vector<std::vector<Segment>>
  paths(Pose2 from, Pose2 to, const Words& words) const;

  /**
   * One CC turn of `deflection` radians (>= 0) in `sense` (+1 the motion
   * turning anticlockwise), in `gear`; `ccStart`/`ccEnd` false: that end at
   * full lock (a cusp's). Empty if it's too small for its clothoids.
   */
  bool turn(float deflection,
            int sense,
            int gear,
            bool ccStart,
            bool ccEnd,
            std::vector<Segment>& out) const;

private:
  struct Motion
  {
    glm::vec2 p;
    float d; // the direction it moves in
  };
  enum class End
  {
    Start, // a CC turn's start (its CC circle)
    End,   // a CC turn's end (its CC circle)
    Lock,  // a full-lock end, at a cusp (the arc's own circle)
  };
  glm::vec2 centre(Motion m, int sense, End end) const;
  void csc(Motion a,
           Motion b,
           int gear,
           std::vector<std::vector<Segment>>& out,
           bool ccStart = true) const;
  void ccc(Motion a,
           Motion b,
           int gear,
           std::vector<std::vector<Segment>>& out) const;
  void threePoint(Motion a,
                  Motion b,
                  int gear,
                  std::vector<std::vector<Segment>>& out) const;
  void cuspFirst(Motion a,
                 Motion b,
                 int gear,
                 int samples,
                 std::vector<std::vector<Segment>>& out) const;

  float m_kappa, m_sigma, m_thetaC, m_r, m_mu, m_R;
  std::vector<float> m_elementary; // (clothoid lengths by deflection)
};

/** Where a segment from `from` gets to after `s` metres of it. */
Pose2 advance(Pose2 from, const Segment& segment, float s);
/** Where segments from `from` end. */
Pose2 endOf(Pose2 from, std::span<const Segment> segments);
float lengthOf(std::span<const Segment> segments);
/** The same way driven the other way round (from its end to its start). */
std::vector<Segment> reversed(std::span<const Segment> segments);

/**
 * A trailer's new angle on the hitch after its tractor's reference point has
 * gone `ds` (signed: backing up below 0) on curvature `k`.
 */
float hitchStep(const Trailer& trailer, float hitch, float ds, float k);
/** The trailer's axle pose for the tractor at `pose`, at `hitch`. */
Pose2 trailerPose(const Trailer& trailer, Pose2 pose, float hitch);

/**
 * Sample `path.segments` from `path.start` every `step` metres into
 * `path.points` (positions, headings, curvature, gear, the trailer's angle),
 * a point at every segment's end too. Speeds are left for SpeedProfile.
 */
void sample(VehiclePath& path,
            float step,
            const Trailer* trailer = nullptr,
            int leg = 0);

} // namespace sfs::nav::vehicle
