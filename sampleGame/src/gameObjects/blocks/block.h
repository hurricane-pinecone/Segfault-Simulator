#pragma once

#include "engine/game/gameObject.h"
#include "glm/glm/ext/vector_float2.hpp"
#include <map>

class Block : public sfs::GameObject
{
public:
  enum class Shape
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
    std::map<std::pair<Shape, Direction>, BlockSpriteDef> variants;
  };

  Block(glm::vec2 position,
        int elevation,
        Shape shape = Shape::Full,
        Direction direction = Direction::North)
      : m_position(position), m_elevation(elevation), m_shape(shape),
        m_direction(direction)
  {
  }

protected:
  virtual const BlockSpriteDef& getVariant(Shape shape,
                                           Direction direction) const = 0;

protected:
  glm::vec2 m_position;
  int m_elevation;

  Shape m_shape;
  Direction m_direction;
};
