#pragma once

#include <cstdint>

#include <net_state.h>
#include <map.h>

// The simulation: players, zombies, bullets, waves and points, in world px. It owns its
// state and never touches the screen, the menus or the network, so the renderer, the panel
// and the menus can only read it through view().
//
// F6 co-op: two players share one wallet; gun and buff levels are per player. Player 0 is local (host),
// player 1 is the net peer (inactive in Solo). Zombies chase their nearest alive player.
class sim {
  public:
    static constexpr uint8_t PLAYER_SIZE = 8;
    static constexpr uint8_t ZOMBIE_SIZE = 6;
    static constexpr uint8_t BULLET_SIZE = 4;
    static constexpr uint8_t MAX_ZOMBIES = 10;
    static constexpr uint8_t MAX_BULLETS = 8;
    static constexpr uint8_t NUM_PLAYERS = 2;
    static constexpr uint8_t PLAYER_HP_MAX = 10; // the panel draws one pip per point
    static constexpr uint8_t REVIVE_HP = 3;     // back on your feet at 3 HP, heal up after
    static constexpr uint8_t BLEED_SECS = 15;   // bleed-out window before death
    static constexpr uint8_t MAX_LVL = 10;      // damage and speed cap here, pips per level

    enum class weapon : uint8_t { pistol, smg, shotgun, rifle, m16, sniper };
    enum class actor_kind : uint8_t { normal, runner, boss };
    enum class event : uint8_t {
      none,
      shoot,
      buy_heal,
      buy_dmg,
      buy_spd,
      roulette,
      denied,
      hurt,
      wave,
      over,
      revive,
      buy_rpd, // appended last: earlier wire values never shift
      reload,  // mag swap started (manual top-up or auto on empty)
      reloaded, // mag full again, back in the fight
      empty // legacy dry click: auto-reload rescues now, never emitted
    };

    // shop: points are the spendable wallet. Heal is flat; damage and speed are
    // permanent levels on the character (like HP) and each level costs more.
    static constexpr uint32_t PRICE_HEAL = 100; // +2 HP
    static constexpr uint32_t PRICE_DMG = 150;  // damage +25%/level, base price
    static constexpr uint32_t PRICE_SPD = 150;  // speed +8%/level, base price
    static constexpr uint32_t PRICE_RPD = 150;  // rapid -6% cooldown/level, base price
    static constexpr uint32_t PRICE_ROLL = 100; // roulette: random weapon
    static constexpr uint32_t LVL_PRICE_STEP = 300; // extra cost per level owned

    // facing: 8-wind sector from dir_of(), 0=E 1=SE 2=S 3=SW 4=W 5=NW 6=N 7=NE
    // (+y is south on the glass). Render folds it onto the stored art + hflip.
    static uint8_t dir_of(float dx, float dy);
    // facing debounce: a new sector must repeat 2 frames (~66ms) to
    // apply, so border jitter never shows (noise never sticks,
    // real turns apply with no perceptible lag, held diagonals show)
    static void _face_toward(uint8_t& facing, uint8_t& want, uint8_t& cnt, float dx,
                             float dy);
    static constexpr uint8_t FACE_FRAMES = 2; // ~66ms: noise never shows, turns feel instant

    struct player_state {
      float x, y;
      uint8_t hp; // 0 = down or dead
      bool active; // false = no second player (Solo)
      bool downed; // bleeding out: revive, don't respawn
      uint8_t bleed; // seconds left while downed
      uint8_t facing; // 8-wind dir above, move-driven, aim overrides on fire
    };

    // remote control for player 1, fed by game from the net each frame. Fire is a
    // level for autos (MP9/AR-15) and an edge for the rest; game derives it from
    // the peer's bitmask accordingly. Interact/pause/reload are always edges.
    struct ctl {
      float jx, jy;
      bool fire;
      bool interact;
      bool pause;
      bool reload;
    };

