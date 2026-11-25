#pragma once

// Simulation: players, zombies, bullets, waves and points

#include <cstdint>

#include <Net_state.h>
#include <Map.h>

class Sim {
  public:
    static constexpr uint8_t PLAYER_SIZE = 8;
    static constexpr uint8_t ZOMBIE_SIZE = 6;
    static constexpr uint8_t BULLET_SIZE = 4;
    static constexpr uint8_t MAX_ZOMBIES = 10;
    static constexpr uint8_t MAX_BULLETS = 8;
    static constexpr uint8_t NUM_PLAYERS = 2;
    static constexpr uint8_t PLAYER_HP_MAX = 10;
    static constexpr uint8_t REVIVE_HP = 3;
    static constexpr uint8_t BLEED_SECS = 15;
    static constexpr uint8_t MAX_LVL = 10;

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
      buy_rpd,
      reload,
      reloaded
    };

    static constexpr uint32_t PRICE_HEAL = 100;
    static constexpr uint32_t PRICE_DMG = 150;
    static constexpr uint32_t PRICE_SPD = 150;
    static constexpr uint32_t PRICE_RPD = 150;
    static constexpr uint32_t PRICE_ROLL = 100;
    static constexpr uint32_t LVL_PRICE_STEP = 300;

    // Facing: 0=E 1=SE 2=S 3=SW 4=W 5=NW 6=N 7=NE; +y is south
    static uint8_t dir_of(float dx, float dy);
    static void face_toward(uint8_t& facing, uint8_t& want, uint8_t& cnt, float dx, float dy);
    static constexpr uint8_t FACE_FRAMES = 2;

    struct player_state {
      float x, y;
      uint8_t hp;
      bool active;
      bool downed;
      uint8_t bleed;
      uint8_t facing;
    };

    struct ctl {
      float jx, jy;
      bool fire;
      bool reload;
    };

    struct state {
      struct actor {
        float x, y;
        uint8_t hp;
        bool active;
        actor_kind kind;
        uint8_t facing;
      };
      struct shot {
        float x, y, vx, vy;
        uint8_t dmg;
        bool active;
      };
      player_state players[NUM_PLAYERS];
      uint16_t wave, kills;
      uint32_t points;
      weapon guns[NUM_PLAYERS];
      uint8_t dmg_lvl[NUM_PLAYERS];
      uint8_t spd_lvl[NUM_PLAYERS];
      uint8_t rpd_lvl[NUM_PLAYERS];
      uint8_t ammo[NUM_PLAYERS];
      uint32_t reload_end[NUM_PLAYERS];
      event last_event;
      actor zombies[MAX_ZOMBIES];
      shot bullets[MAX_BULLETS];
    };

    static const state& view() { return s; }

    static void reset();
    static void set_p2_active(bool active);
    static void set_p2(const ctl& c);
    static bool step(uint32_t now);
    static bool revive(uint8_t p);

    static void snapshot(Net::game_state_msg& n);
    static void apply_snapshot(const Net::game_state_msg& n);

    static bool buy_heal(uint32_t now, uint8_t p = 0);
    static bool buy_damage(uint32_t now, uint8_t p = 0);
    static bool buy_speed(uint32_t now, uint8_t p = 0);
    static bool buy_rapid(uint32_t now, uint8_t p = 0);
    static bool roll_roulette(uint32_t now, uint8_t p = 0);
    static bool start_reload(uint32_t now, uint8_t p = 0);
    static bool reloading(uint8_t p);

    static uint32_t price_for(uint32_t base, uint8_t lvl);
    static bool auto_fire(weapon w);
    static uint8_t dmg_bonus(uint8_t lvl);
    static uint8_t spd_bonus(uint8_t lvl);
    static uint8_t rpd_cut(uint8_t lvl);
    static const char* gun_name_p(uint8_t p = 0);
    static const char* gun_name(weapon w);
  private:
    static constexpr float player_speed = 110.0f;
    static constexpr float zombie_speed = 40.0f;
    static constexpr float runner_speed = 80.0f;
    static constexpr float boss_speed = 30.0f;
    static constexpr uint32_t runner_reward = 15;
    static constexpr uint16_t spawn_min_d2 = 100 * 100;
    static constexpr float bullet_speed = 320.0f;
    static constexpr float fire_range = 160.0f;
    static constexpr float sniper_range = 220.0f;
    static constexpr uint8_t burst_shots = 3;
    static constexpr uint32_t burst_gap_ms = 100;
    static constexpr float hit_dist = 5.0f;
    static constexpr float contact_dist = 9.0f;
    static constexpr float declump_dist = 7.0f;
    static constexpr uint32_t damage_cd_ms = 400;

    static constexpr int8_t nbr_x[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static constexpr int8_t nbr_y[8] = {0, 0, 1, -1, 1, -1, 1, -1};

    static state s;
    static uint32_t last_ms, last_damage[NUM_PLAYERS], last_shot[NUM_PLAYERS];
    static uint8_t burst_left[NUM_PLAYERS];
    static uint32_t burst_next[NUM_PLAYERS];
    static uint32_t last_aim[NUM_PLAYERS];
    static constexpr uint32_t AIM_HOLD_MS = 500;
    static uint8_t face_want[NUM_PLAYERS], face_cnt[NUM_PLAYERS];
    static uint8_t zface_want[MAX_ZOMBIES], zface_cnt[MAX_ZOMBIES];
    static uint32_t bleed_acc[NUM_PLAYERS];
    static uint16_t wave_quota;
    static uint16_t wave_spawned;
    static uint32_t wave_break_until;
    static constexpr uint32_t WAVE_BREAK_MS = 3000;
    static int16_t path_tx[NUM_PLAYERS], path_ty[NUM_PLAYERS];
    static ctl p2ctl;

    static bool alive(uint8_t p);
    static bool downed(uint8_t p);
    static void move_entity(float& x, float& y, float dx, float dy, uint8_t size);
    static bool step_zombie(uint8_t z, float ddx, float ddy, float dt);
    static void zombie_steer(uint8_t z, float pcx, float pcy, float dt, const uint16_t f[Tilemap::ROWS][Tilemap::COLS]);
    static void spawn_wave();
    static void respawn(uint8_t p);
    static void do_fire(uint32_t now, uint8_t p);
    static bool fire_single(uint32_t now, uint8_t p);
    static uint32_t fire_cd(weapon w);
    static uint8_t mag_size(weapon w);
    static uint32_t reload_ms(weapon w);
    static uint8_t base_dmg(weapon w);
    static float weapon_range(weapon w);
    static uint8_t eff_dmg(uint8_t base, uint8_t lvl);
    static float spd_mult(uint8_t lvl);
    static float rpd_mult(uint8_t lvl);
    static uint8_t zombie_hp(actor_kind kind, uint16_t wave);
    static uint8_t zombie_dmg(actor_kind kind);
    static float speed_for(actor_kind kind);
    static uint32_t kill_reward(actor_kind kind, uint16_t wave);
    static uint16_t wave_total(uint16_t wave);
    static uint8_t wave_runners(uint16_t wave, uint16_t total);
    static bool wave_boss(uint16_t wave);
    static actor_kind wave_kind(uint16_t idx, bool boss, uint8_t runners, uint16_t total);
    static void spawn_into(uint8_t slot, actor_kind kind);
    static weapon roll_weapon(uint8_t r);
    static uint8_t fire_one(uint32_t now, float dx, float dy, uint8_t dmg, uint8_t p);
};
