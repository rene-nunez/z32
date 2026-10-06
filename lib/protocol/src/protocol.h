#pragma once

#include <cstdint>

#include <network.h>
#include "net_state.h"

// handlers can dispatch from data[0] the `type` without parsing the rest
enum class msg_type : uint8_t {
  heartbeat = 0x01,
  game_state = 0x02,
  player_input = 0x10,
  chat = 0x20,
};

static_assert((uint8_t)msg_type::game_state == net::TYPE_STATE, "net/game_state type mismatch");
static_assert((uint8_t)msg_type::player_input == net::TYPE_INPUT, "net/player_input type mismatch");
static_assert((uint8_t)msg_type::chat == net::TYPE_CHAT, "net/chat type mismatch");

// payload of a heartbeat is 8 bytes total
struct __attribute__((packed)) heartbeat_msg {
  msg_type type = msg_type::heartbeat;
  uint32_t tick = 0;
  uint8_t role = ROLE_HOST;
  uint16_t reserved = 0;
};