    struct state {
      struct actor {
        float x, y;
        uint8_t hp;
        bool active;
        actor_kind kind;
        uint8_t facing; // 8-wind dir above, set by the steer
      };
      struct shot {
        float x, y, vx, vy;
        uint8_t dmg;
        bool active;
      };
      player_state players[NUM_PLAYERS];
      uint16_t wave, kills;
      uint32_t points; // shared wallet: both players spend from it
      weapon guns[NUM_PLAYERS]; // per-player loadout, P2 joins fresh (pistol/0)
      uint8_t dmg_lvl[NUM_PLAYERS]; // permanent damage levels, 0..MAX_LVL
      uint8_t spd_lvl[NUM_PLAYERS]; // permanent speed levels, 0..MAX_LVL
      uint8_t rpd_lvl[NUM_PLAYERS]; // permanent rapid levels, 0..MAX_LVL
      uint8_t ammo[NUM_PLAYERS]; // rounds left in the mag, refilled by reloads
      uint32_t reload_end[NUM_PLAYERS]; // millis() when the swap finishes, 0 = ready
      event last_event; // set by step() and by the buy calls below, read by game
      actor zombies[MAX_ZOMBIES];
      shot bullets[MAX_BULLETS];
    };

    // the only way in: a const ref, so nothing outside can move a zombie or spend a point.
    // The renderer and the panel read it every frame, and it is the whole net-sync payload.
    static const state& view() { return _s; }

    static void reset();
    static void set_p2_active(bool active); // Multi start on the host
    static void set_p2(const ctl& c);       // fresh peer input, every host frame
    static bool step(uint32_t now); // false once nobody is left standing
    static bool revive(uint8_t p);  // partner lift: downed back to 3 HP

    // net sync: host fills n (game stamps type+seq), client applies it wholesale.
    static void snapshot(net::game_state_msg& n);
    static void apply_snapshot(const net::game_state_msg& n);

    // shop, called by game on an INTERACT edge near a machine. Exact points pay:
    // points >= price succeeds. On denial last_event is denied. Heal lands on
    // player p, and p's own gun/levels benefit only p; the wallet is shared.
    static bool buy_heal(uint32_t now, uint8_t p = 0);
    static bool buy_damage(uint32_t now, uint8_t p = 0);
    static bool buy_speed(uint32_t now, uint8_t p = 0);
    static bool buy_rapid(uint32_t now, uint8_t p = 0);
    static bool roll_roulette(uint32_t now, uint8_t p = 0);
    static bool start_reload(uint32_t now, uint8_t p = 0); // mag swap, false if full/busy (manual + auto)
    static bool reloading(uint8_t p); // a swap is still running for p

    static uint32_t price_for(uint32_t base, uint8_t lvl); // base + STEP*lvl
    static bool auto_fire(weapon w); // hold-to-fire: MP9 + AR-15 only, rest is press-per-shot
    static uint8_t mag_size(weapon w) { return _mag_size(w); } // rounds per mag, for the HUD
    // display bonuses for the panel/prompts: DMG +25/lvl, SPD +8/lvl, ROF the real
    // accumulated cooldown cut in % (multiplicative, L10 = 46)
    static uint8_t dmg_bonus(uint8_t lvl);
    static uint8_t spd_bonus(uint8_t lvl);
    static uint8_t rpd_cut(uint8_t lvl);
    static const char* gun_name_p(uint8_t p = 0); // that player's gun
    static const char* gun_name(weapon w);
  private:
    static constexpr float player_speed = 110.0f;
    static constexpr float zombie_speed = 40.0f; // normal; runner 80, boss 30
    static constexpr float runner_speed = 80.0f;
    static constexpr float boss_speed = 30.0f;
    static constexpr uint32_t runner_reward = 15; // base, +2 per wave on top
    static constexpr uint16_t spawn_min_d2 = 100 * 100; // keep spawns >= 100px away
    static constexpr float bullet_speed = 320.0f;
    static constexpr float fire_range = 160.0f;
    static constexpr float sniper_range = 220.0f;
    static constexpr uint8_t burst_shots = 3; // M16: one press fires 3 rounds
    static constexpr uint32_t burst_gap_ms = 100; // spacing between burst rounds
    static constexpr float hit_dist = 5.0f;
    static constexpr float contact_dist = 9.0f;
    static constexpr float declump_dist = 7.0f; // zombie-zombie push-out: core 6px + 1px air
    static constexpr uint32_t damage_cd_ms = 400;

