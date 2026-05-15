#pragma once

#include "engine/game/gameObject.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <map>

enum class BlockShape
{
  Full,
  Half,
  Stairs,
  Slope,
  Wall
};

enum class Direction
{
  North,
  East,
  South,
  West
};

struct BlockSpriteDef
{
  std::string name;
  std::string path;
  SDL_Rect src;
};

struct BlockDef
{
  std::map<std::pair<BlockShape, Direction>, BlockSpriteDef> variants;
};

class Block : public sfs::GameObject
{
public:
  Block(glm::vec2 position,
        int elevation,
        BlockShape shape = BlockShape::Full,
        Direction direction = Direction::North)
      : m_position(position), m_elevation(elevation), m_shape(shape),
        m_direction(direction)
  {
  }

protected:
  virtual const BlockSpriteDef& getVariant(BlockShape shape,
                                           Direction direction) const = 0;

protected:
  glm::vec2 m_position;
  int m_elevation;

  BlockShape m_shape;
  Direction m_direction;
};
