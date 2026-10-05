#pragma once

// Net wire format for F6 co-op. Deliberately free of Arduino/ESP-NOW includes so the
// native bank (test/test_native) can compile it with plain g++: structs, sizes and the
// quantizers are all checked there. The firmware mapping sim<->net lives in sim
// (snapshot/apply_snapshot); game only moves these bytes.
//
// Layout (little-endian, same core both ends):
//   game_state: type(1) + seq(2) + players 2x7 + meta 21 + zombies 8x6 + bullets 8x6 = 134B
//   player_input: type(1) + jx(1) + jy(1) + buttons(1) + seq(1) = 5B
// Both fit the 250 bytes/msg cap with room to spare.
#include <cmath>
#include <cstdint>

namespace net {
  constexpr uint8_t TYPE_STATE = 0x02; // == msg_type::game_state, asserted in Protocol.h
  constexpr uint8_t TYPE_INPUT = 0x10; // == msg_type::player_input, asserted in Protocol.h
  constexpr uint8_t TYPE_CHAT = 0x20; // == msg_type::chat, asserted in Protocol.h

  constexpr size_t STATE_LEN = 134;
  constexpr size_t INPUT_LEN = 5;
  constexpr size_t MAX_MSG = 250;

  // screen mirror for the co-op pause menu: the host owns it, the client only paints it.
  // game-owned bytes: sim::snapshot leaves them alone, game sets them per broadcast.
  constexpr uint8_t SCREEN_PLAYING = 0;
  constexpr uint8_t SCREEN_PAUSE = 1;
  constexpr uint8_t SCREEN_OVER = 2;

  // button bitmask for player_input (levels; host derives edges)
  constexpr uint8_t fire_bit = 0x01;
  constexpr uint8_t reload_bit = 0x02;
  constexpr uint8_t interact_bit = 0x04;
  constexpr uint8_t pause_bit = 0x08;

  // zombie/bullet flags packing
  constexpr uint8_t ZF_ACTIVE = 0x01;
  constexpr uint8_t ZF_KIND_SHIFT = 1; // 2 bits: actor_kind as u8
  constexpr uint8_t ZF_DIR_SHIFT = 3; // 3 bits: sim 8-wind facing (bits 3..5)
  constexpr uint8_t ZF_DIR_MASK = 0x07;
  constexpr uint8_t BF_ACTIVE = 0x01;
  constexpr uint8_t BF_DMG_SHIFT = 1; // 5 bits: bullet dmg (bits 1..5, 6..7 reserved)

  constexpr uint8_t PF_ACTIVE = 0x01;
  constexpr uint8_t PF_DOWNED = 0x02; // bleeding out, needs a revive
  constexpr uint8_t PF_RELOADING = 0x20; // mid mag swap (bit 5: dir owns bits 2..4)
  constexpr uint8_t PF_DIR_SHIFT = 2; // 3 bits: sim 8-wind facing (bits 2..4)
  constexpr uint8_t PF_DIR_MASK = 0x07;

  struct __attribute__((packed)) net_player {
    uint16_t x, y; // world px * 4 (0.25px steps; 960*4 < 2^16)
    uint8_t hp;
    uint8_t flags; // PF_ACTIVE | PF_DOWNED
    uint8_t bleed; // seconds left while downed
  };

  struct __attribute__((packed)) net_zombie {
    uint16_t x, y; // world px * 4
    uint8_t hp;
    uint8_t flags; // ZF_ACTIVE | kind << ZF_KIND_SHIFT
  };

  struct __attribute__((packed)) net_bullet {
    uint16_t x, y; // world px * 4
    uint8_t angle; // atan2 heading, 0..255
    uint8_t flags; // BF_ACTIVE | dmg << BF_DMG_SHIFT
  };

  struct __attribute__((packed)) game_state_msg {
    uint8_t type = TYPE_STATE;
    uint16_t seq = 0;
    net_player players[2];
    uint16_t wave = 0;
    uint16_t kills = 0;
    uint32_t points = 0; // shared wallet
    uint8_t guns[2] = {0, 0};   // sim::weapon as u8, per player
    uint8_t dmg_lvl[2] = {0, 0};
    uint8_t spd_lvl[2] = {0, 0};
    uint8_t rpd_lvl[2] = {0, 0}; // rapid (fire-rate) levels, 0..MAX_LVL
    uint8_t ammo[2] = {0, 0}; // rounds left in the mag, per player
    uint8_t event = 0; // sim::event as u8
    uint8_t screen = 0; // SCREEN_* (game-owned menu mirror)
    uint8_t sel = 0;    // pause cursor (game-owned, host drives it)
    net_zombie zombies[8];
    net_bullet bullets[8];
  };

  struct __attribute__((packed)) player_input_msg {
    uint8_t type = TYPE_INPUT;
    int8_t jx = 0; // -127..127
    int8_t jy = 0;
    uint8_t buttons = 0; // BTN_* bitmask, levels (host derives edges)
    uint8_t seq = 0;
  };

  // co-op callouts (SAVE ME / THX): 4B edge-triggered shouts, one code
  // path both directions. THX derives locally from the shared sim state
  // (zero bytes); only the voluntary SAVE ME travels (still the come wire
  // id, no protocol change). Solo never sends.
  enum class chat_id : uint8_t { help, come, thanks, ammo };

  struct __attribute__((packed)) chat_msg {
    uint8_t type = TYPE_CHAT;
    uint8_t from = 0; // player index 0/1 (P1/P2)
    uint8_t id = 0;   // chat_id as u8
    uint8_t seq = 0;
  };

  // --- quantizers (shared by firmware + native test) ---
  inline uint16_t qpos(float p) {
    return (uint16_t)(p * 4.0f + 0.5f);
  }

  inline float uqpos(uint16_t q) {
    return (float)q / 4.0f;
  }

  inline uint8_t qangle(float dx, float dy) {
    // atan2 in (-pi, pi] -> 0..255; +pi wraps to 0 (-pi == +pi on decode)
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
} // namespace net

static_assert(sizeof(net::game_state_msg) == net::STATE_LEN, "game_state must stay 134B");
static_assert(sizeof(net::player_input_msg) == net::INPUT_LEN, "player_input must stay 5B");
static_assert(net::STATE_LEN <= net::MAX_MSG, "game_state exceeds the 250B cap");
static_assert(sizeof(net::chat_msg) == 4, "chat must stay 4B");
static_assert(sizeof(net::chat_msg) <= net::MAX_MSG, "chat exceeds the 250B cap");
