#include "engine/core/rendering/contactShadows.h"

namespace sfs
{

namespace
{

void read(const nlohmann::json& json, const char* key, float& out)
{
  if (const auto it = json.find(key); it != json.end() && it->is_number())
    out = it->get<float>();
}

} // namespace

nlohmann::json ContactShadowSettings::toJson() const
{
  return {{"enabled", enabled},
          {"length", length},
          {"thickness", thickness},
          {"strength", strength}};
}

ContactShadowSettings
ContactShadowSettings::fromJson(const nlohmann::json& json,
                                const ContactShadowSettings& defaults)
{
  ContactShadowSettings out = defaults;
  if (!json.is_object())
    return out;
  if (const auto it = json.find("enabled");
      it != json.end() && it->is_boolean())
    out.enabled = it->get<bool>();
  read(json, "length", out.length);
  read(json, "thickness", out.thickness);
  read(json, "strength", out.strength);
  return out;
}

ContactShadowSettings
ContactShadowSettings::fromJson(const nlohmann::json& json)
{
  return fromJson(json, ContactShadowSettings());
}

} // namespace sfs
