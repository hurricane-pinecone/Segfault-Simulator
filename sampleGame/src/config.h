#pragma once

#include <string>

#ifdef ENGINE_WEB
inline const std::string ASSET_ROOT = "/assets/";
#else
inline const std::string ASSET_ROOT = "./assets/";
#endif
