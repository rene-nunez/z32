// Native checks for the F6 wire format.
#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include <Net_state.h>

static int fails = 0;

static void check(bool ok, const char* what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++fails;
  }
}

static void checkf(bool ok, const char* fmt, ...) {
  if (!ok) {
    va_list ap;
    va_start(ap, fmt);
    printf("FAIL ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    ++fails;
  }
}

int main() {
  check(sizeof(Net::game_state_msg) == 146, "state len 146");
  check(Net::STATE_LEN == 146, "STATE_LEN 146");
  check(sizeof(Net::player_input_msg) == 5, "input len 5");
  check(sizeof(Net::game_state_msg) <= 250, "state under cap");

  Net::game_state_msg s;
  Net::player_input_msg in;
  check(((const uint8_t*)&s)[0] == 0x02, "state type byte");
  check(((const uint8_t*)&in)[0] == 0x10, "input type byte");

  // kills/wave ride u16; u8 would wrap 300 to 44
  s.kills = 300;
  s.wave = 300;
  check(s.kills == 300, "kills hold 300");
  check(s.wave == 300, "wave holds 300");

  s.guns[0] = 3;
  s.guns[1] = 5;
  s.dmg_lvl[0] = 2;
  s.dmg_lvl[1] = 5;
  s.spd_lvl[0] = 1;
  s.rpd_lvl[1] = 4;
  check(s.guns[0] == 3 && s.guns[1] == 5, "guns per player");
  check(s.dmg_lvl[0] == 2 && s.dmg_lvl[1] == 5, "dmg per player");
  check(s.spd_lvl[0] == 1 && s.rpd_lvl[1] == 4, "spd/rpd per player");

  s.ammo[0] = 15;
  s.ammo[1] = 0;
  check(s.ammo[0] == 15 && s.ammo[1] == 0, "ammo per player");
  // facing rides spare flag bits, so reload must not touch them
  for (uint8_t d = 0; d < 8; ++d) {
    const uint8_t f = Net::PF_ACTIVE | Net::PF_RELOADING | (uint8_t)(d << Net::PF_DIR_SHIFT);
    check((uint8_t)((f >> Net::PF_DIR_SHIFT) & Net::PF_DIR_MASK) == d, "player facing round-trip");
  }

  const float pxs[] = {0.0f, 1.3f, 159.9f, 480.0f, 959.75f};
  for (unsigned i = 0; i < sizeof(pxs) / sizeof(pxs[0]); ++i) {
    const float back = Net::uqpos(Net::qpos(pxs[i]));
    const float err = back > pxs[i] ? back - pxs[i] : pxs[i] - back;
    checkf(err <= 0.25f, "qpos round-trip %f", (double)pxs[i]);
  }
  check(Net::qpos(959.75f) < 65535u, "qpos fits u16 at world edge");

  for (int d = 0; d < 360; d += 7) {
    const float t = (float)d * 3.14159265f / 180.0f;
    float dx = cosf(t), dy = sinf(t);
    float ox, oy;
    Net::uqangle(Net::qangle(dx, dy), ox, oy);
    const float dot = dx * ox + dy * oy;
    checkf(dot > 0.999f, "qangle round-trip %d deg", d);
  }

  const float axs[] = {-1.0f, -0.5f, 0.0f, 0.33f, 1.0f};
  for (unsigned i = 0; i < sizeof(axs) / sizeof(axs[0]); ++i) {
    const float back = Net::uqaxis(Net::qaxis(axs[i]));
    const float err = back > axs[i] ? back - axs[i] : axs[i] - back;
    checkf(err <= 1.0f / 127.0f + 1e-6f, "qaxis round-trip %f", (double)axs[i]);
  }
  check(Net::qaxis(2.0f) == 127, "qaxis clamps high");
  check(Net::qaxis(-2.0f) == -127, "qaxis clamps low");

  // flag fields must not overlap: kinds/facing survive each other's bits
  for (uint8_t k = 0; k < 3; ++k) {
    for (uint8_t d = 0; d < 8; ++d) {
      const uint8_t f = Net::ZF_ACTIVE | (uint8_t)(k << Net::ZF_KIND_SHIFT) |
                        (uint8_t)(d << Net::ZF_DIR_SHIFT);
      check((uint8_t)((f >> Net::ZF_KIND_SHIFT) & 0x03) == k, "zombie kind bits");
      check((uint8_t)((f >> Net::ZF_DIR_SHIFT) & Net::ZF_DIR_MASK) == d, "zombie facing bits");
    }
  }
  for (uint8_t d = 0; d <= 16; ++d) {
    const uint8_t f = Net::BF_ACTIVE | (uint8_t)(d << Net::BF_DMG_SHIFT);
    check((uint8_t)((f >> Net::BF_DMG_SHIFT) & 0x1F) == d, "bullet dmg bits");
  }

  Net::chat_msg c;
  check(sizeof(Net::chat_msg) == 4, "chat len 4");
  check(((const uint8_t*)&c)[0] == 0x20, "chat type byte");
  check(sizeof(Net::chat_msg) <= 250, "chat under cap");
  check((uint8_t)Net::chat_id::help == 0 && (uint8_t)Net::chat_id::come == 1 &&
            (uint8_t)Net::chat_id::thanks == 2 && (uint8_t)Net::chat_id::ammo == 3,
        "chat wire ids unchanged");

  if (fails == 0) {
    printf("all net checks passed\n");
  }
  return fails != 0;
}
