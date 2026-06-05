#include "engine/core/scripting/particleSchema.h"

#include "engine/core/particles/particleEffect.h"

namespace sfs
{

const LuaSchema& particleEffectSchema()
{
  static const LuaSchema schema = {
      field(
          "burst", &ParticleEffectDesc::burstCount, "int: particles per blast"),
      field("spread",
            &ParticleEffectDesc::directionSpread,
            "radians: launch cone width"),
      field(
          "gravityZ", &ParticleEffectDesc::gravityZ, "number: vertical accel"),
      field("drag", &ParticleEffectDesc::drag, "number: velocity damping"),
      rangeField("size", &ParticleEffectDesc::size, "tiles (min/max)"),
      rangeField("speed", &ParticleEffectDesc::speed, "tiles/sec (min/max)"),
      rangeField(
          "lifetime", &ParticleEffectDesc::lifetime, "seconds (min/max)"),
      colorField("color", "sfs.colors value (or {r,g,b})"),
  };
  return schema;
}

} // namespace sfs
