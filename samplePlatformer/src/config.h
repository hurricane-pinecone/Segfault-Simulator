#pragma once

#include <string>

#ifdef ENGINE_WEB
inline const std::string ASSET_ROOT = "/assets/";
#else
inline const std::string ASSET_ROOT = "./assets/";
#endif

inline constexpr int WINDOW_WIDTH = 1600;
inline constexpr int WINDOW_HEIGHT = 1200;

// Side-on movement tuning, in world pixels (the flat world is 1 unit = 1 px).
inline constexpr float MOVE_SPEED = 320.0f;  // px/s horizontal
inline constexpr float JUMP_SPEED = 760.0f;  // px/s initial upward (−Y)
inline constexpr float GRAVITY = 2000.0f;    // px/s^2 downward (+Y)

// World extent (procedural level). Ground spans this; floating platforms are
// generated above it.
inline constexpr float WORLD_WIDTH = 4200.0f;
inline constexpr float GROUND_Y = 780.0f;
inline constexpr int PLATFORM_COUNT = 18;
inline constexpr float PLAYER_START_X = WORLD_WIDTH * 0.5f;
inline constexpr float PLAYER_START_Y = GROUND_Y - 320.0f;

// Combat tuning (world pixels). Bullets are fast, short-lived laser bolts.
inline constexpr float BULLET_SPEED = 1500.0f; // px/s
inline constexpr float BULLET_LIFE = 0.5f;     // seconds before it expires
inline constexpr float BULLET_HIT = 7.0f;      // hit-box half-edge (px)
inline constexpr float BULLET_DAMAGE = 12.0f;
inline constexpr float ENEMY_SPEED = 130.0f;   // chase px/s
inline constexpr float ENEMY_HEALTH = 30.0f;

// Constant respawn for testing.
inline constexpr double ENEMY_SPAWN_INTERVAL = 0.7; // seconds between spawns
inline constexpr int ENEMY_CAP = 28;                // max live enemies

// Combat feel.
inline constexpr double FIRE_INTERVAL = 0.08;    // seconds between auto-fire shots
inline constexpr float ENEMY_KNOCKBACK = 360.0f; // upward pop when hit (px/s)
inline constexpr float MUZZLE_FLASH_TIME = 0.05f;
inline constexpr float DEATH_FLASH_TIME = 0.18f;

// Screen shake.
inline constexpr float SHAKE_ON_KILL = 16.0f; // magnitude (px)
inline constexpr float SHAKE_DECAY = 70.0f;    // px/s
