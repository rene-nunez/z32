// Native bank for the F6 net wire format. Builds without Arduino: run
// test/test_native/run.sh. Checks sizes against the 250B cap and the
// quantizer round-trips (positions, angles, axes, flag packing).
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <NetState.h>

static int fails = 0;
static void check(bool ok, const char* what) {
  if (!ok) {
    printf("FAIL %s\n", what);
    ++fails;
  }
}

int main() {
  check(sizeof(net::game_state_msg) == 134, "state len 134");
  check(net::STATE_LEN == 134, "STATE_LEN 134");
  check(sizeof(net::player_input_msg) == 5, "input len 5");
  check(sizeof(net::game_state_msg) <= 250, "state under cap");

  // type byte first, so handlers dispatch from data[0]
  net::game_state_msg s;
  net::player_input_msg in;
  check(((const uint8_t*)&s)[0] == 0x02, "state type byte");
  check(((const uint8_t*)&in)[0] == 0x10, "input type byte");

  // kills/wave ride u16: 300 must survive the wire (u8 wrapped it to 44)
  s.kills = 300;
  s.wave = 300;
  check(s.kills == 300, "kills hold 300");
  check(s.wave == 300, "wave holds 300");

  // per-player builds: guns + levels ride arrays, wallet stays shared
  s.guns[0] = 3;
  s.guns[1] = 5;
  s.dmg_lvl[0] = 2;
  s.dmg_lvl[1] = 5;
  s.spd_lvl[0] = 1;
  s.rpd_lvl[1] = 4;
  check(s.guns[0] == 3 && s.guns[1] == 5, "guns per player");
  check(s.dmg_lvl[0] == 2 && s.dmg_lvl[1] == 5, "dmg per player");
  check(s.spd_lvl[0] == 1 && s.rpd_lvl[1] == 4, "spd/rpd per player");

  // mags ride u8 per player, reload flag packs into the player flags
  s.ammo[0] = 15;
  s.ammo[1] = 0;
  check(s.ammo[0] == 15 && s.ammo[1] == 0, "ammo per player");
  const uint8_t pf = net::PF_ACTIVE | net::PF_RELOADING;
  check((pf & net::PF_RELOADING) != 0, "reloading bit");
  check((uint8_t)((pf >> net::PF_DIR_SHIFT) & net::PF_DIR_MASK) == 0, "dir bits survive reload bit");

  // positions: world 0..960 x 0..480, error under one quantum (0.25px)
  const float pxs[] = {0.0f, 1.3f, 159.9f, 480.0f, 959.75f};
  for (unsigned i = 0; i < sizeof(pxs) / sizeof(pxs[0]); ++i) {
    const float back = net::uqpos(net::qpos(pxs[i]));
    float err = back - pxs[i];
    if (err < 0) {
      err = -err;
    }
    char what[48];
    snprintf(what, sizeof(what), "qpos round-trip %f", (double)pxs[i]);
    check(err <= 0.25f, what);
  }
  check(net::qpos(959.75f) < 65535u, "qpos fits u16 at world edge");

  // angles: sweep the circle, decoded heading must stay within one step
  for (int d = 0; d < 360; d += 7) {
    const float t = (float)d * 3.14159265f / 180.0f;
    float dx = cosf(t), dy = sinf(t);
    float ox, oy;
    net::uqangle(net::qangle(dx, dy), ox, oy);
    const float dot = dx * ox + dy * oy; // 1.0 = exact
    char what[48];
    snprintf(what, sizeof(what), "qangle round-trip %d deg", d);
    check(dot > 0.999f, what);
  }

  // axes: -1..1 maps to -127..127 and back within one step
  const float axs[] = {-1.0f, -0.5f, 0.0f, 0.33f, 1.0f};
  for (unsigned i = 0; i < sizeof(axs) / sizeof(axs[0]); ++i) {
    const float back = net::uqaxis(net::qaxis(axs[i]));
    float err = back - axs[i];
    if (err < 0) {
      err = -err;
    }
    char what[48];
    snprintf(what, sizeof(what), "qaxis round-trip %f", (double)axs[i]);
    check(err <= 1.0f / 127.0f + 1e-6f, what);
  }
  check(net::qaxis(2.0f) == 127, "qaxis clamps high");
  check(net::qaxis(-2.0f) == -127, "qaxis clamps low");

  // flag packing: kinds 0..2, dmgs 0..16 survive the shifts
  for (uint8_t k = 0; k < 3; ++k) {
    const uint8_t f = net::ZF_ACTIVE | (uint8_t)(k << net::ZF_KIND_SHIFT);
    check((f & net::ZF_ACTIVE) != 0, "zombie active bit");
    check((uint8_t)((f >> net::ZF_KIND_SHIFT) & 0x03) == k, "zombie kind bits");
  }
  for (uint8_t d = 0; d <= 16; ++d) {
    const uint8_t f = net::BF_ACTIVE | (uint8_t)(d << net::BF_DMG_SHIFT);
    check((uint8_t)((f >> net::BF_DMG_SHIFT) & 0x1F) == d, "bullet dmg bits");
  }

  if (fails == 0) {
    printf("all net checks passed\n");
  }
  return fails != 0;
}
