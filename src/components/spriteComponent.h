#pragma once

#include <string>

struct SpriteComponent
{
  int width;
  int height;
  std::string path;

  SpriteComponent(std::string path, int width, int height)
  {
    this->path = path;
    this->width = width;
    this->height = height;
  }
};
