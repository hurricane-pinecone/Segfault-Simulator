#pragma once

#include <glm/glm/ext/vector_float4.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace sfs
{

/**
 * One global shader parameter: the name shaders and materials know it by,
 * and its value (a number is its x).
 */
struct ShaderParameter
{
  bool operator==(const ShaderParameter&) const = default;

  /** Its name, as shaders and materials' Global Parameter nodes read it. */
  std::string name;
  /** Its value; a number is x. */
  glm::vec4 value{0.0f};
};

/**
 * The scene's global shader parameters, as Unreal's Material Parameter
 * Collections or Godot's global shader uniforms: named values every material
 * and shader can read, which a game sets (how wet a storm has left the
 * ground, a pulse every glowing sign shares). The renderer reads them each
 * frame; a parameter the scene hasn't got reads 0.
 *
 * The engine's own looks read these (each its x, 0..1):
 * - `wetness`: how wet surfaces are (darker, glossier, water standing on
 *   flat ground where the sky is open).
 * - `puddles`: how full a terrain's low spots are (TerrainMaterial::puddles).
 */
struct [[sfs::scene_data]] ShaderGlobals
{
  /** The parameters, by name (each name once). */
  std::vector<sfs::ShaderParameter> parameters;
};

/** How many global shader parameters a frame carries (its slots). */
inline constexpr std::size_t kMaxShaderGlobals = 16;

/** Every slot's value, as a frame carries them. */
using ShaderGlobalValues = std::array<glm::vec4, kMaxShaderGlobals>;

/** The engine's own parameters' slots (shaderGlobalSlot()). */
inline constexpr std::size_t kShaderGlobalWetness = 0;
inline constexpr std::size_t kShaderGlobalPuddles = 1;

/**
 * The slot a parameter's value has in every frame, the same for the whole
 * process, giving it one if it has none: the engine's own first (`wetness` 0,
 * `puddles` 1), others in the order a shader that reads them is first built
 * (a material graph's Global Parameter node). Safe from any thread.
 *
 * @param name the parameter's name
 * @return its slot; -1 for an empty name, or once all kMaxShaderGlobals
 *         are taken (logged once)
 */
int shaderGlobalSlot(std::string_view name);

/**
 * The slot a parameter has, without giving it one: what a frame fills. A name
 * no shader reads has none. Lock-free, safe from any thread.
 *
 * @param name the parameter's name
 * @return its slot, or -1 if it has none
 */
int findShaderGlobalSlot(std::string_view name);

/**
 * A parameter's value.
 *
 * @param globals the scene's parameters
 * @param name    the parameter's name
 * @return its value, or null if `globals` hasn't got it
 */
const glm::vec4* shaderGlobal(const ShaderGlobals& globals,
                              std::string_view name);

/**
 * Set a parameter's value, adding it if `globals` hasn't got it.
 *
 * @param globals the scene's parameters
 * @param name    the parameter's name
 * @param value   its value (a number: x)
 */
void setShaderGlobal(ShaderGlobals& globals,
                     std::string_view name,
                     const glm::vec4& value);

/**
 * Set a parameter to a number: its x, the rest 0 (see the other overload).
 *
 * @param globals the scene's parameters
 * @param name    the parameter's name
 * @param value   the number
 */
void setShaderGlobal(ShaderGlobals& globals,
                     std::string_view name,
                     float value);

/**
 * Fill `values` from the scene's parameters, each at its slot
 * (findShaderGlobalSlot(): a parameter no shader reads is left out); slots
 * none of them has read 0. No allocation, no lock.
 *
 * @param globals the scene's parameters (null: none)
 * @param values  every slot's value, filled
 */
void gatherShaderGlobals(const ShaderGlobals* globals,
                         ShaderGlobalValues& values);

} // namespace sfs