    // 8-neighbourhood, cardinals first: the zombie aims at the best neighbour's centre,
    // so a diagonal step reads as smooth drift instead of a tile-by-tile shuffle
    static constexpr int8_t nbr_x[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int8_t nbr_y[8] = {0, 0, 1, -1, 1, -1, 1, -1};

    // host-local runtime state, deliberately outside the view: the frame clock, the fire and
    // damage cooldowns and the tiles the BFS fields were last built for are not peer state
    static state _s;
    static uint32_t _last_ms, _last_damage[NUM_PLAYERS], _last_shot[NUM_PLAYERS];
    static uint8_t _burst_left[NUM_PLAYERS]; // M16 rounds still owed this burst
    static uint32_t _burst_next[NUM_PLAYERS]; // millis() when the next burst round fires
    static uint32_t _last_aim[NUM_PLAYERS]; // last fire time: aim holds facing briefly
    static constexpr uint32_t AIM_HOLD_MS = 500;
    static uint8_t _face_want[NUM_PLAYERS], _face_cnt[NUM_PLAYERS]; // player debounce
    static uint8_t _zface_want[MAX_ZOMBIES], _zface_cnt[MAX_ZOMBIES]; // zombie debounce
    static uint32_t _bleed_acc[NUM_PLAYERS]; // ms banked toward the next bleed tick
    static uint16_t _wave_quota;   // kills to clear this wave (wave+3, uncapped)
    static uint16_t _wave_spawned; // zombies spawned so far this wave (cap 10 alive)
    static uint32_t _wave_break_until; // millis() when the next wave may spawn, 0 = no break
    static constexpr uint32_t WAVE_BREAK_MS = 3000; // silent breather between waves (shop/heal/reload)
    static int16_t _path_tx[NUM_PLAYERS], _path_ty[NUM_PLAYERS];
    static ctl _p2ctl;

    static bool _alive(uint8_t p); // active, standing, shooting
    static bool _downed(uint8_t p); // active, bleeding out, needs a revive
    static void _move_entity(float& x, float& y, float dx, float dy, uint8_t size);
    static bool _step_zombie(uint8_t z, float ddx, float ddy, float dt);
    static void _zombie_steer(uint8_t z, float pcx, float pcy, float dt,
                              const uint16_t f[tilemap::ROWS][tilemap::COLS]);
    static void _spawn_wave();
    static void _respawn(uint8_t p);
    static void _do_fire(uint32_t now, uint8_t p);
    static bool _fire_single(uint32_t now, uint8_t p); // aim + spawn one round, true if it left
    static uint32_t _fire_cd(weapon w);
    static uint8_t _mag_size(weapon w); // rounds per mag: 15 / 30 / 8 / 30 / 30 / 10
    static uint32_t _reload_ms(weapon w); // swap time: 1s, sniper 2s, shotgun 1.5s
    static uint8_t _base_dmg(weapon w);
    static float _fire_range(weapon w); // auto-aim reach: sniper sees further
    static uint8_t _eff_dmg(uint8_t base, uint8_t lvl); // base*(1+0.25*lvl), half-up, min 1
    static float _spd_mult(uint8_t lvl);                // 1+0.08*lvl
    static float _rpd_mult(uint8_t lvl);                // 0.94^lvl, 0.54 at max
    static uint8_t _zombie_hp(actor_kind kind, uint16_t wave); // normal 2+w/2, runner 1+w/4, boss 20+w (capped 255)
    static uint8_t _zombie_dmg(actor_kind kind);        // boss 3, rest 1
    static float _zombie_speed(actor_kind kind);        // 40 / 80 / 30
    static uint32_t _kill_reward(actor_kind kind, uint16_t wave); // normal 10+2w, runner 15+2w, boss 150+10w
    static uint16_t _wave_total(uint16_t wave); // quota: wave+3, uncapped (cap is alive at once)
    static uint8_t _wave_runners(uint16_t wave, uint16_t total); // 0 on wave 1, else min(2w/3, half)
    static bool _wave_boss(uint16_t wave);     // every 5th wave steals slot 0
    static actor_kind _wave_kind(uint16_t idx, bool boss, uint8_t runners, uint16_t total); // kind of spawn idx, runners spread evenly
    static void _spawn_into(uint8_t slot, actor_kind kind); // random >=100px spawn into a slot
    // roulette odds over r = rand % 100: SMG 30, pistol 10, shotgun 25, rifle 12,
    // M16 13, sniper 10. SMG and shotgun hit more often; the pistol stays the booby prize.
    static weapon _roll_weapon(uint8_t r);
    static uint8_t _fire_one(uint32_t now, float dx, float dy, uint8_t dmg, uint8_t p);
};
