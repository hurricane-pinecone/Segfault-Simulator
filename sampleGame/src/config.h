#pragma once

#include <string>

#ifdef ENGINE_WEB
inline const std::string ASSET_ROOT = "/assets/";
#else
inline const std::string ASSET_ROOT = "./assets/";
#endif

inline constexpr int WINDOW_WIDTH = 1600;
inline constexpr int WINDOW_HEIGHT = 1200;
inline constexpr float WORLD_SCALE = 2.0f;
