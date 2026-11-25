#pragma once

// Packed co-op wire structs; Arduino-free for native tests

#include <cmath>
#include <cstdint>

namespace Net {
  // Wire sizes: game_state 146B, player_input 5B, chat 4B; cap 250B
  constexpr uint8_t TYPE_STATE = 0x02;
  constexpr uint8_t TYPE_INPUT = 0x10;
  constexpr uint8_t TYPE_CHAT = 0x20;

  constexpr size_t STATE_LEN = 146;
  constexpr size_t INPUT_LEN = 5;
  constexpr size_t MAX_MSG = 250;

  constexpr uint8_t SCREEN_PLAYING = 0;
  constexpr uint8_t SCREEN_PAUSE = 1;
  constexpr uint8_t SCREEN_OVER = 2;

  constexpr uint8_t fire_bit = 0x01;
  constexpr uint8_t reload_bit = 0x02;
  constexpr uint8_t interact_bit = 0x04;
  constexpr uint8_t pause_bit = 0x08;

  constexpr uint8_t ZF_ACTIVE = 0x01;
  constexpr uint8_t ZF_KIND_SHIFT = 1;
  constexpr uint8_t ZF_DIR_SHIFT = 3;
  constexpr uint8_t ZF_DIR_MASK = 0x07;
  constexpr uint8_t BF_ACTIVE = 0x01;
  constexpr uint8_t BF_DMG_SHIFT = 1;

  constexpr uint8_t PF_ACTIVE = 0x01;
  constexpr uint8_t PF_DOWNED = 0x02;
  constexpr uint8_t PF_RELOADING = 0x20;
  constexpr uint8_t PF_DIR_SHIFT = 2;
  constexpr uint8_t PF_DIR_MASK = 0x07;

  struct __attribute__((packed)) net_player {
    uint16_t x, y;
    uint8_t hp;
    uint8_t flags;
    uint8_t bleed;
  };

  struct __attribute__((packed)) net_zombie {
    uint16_t x, y;
    uint8_t hp;
    uint8_t flags;
  };

  struct __attribute__((packed)) net_bullet {
    uint16_t x, y;
    uint8_t angle;
    uint8_t flags;
  };

  struct __attribute__((packed)) game_state_msg {
    uint8_t type = TYPE_STATE;
    uint16_t seq = 0;
    net_player players[2];
    uint16_t wave = 0;
    uint16_t kills = 0;
    uint32_t points = 0;
    uint8_t guns[2] = {0, 0};
    uint8_t dmg_lvl[2] = {0, 0};
    uint8_t spd_lvl[2] = {0, 0};
    uint8_t rpd_lvl[2] = {0, 0};
    uint8_t ammo[2] = {0, 0};
    uint8_t event = 0;
    uint8_t screen = 0;
    uint8_t sel = 0;
    net_zombie zombies[10];
    net_bullet bullets[8];
  };

  struct __attribute__((packed)) player_input_msg {
    uint8_t type = TYPE_INPUT;
    int8_t jx = 0;
    int8_t jy = 0;
    uint8_t buttons = 0;
    uint8_t seq = 0;
  };

  enum class chat_id : uint8_t { help, come, thanks, ammo };

  struct __attribute__((packed)) chat_msg {
    uint8_t type = TYPE_CHAT;
    uint8_t from = 0;
    uint8_t id = 0;
    uint8_t seq = 0;
  };

  inline uint16_t qpos(float p) {
    return (uint16_t)(p * 4.0f + 0.5f);
  }

  inline float uqpos(uint16_t q) {
    return (float)q / 4.0f;
  }

  inline uint8_t qangle(float dx, float dy) {
    const float t = atan2f(dy, dx) * 128.0f / 3.14159265f + 128.0f;
    return (uint8_t)((int)(t + 0.5f) & 0xFF);
  }

  inline void uqangle(uint8_t a, float& dx, float& dy) {
    const float t = ((float)a - 128.0f) * 3.14159265f / 128.0f;
    dx = cosf(t);
    dy = sinf(t);
  }

  inline int8_t qaxis(float v) {
    if (v > 1.0f) {
      v = 1.0f;
    } else if (v < -1.0f) {
      v = -1.0f;
    }
    return (int8_t)(v * 127.0f);
  }

  inline float uqaxis(int8_t q) {
    return (float)q / 127.0f;
  }
}

static_assert(sizeof(Net::game_state_msg) == Net::STATE_LEN, "game_state must stay 146B");
static_assert(sizeof(Net::player_input_msg) == Net::INPUT_LEN, "player_input must stay 5B");
static_assert(Net::STATE_LEN <= Net::MAX_MSG, "game_state exceeds the 250B cap");
static_assert(sizeof(Net::chat_msg) == 4, "chat must stay 4B");
static_assert(sizeof(Net::chat_msg) <= Net::MAX_MSG, "chat exceeds the 250B cap");
