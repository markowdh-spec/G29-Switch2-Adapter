/*
 * Logitech G29 host driver for OGX-Mini (G29-Switch2-Adapter)
 * Copyright (c) 2026 markowdh-spec
 * SPDX-License-Identifier: MIT
 */
#include <cstring>
#include <tuple>

#include "tusb.h"
#include "host/usbh.h"
#include "class/hid/hid_host.h"

#include "Board/ogxm_log.h"
#include "Board/board_api.h"
#include "TaskQueue/TaskQueue.h"
#include "Board/board_api_private/board_api_private.h"

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#ifndef G29_SWITCH_TO_NATIVE
#define G29_SWITCH_TO_NATIVE 0
#endif

#ifndef G29_RAW_DUMP
#define G29_RAW_DUMP 0
#endif

#ifndef G29_DIAGNOSTICS
#define G29_DIAGNOSTICS 0
#endif
#include "USBHost/HostDriver/LogitechG29/LogitechG29.h"

using namespace LogitechG29;

namespace {

/* Documented Logitech wheel commands (7-byte output reports, no report ID). */
[[maybe_unused]] constexpr uint8_t CMD_SWITCH_TO_G29[CMD_LEN]        = { 0xF8, 0x09, 0x05, 0x01, 0x01, 0x00, 0x00 };
constexpr uint8_t CMD_SWITCH_TO_G923[CMD_LEN]       = { 0xF8, 0x09, 0x07, 0x01, 0x01, 0x00, 0x00 };
constexpr uint8_t CMD_AUTOCENTER_ACTIVATE[CMD_LEN]  = { 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
constexpr uint8_t CMD_AUTOCENTER_OFF[CMD_LEN]       = { 0xF5, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

/* Spring-to-centre parameters for a 0..0xFFFF strength (same scaling the Linux lg4ff driver uses). */
void make_autocenter_cmd(uint16_t magnitude, uint8_t (&out)[CMD_LEN])
{
    uint32_t a, b;
    if (magnitude <= 0xAAAA) {
        a = 0x0Cu * magnitude;
        b = 0x80u * magnitude;
    } else {
        a = (0x0Cu * 0xAAAA) + 0x06u * (magnitude - 0xAAAA);
        b = (0x80u * 0xAAAA) + 0xFFu * (magnitude - 0xAAAA);
    }
    a >>= 1;
    out[0] = 0xFE;
    out[1] = 0x0D;
    out[2] = static_cast<uint8_t>(a / 0xAAAA);
    out[3] = static_cast<uint8_t>(a / 0xAAAA);
    out[4] = static_cast<uint8_t>(b / 0xAAAA);
    out[5] = 0x00;
    out[6] = 0x00;
}

/* ---- Persistent settings: append-only log of 16-byte records in one flash sector
 * just below OGX's own NVS area. Each save programs one record (page program, ~1 ms);
 * the sector is only erased when all 256 slots are used. ---- */
constexpr uint32_t CFG_MAGIC    = 0x43393247u; // "G29C"
constexpr uint32_t CFG_SLOT     = 16;
constexpr uint32_t CFG_SLOTS    = FLASH_SECTOR_SIZE / CFG_SLOT;
constexpr uint32_t CFG_OFFSET   = PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE * (NVS_SECTORS + 1);

struct StoredCfg {
    uint32_t magic;
    uint8_t  version;
    uint8_t  anti_dz;
    uint8_t  spring;
    uint8_t  rumble;
    uint8_t  steer_idx;
    uint8_t  reserved[6];
    uint8_t  check;
};
static_assert(sizeof(StoredCfg) == CFG_SLOT, "StoredCfg must be one slot");

uint8_t cfg_check(const StoredCfg& c)
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&c);
    uint8_t s = 0xA5;
    for (uint32_t i = 0; i + 1 < sizeof(StoredCfg); ++i) s = static_cast<uint8_t>((s << 1 | s >> 7) ^ p[i]);
    return s;
}

const StoredCfg* cfg_slot(uint32_t i)
{
    return reinterpret_cast<const StoredCfg*>(XIP_BASE + CFG_OFFSET + i * CFG_SLOT);
}

/** Returns the newest valid record (or nullptr) and the first free slot (CFG_SLOTS = full). */
const StoredCfg* cfg_find(uint32_t& free_slot)
{
    const StoredCfg* last = nullptr;
    free_slot = CFG_SLOTS;
    for (uint32_t i = 0; i < CFG_SLOTS; ++i) {
        const StoredCfg* c = cfg_slot(i);
        if (c->magic == 0xFFFFFFFFu) { free_slot = i; break; }
        if (c->magic == CFG_MAGIC && c->check == cfg_check(*c)) last = c;
    }
    return last;
}

struct CfgWriteJob {
    uint32_t slot;
    bool erase_first;
    StoredCfg cfg;
};

uint8_t s_cfg_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

void cfg_write_cb(void* param)
{
    const CfgWriteJob* job = static_cast<const CfgWriteJob*>(param);
    if (job->erase_first) {
        flash_range_erase(CFG_OFFSET, FLASH_SECTOR_SIZE);
    }
    const uint32_t byte_off = job->slot * CFG_SLOT;
    const uint32_t page_off = (byte_off / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE;
    std::memset(s_cfg_page, 0xFF, sizeof(s_cfg_page)); // 0xFF bytes leave existing records untouched
    std::memcpy(s_cfg_page + (byte_off - page_off), &job->cfg, sizeof(StoredCfg));
    flash_range_program(CFG_OFFSET + page_off, s_cfg_page, FLASH_PAGE_SIZE);
}

/** Wheel position 0..65535 (centre 32768) -> stick X, with ±steer_deg = full deflection. */
int16_t wheel_to_stick(uint16_t raw, uint16_t steer_deg = STEER_DEGREES)
{
    const int32_t full = (32768 * 2 * int32_t(steer_deg)) / WHEEL_LOCK_DEGREES;      // counts for full stick
    constexpr int32_t dz   = (32768 * 2 * STEER_DEADZONE_DEG) / WHEEL_LOCK_DEGREES; // counts ignored at centre

    int32_t off = static_cast<int32_t>(raw) - 32768;
    const bool neg = off < 0;
    int32_t mag = neg ? -off : off;
    if (mag <= dz) {
        return 0;
    }
    mag -= dz;
    int32_t out = (mag * 32767) / (full - dz);
    if (out > 32767) {
        out = 32767;
    }
    return static_cast<int16_t>(neg ? -out : out);
}

/** Pedal byte (255 = released, 0 = floored) -> pressed amount 0..255. */
inline uint8_t pedal_pressed(uint8_t raw)
{
    return static_cast<uint8_t>(255 - raw);
}

void map_hat(Gamepad& gamepad, uint8_t hat, Gamepad::PadIn& gp_in)
{
    switch (hat) {
        case 0: gp_in.dpad |= gamepad.MAP_DPAD_UP;         break;
        case 1: gp_in.dpad |= gamepad.MAP_DPAD_UP_RIGHT;   break;
        case 2: gp_in.dpad |= gamepad.MAP_DPAD_RIGHT;      break;
        case 3: gp_in.dpad |= gamepad.MAP_DPAD_DOWN_RIGHT; break;
        case 4: gp_in.dpad |= gamepad.MAP_DPAD_DOWN;       break;
        case 5: gp_in.dpad |= gamepad.MAP_DPAD_DOWN_LEFT;  break;
        case 6: gp_in.dpad |= gamepad.MAP_DPAD_LEFT;       break;
        case 7: gp_in.dpad |= gamepad.MAP_DPAD_UP_LEFT;    break;
        default: break; // 8 = neutral
    }
}

/** Status colour on the board's RGB LED; only rewritten when it changes. */
void status_led(uint8_t r, uint8_t g, uint8_t b)
{
    static uint32_t last = 0xFFFFFFFF;
    const uint32_t c = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
    if (c != last && board_api_rgb::set_led) {
        board_api_rgb::set_led(r, g, b);
        last = c;
    }
}

} // namespace

bool LogitechG29Host::is_joystick_interface(const uint8_t* report_desc, uint16_t desc_len)
{
    /* Usage Page (Generic Desktop) 05 01, Usage (Joystick 04 / Game Pad 05) */
    if (!report_desc || desc_len < 4) {
        return false;
    }
    return report_desc[0] == 0x05 && report_desc[1] == 0x01 && report_desc[2] == 0x09 &&
           (report_desc[3] == 0x04 || report_desc[3] == 0x05);
}

void LogitechG29Host::queue_cmd(const uint8_t (&cmd)[CMD_LEN])
{
    if (cmd_count_ < MAX_CMDS) {
        std::memcpy(cmds_[cmd_count_], cmd, CMD_LEN);
        ++cmd_count_;
    }
}

void LogitechG29Host::initialize(Gamepad& gamepad, uint8_t address, uint8_t instance,
                                 const uint8_t* report_desc, uint16_t desc_len)
{
    (void)report_desc;
    (void)desc_len;

    uint16_t vid = 0, pid = 0;
    tuh_vid_pid_get(address, &vid, &pid);
    pid_ = pid;
    /* Anything that is not a known "needs mode switch" ID is treated as native layout. */
    native_ = !(pid == PID_COMPAT || pid == PID_G923_PS);
    cmd_report_id_ = 0;

    cmd_count_ = 0;
    cmd_next_ = 0;
    std::memset(prev_report_, 0, sizeof(prev_report_));
    prev_len_ = 0;
    load_settings();

    if (native_) {
        if (ENABLE_AUTOCENTER) {
            uint8_t spring[CMD_LEN];
            make_autocenter_cmd(AUTOCENTER_STRENGTH, spring);
            queue_cmd(spring);
            queue_cmd(CMD_AUTOCENTER_ACTIVATE);
        } else {
            /* Centring spring is set from send_feedback() (soft by default, red dial adjusts). */
            start_ms_ = board_api::ms_since_boot();
            applied_level_ = 0xFF;
            spring_step_ = 0;
        }
        start_rumble(gamepad, address, instance);
        OGXM_LOG("G29: native mode (%04x:%04x) addr=%u inst=%u\n", vid, pid,
                 static_cast<unsigned>(address), static_cast<unsigned>(instance));
    } else if (pid == PID_G923_PS) {
        /* G923 PlayStation FFB mode: ask it to re-enumerate in classic mode (report ID 0x30). */
        cmd_report_id_ = 0x30;
        queue_cmd(CMD_SWITCH_TO_G923);
        OGXM_LOG("G923: PS mode (%04x:%04x) -> requesting classic mode\n", vid, pid);
    } else {
        /* Compatibility mode: ask the wheel to re-enumerate as a native G29.
         * Deliberately NOT sending "revert mode on USB reset" (F8 0A): OGX reboots
         * the board when a host device stays unplugged >1.5 s, and that reboot
         * resets the bus. Without F8 0A the wheel stays native until power-off,
         * so a reboot converges to native mode instead of looping. */
#if G29_SWITCH_TO_NATIVE
        queue_cmd(CMD_SWITCH_TO_G29);
        OGXM_LOG("G29: compat mode (%04x:%04x) -> requesting native mode switch\n", vid, pid);
#else
        /* Stay in compatibility mode: no re-enumeration needed. */
        if (ENABLE_AUTOCENTER) {
            uint8_t spring[CMD_LEN];
            make_autocenter_cmd(AUTOCENTER_STRENGTH, spring);
            queue_cmd(spring);
            queue_cmd(CMD_AUTOCENTER_ACTIVATE);
        }
        OGXM_LOG("G29: compat mode (%04x:%04x), staying in compat\n", vid, pid);
#endif
    }

    gamepad.set_analog_host(true);
    tuh_hid_receive_report(address, instance);
}

void LogitechG29Host::process_report(Gamepad& gamepad, uint8_t address, uint8_t instance,
                                     const uint8_t* report, uint16_t len)
{
    if (report == nullptr || len == 0) {
        tuh_hid_receive_report(address, instance);
        return;
    }

#if !G29_RAW_DUMP
    const uint16_t cmp_len = (len < REPORT_LEN) ? len : REPORT_LEN;
    if (cmp_len == prev_len_ && std::memcmp(prev_report_, report, cmp_len) == 0) {
        tuh_hid_receive_report(address, instance);
        return;
    }
    std::memcpy(prev_report_, report, cmp_len);
    prev_len_ = cmp_len;
#endif

    const uint8_t* r = report;
    Gamepad::PadIn gp_in{};

#if G29_RAW_DUMP
    {
        /* Layout finder (only the LEFT stick + face buttons are used; they reach the PC reliably).
         *   First 4 s after the first report: hands off! Any byte changing then is marked as
         *   noise (counters/timestamps) and ignored afterwards. Button Y lit = learning phase.
         *   After that: AXIS0 = index / 64 of the byte that changed most, AXIS1 = its value / 256.
         *   Button A lit = a real change was found. Button B lit = report length >= 32. */
        static uint8_t prev[64] = {0};
        static uint8_t noisy[64] = {0};
        static uint16_t prev_len = 0;
        static uint16_t last_idx = 0;
        static uint8_t last_val = 0;
        static uint32_t t0 = 0;
        static bool seen = false;
        const uint32_t now = board_api::ms_since_boot();
        if (t0 == 0) t0 = now ? now : 1;
        const bool learning = (now - t0) < 4000;
        const uint16_t n = (len < 64) ? len : 64;
        int best = 0;
        for (uint16_t i = 0; i < n; ++i) {
            int d = int(report[i]) - int(prev[i]);
            if (d < 0) d = -d;
            if (prev_len != n || d == 0) continue;
            if (learning) { noisy[i] = 1; continue; }
            if (noisy[i]) continue;
            if (d > best) { best = d; last_idx = i; last_val = report[i]; seen = true; }
        }
        std::memcpy(prev, report, n);
        prev_len = n;
        gp_in.joystick_lx = static_cast<int16_t>(last_idx * 512);
        gp_in.joystick_ly = static_cast<int16_t>(-(int32_t(last_val) * 128));
        if (learning) gp_in.buttons |= gamepad.MAP_BUTTON_Y;
        if (seen)     gp_in.buttons |= gamepad.MAP_BUTTON_A;
        if (len >= 32) gp_in.buttons |= gamepad.MAP_BUTTON_B;
        status_led(0x00, 0xFF, 0xFF);
        gamepad.set_pad_in(gp_in);
        tuh_hid_receive_report(address, instance);
        return;
    }
#endif

    uint8_t gas_raw = 255, brake_raw = 255, clutch_raw = 255;
    int16_t diag_mode = 0;

    if (native_ && len >= REPORT_LEN) {
        /* ---- Native G29 layout (12 bytes) ---- */
        diag_mode = 16384;
        map_hat(gamepad, r[0] & B0::HAT_MASK, gp_in);
        if (r[0] & B0::CROSS)    gp_in.buttons |= gamepad.MAP_BUTTON_A;
        if (r[0] & B0::CIRCLE)   gp_in.buttons |= gamepad.MAP_BUTTON_B;
        if (r[0] & B0::SQUARE)   gp_in.buttons |= gamepad.MAP_BUTTON_X;
        if (r[0] & B0::TRIANGLE) gp_in.buttons |= gamepad.MAP_BUTTON_Y;
        if (r[1] & B1::L1)      gp_in.buttons |= gamepad.MAP_BUTTON_LB;
        if (r[1] & B1::R1)      gp_in.buttons |= gamepad.MAP_BUTTON_RB;
        if (r[1] & B1::SHARE)   gp_in.buttons |= gamepad.MAP_BUTTON_BACK;
        if (r[1] & B1::OPTIONS) gp_in.buttons |= gamepad.MAP_BUTTON_START;
        if (r[1] & B1::L3)      gp_in.buttons |= gamepad.MAP_BUTTON_L3;
        if (r[1] & B1::R3)      gp_in.buttons |= gamepad.MAP_BUTTON_R3;
        if (r[3] & B3::PS)      gp_in.buttons |= gamepad.MAP_BUTTON_SYS;
        if (r[2] & B2::GEAR_R)  gp_in.buttons |= gamepad.MAP_BUTTON_A;

        /* H-shifter -> sequential: each gear step becomes a paddle press (up = R, down = L).
         * The game clamps at 1st/top gear, so going back to 1st re-syncs automatically. */
        {
            int gear = 0;
            for (int i = 0; i < 6; ++i) {
                if (r[2] & (1u << i)) gear = i + 1;
            }
            if (r[2] & B2::GEAR_R) gear = 7;
            if (gear != knob_gear_) {
                if (gear >= 1 && gear <= 6) {
                    const int diff = gear - game_gear_;
                    if (diff > 0) shift_up_pending_ += static_cast<uint8_t>(diff);
                    if (diff < 0) shift_down_pending_ += static_cast<uint8_t>(-diff);
                    game_gear_ = gear;
                } else if (gear == 7) {
                    shift_up_pending_ = shift_down_pending_ = 0;
                    game_gear_ = 1;  // reverse is driven by the brake; game sits in 1st afterwards
                }
                knob_gear_ = gear;
            }
        }

        const uint16_t wheel = static_cast<uint16_t>(r[IDX_WHEEL_LO] | (r[IDX_WHEEL_HI] << 8));
        /* Enter (centre of the red dial):
         *   tap            -> vibration light -> medium -> strong -> off (on release)
         *   hold + "+"/"-" -> steering range 90/120/180/270 deg (LED blinks 1..4x)
         *   hold 5 s       -> factory defaults (handled in rumble_tick) */
        const bool enter_now = (r[3] & B3::ENTER) != 0;
        if (enter_now && !enter_prev_) {
            enter_down_t_ = board_api::ms_since_boot();
            enter_combo_used_ = false;
        }
        if (!enter_now && enter_prev_ && !enter_combo_used_) {
            rumble_level_ = static_cast<uint8_t>((rumble_level_ + 1) % 4);
            led_blink(static_cast<uint8_t>(rumble_level_ == 0 ? 1 : rumble_level_), rumble_level_ == 0 ? 0xFF0000u : 0xFF00FFu);
            mark_dirty();
        }
        enter_prev_ = enter_now;

        /* +/- rocker: anti-deadzone (5 % steps, 0..50 %), or steering range while Enter is held. */
        const bool plus_now  = (r[2] & B2::PLUS) != 0;
        const bool minus_now = (r[3] & B3::MINUS) != 0;
        if (enter_now) {
            if (plus_now && !plus_prev_ && steer_idx_ + 1 < STEER_OPTION_COUNT) { ++steer_idx_; }
            if (minus_now && !minus_prev_ && steer_idx_ > 0)                     { --steer_idx_; }
            if ((plus_now && !plus_prev_) || (minus_now && !minus_prev_)) {
                enter_combo_used_ = true;
                led_blink(static_cast<uint8_t>(steer_idx_ + 1), 0xFFFFFFu);
                mark_dirty();
            }
        } else {
            if (plus_now && !plus_prev_ && anti_dz_pct_ < 50)  { anti_dz_pct_ += 5; mark_dirty(); }
            if (minus_now && !minus_prev_ && anti_dz_pct_ > 0) { anti_dz_pct_ -= 5; mark_dirty(); }
        }
        plus_prev_ = plus_now;
        minus_prev_ = minus_now;

        /* Red dial: turn right = stiffer centring spring, left = softer (0 = off). */
        const bool cw_now  = (r[3] & B3::DIAL_CW) != 0;
        const bool ccw_now = (r[3] & B3::DIAL_CCW) != 0;
        if (cw_now && !cw_prev_ && spring_level_ < SPRING_LEVEL_MAX) { ++spring_level_; mark_dirty(); }
        if (ccw_now && !ccw_prev_ && spring_level_ > 0)              { --spring_level_; mark_dirty(); }
        cw_prev_ = cw_now;
        ccw_prev_ = ccw_now;

        /* Anti-deadzone: the moment the wheel leaves centre, jump past the game's own
         * stick deadzone, then scale linearly to full lock. */
        int32_t steer = wheel_to_stick(wheel, STEER_OPTIONS[steer_idx_]);
        if (steer != 0) {
            const int32_t floor_v = (32767 * anti_dz_pct_) / 100;
            const int32_t mag = (steer < 0) ? -steer : steer;
            int32_t out = floor_v + (mag * (32767 - floor_v)) / 32767;
            if (out > 32767) out = 32767;
            steer = (steer < 0) ? -out : out;
        }
        std::tie(gp_in.joystick_lx, gp_in.joystick_ly) =
            gamepad.scale_joystick_l(static_cast<int16_t>(steer), static_cast<int16_t>(0));

        if (!(r[IDX_FLAGS] & FLAG_PEDALS_DISCONNECTED)) {
            gas_raw = r[IDX_GAS];
            brake_raw = r[IDX_BRAKE];
            clutch_raw = r[IDX_CLUTCH];
        }
        update_led(); // blue = native reports (plus setting feedback patterns)
    } else if (!native_ && pid_ != PID_G923_PS && len >= 7 && len < REPORT_LEN) {
        /* ---- Driving Force EX compatibility layout (power-up state) ---- */
        diag_mode = -16384;
        /* byte0 + bits0-1 of byte1: 10-bit wheel (0..1023, centre 512) */
        const uint16_t w10 = static_cast<uint16_t>(r[0] | ((r[1] & 0x03) << 8));
        if (r[1] & 0x04) gp_in.buttons |= gamepad.MAP_BUTTON_A;  // cross
        if (r[1] & 0x08) gp_in.buttons |= gamepad.MAP_BUTTON_X;  // square
        if (r[1] & 0x10) gp_in.buttons |= gamepad.MAP_BUTTON_B;  // circle
        if (r[1] & 0x20) gp_in.buttons |= gamepad.MAP_BUTTON_Y;  // triangle
        if (r[1] & 0x40) gp_in.buttons |= gamepad.MAP_BUTTON_RB; // R1
        if (r[1] & 0x80) gp_in.buttons |= gamepad.MAP_BUTTON_LB; // L1
        if (r[2] & 0x04) gp_in.buttons |= gamepad.MAP_BUTTON_BACK;
        if (r[2] & 0x08) gp_in.buttons |= gamepad.MAP_BUTTON_START;
        map_hat(gamepad, r[4] & 0x0F, gp_in);
        /* compat mode reports ±450° in 10 bits; scale to the 16-bit native range */
        std::tie(gp_in.joystick_lx, gp_in.joystick_ly) =
            gamepad.scale_joystick_l(wheel_to_stick(static_cast<uint16_t>(w10 << 6), STEER_OPTIONS[steer_idx_]), static_cast<int16_t>(0));
        gas_raw = r[5];
        brake_raw = r[6];
        status_led(0xFF, 0xA0, 0x00); // yellow = compat reports
    } else {
        status_led(0xFF, 0x00, 0xFF); // purple = unknown report
    }

    const uint8_t gas = pedal_pressed(gas_raw);
    const uint8_t brake = pedal_pressed(brake_raw);
    const uint8_t clutch = pedal_pressed(clutch_raw);
    if (gas    >= PEDAL_THRESHOLD) gp_in.buttons |= gamepad.MAP_BUTTON_B;  // Switch A: accelerate
    if (brake  >= PEDAL_THRESHOLD) gp_in.buttons |= gamepad.MAP_BUTTON_A;  // Switch B: brake
    if (clutch >= PEDAL_THRESHOLD) gp_in.buttons |= gamepad.MAP_BUTTON_RB; // Switch R: drift

#if G29_DIAGNOSTICS
    /* Diagnostics visible in any gamepad tester:
     *   right stick X = layout (+0.5 native, -0.5 compat, 0 unknown)
     *   right stick Y = report length * 1000
     *   RT = gas pressed amount, LT = brake pressed amount */
    gp_in.joystick_rx = diag_mode;
    gp_in.joystick_ry = static_cast<int16_t>(len * 1000);
    gp_in.trigger_r = gas;
    gp_in.trigger_l = brake;
#else
    (void)diag_mode;
    if (native_ && len >= REPORT_LEN) {
        if (r[1] & B1::L2) gp_in.trigger_l = Range::MAX<uint8_t>;
        if (r[1] & B1::R2) gp_in.trigger_r = Range::MAX<uint8_t>;
    }
#endif

    last_pad_ = gp_in;
    has_last_pad_ = true;
    if (shift_btn_) gp_in.buttons |= shift_btn_;
    gamepad.set_pad_in(gp_in);
    tuh_hid_receive_report(address, instance);
}

bool LogitechG29Host::send_feedback(Gamepad& gamepad, uint8_t address, uint8_t instance)
{
    (void)gamepad;
    if (cmd_next_ >= cmd_count_) {
        if (!native_ || ENABLE_AUTOCENTER) {
            return true;
        }
        /* Startup: let the wheel finish its own calibration, then pull it firmly to the
         * centre for a few seconds, then drop to the chosen (soft) level. */
        const uint32_t since = board_api::ms_since_boot() - start_ms_;
        if (since < SPRING_START_DELAY_MS) {
            return true;
        }
        uint8_t level = spring_level_;
        if (since < SPRING_START_DELAY_MS + SPRING_CENTER_MS && level < SPRING_CENTER_LEVEL) {
            level = SPRING_CENTER_LEVEL;
        }
        if (level == applied_level_ && spring_step_ == 0) {
            return true;
        }
        if (spring_step_ == 1 && level != pending_level_) {
            spring_step_ = 0; // level changed mid-sequence: restart
        }
        if (!tuh_hid_send_ready(address, instance)) {
            return false;
        }
        uint8_t cmd[CMD_LEN];
        if (level == 0) {
            std::memcpy(cmd, CMD_AUTOCENTER_OFF, CMD_LEN);
        } else if (spring_step_ == 0) {
            const uint32_t mag = uint32_t(level) * 0xFFFFu / SPRING_LEVEL_MAX;
            make_autocenter_cmd(static_cast<uint16_t>(mag), cmd);
        } else {
            std::memcpy(cmd, CMD_AUTOCENTER_ACTIVATE, CMD_LEN);
        }
        if (tuh_hid_send_report(address, instance, 0, cmd, CMD_LEN)) {
            if (level == 0 || spring_step_ == 1) {
                applied_level_ = level;
                spring_step_ = 0;
            } else {
                pending_level_ = level;
                spring_step_ = 1;
            }
        }
        return true;
    }
    if (!tuh_hid_send_ready(address, instance)) {
        return false;
    }
    if (tuh_hid_send_report(address, instance, cmd_report_id_, cmds_[cmd_next_], CMD_LEN)) {
        OGXM_LOG("G29: sent cmd %u/%u (%02x %02x %02x)\n", static_cast<unsigned>(cmd_next_ + 1),
                 static_cast<unsigned>(cmd_count_), cmds_[cmd_next_][0], cmds_[cmd_next_][1],
                 cmds_[cmd_next_][2]);
        ++cmd_next_;
    }
    return true;
}


/* ---- Vibration: console rumble -> quick left/right force pulses on the wheel ---- */
void LogitechG29Host::start_rumble(Gamepad& gamepad, uint8_t address, uint8_t instance)
{
    stop_rumble_timer();
    rumble_gamepad_ = &gamepad;
    rumble_addr_ = address;
    rumble_inst_ = instance;
    rumble_tid_ = TaskQueue::Core1::get_new_task_id();
    TaskQueue::Core1::queue_delayed_task(rumble_tid_, RUMBLE_TICK_MS, true, [this] { rumble_tick(); });
}

void LogitechG29Host::stop_rumble_timer()
{
    if (rumble_tid_ != 0) {
        TaskQueue::Core1::cancel_delayed_task(rumble_tid_);
        rumble_tid_ = 0;
    }
}

void LogitechG29Host::rumble_tick()
{
    if (!native_ || rumble_gamepad_ == nullptr) {
        return;
    }

    /* Settings housekeeping: factory reset (Enter held 5 s), delayed save, LED patterns. */
    {
        const uint32_t now = board_api::ms_since_boot();
        if (enter_prev_ && !enter_combo_used_ && now - enter_down_t_ >= FACTORY_RESET_HOLD_MS) {
            enter_combo_used_ = true; // releasing Enter must not also cycle the vibration
            factory_reset();
        }
        if (settings_dirty_ && now - settings_change_t_ >= SETTINGS_SAVE_DELAY_MS) {
            save_settings();
        }
        update_led();
    }

    /* Shifter paddle pulses: press SHIFT_PRESS_MS, then a gap of SHIFT_GAP_MS. */
    {
        const uint32_t now = board_api::ms_since_boot();
        bool changed = false;
        if (shift_btn_ != 0) {
            if (now - shift_t_ >= SHIFT_PRESS_MS) { shift_btn_ = 0; shift_t_ = now; changed = true; }
        } else if ((shift_up_pending_ || shift_down_pending_) && now - shift_t_ >= SHIFT_GAP_MS) {
            if (shift_up_pending_) { shift_btn_ = rumble_gamepad_->MAP_BUTTON_RB; --shift_up_pending_; }
            else                   { shift_btn_ = rumble_gamepad_->MAP_BUTTON_LB; --shift_down_pending_; }
            shift_t_ = now;
            changed = true;
        }
        if (changed && has_last_pad_) {
            Gamepad::PadIn p = last_pad_;
            p.buttons |= shift_btn_;
            rumble_gamepad_->set_pad_in(p);
        }
    }
    const Gamepad::PadOut out = rumble_gamepad_->get_pad_out();
    const uint8_t amp = (out.rumble_l > out.rumble_r) ? out.rumble_l : out.rumble_r;
    const bool want = rumble_level_ != 0 && amp >= RUMBLE_MIN_AMP &&
                      (board_api::ms_since_boot() - start_ms_) > (SPRING_START_DELAY_MS + SPRING_CENTER_MS);

    if (!tuh_hid_send_ready(rumble_addr_, rumble_inst_)) {
        return;
    }
    if (!want) {
        if (rumble_active_) {
            const uint8_t stop[CMD_LEN] = { 0x13, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
            if (tuh_hid_send_report(rumble_addr_, rumble_inst_, 0, stop, CMD_LEN)) {
                rumble_active_ = false;
            }
        }
        return;
    }
    const uint32_t max_dev = (rumble_level_ == 1) ? RUMBLE_LIGHT : (rumble_level_ == 2) ? RUMBLE_MEDIUM : RUMBLE_STRONG;  // deflection from 0x80
    const uint8_t dev = static_cast<uint8_t>((uint32_t(amp) * max_dev) / 255u);
    rumble_phase_ = !rumble_phase_;
    const uint8_t force = rumble_phase_ ? static_cast<uint8_t>(0x80 + dev) : static_cast<uint8_t>(0x80 - dev);
    const uint8_t cmd[CMD_LEN] = { 0x11, 0x08, force, 0x80, 0x00, 0x00, 0x00 };
    if (tuh_hid_send_report(rumble_addr_, rumble_inst_, 0, cmd, CMD_LEN)) {
        rumble_active_ = true;
    }
}

/* ---- Persistent settings ---- */
void LogitechG29Host::mark_dirty()
{
    settings_dirty_ = true;
    settings_change_t_ = board_api::ms_since_boot();
}

void LogitechG29Host::load_settings()
{
    uint32_t free_slot = 0;
    const StoredCfg* c = cfg_find(free_slot);
    if (c == nullptr || c->version != 1) {
        return; // nothing stored yet: keep defaults
    }
    if (c->anti_dz <= 50 && c->anti_dz % 5 == 0) anti_dz_pct_ = c->anti_dz;
    if (c->spring <= SPRING_LEVEL_MAX)           spring_level_ = c->spring;
    if (c->rumble <= 3)                          rumble_level_ = c->rumble;
    if (c->steer_idx < STEER_OPTION_COUNT)       steer_idx_ = c->steer_idx;
    OGXM_LOG("G29: settings loaded dz=%u spring=%u rumble=%u steer=%u\n", anti_dz_pct_, spring_level_,
             rumble_level_, STEER_OPTIONS[steer_idx_]);
}

void LogitechG29Host::save_settings()
{
    settings_dirty_ = false;

    uint32_t free_slot = 0;
    const StoredCfg* cur = cfg_find(free_slot);

    CfgWriteJob job{};
    std::memset(&job.cfg, 0, sizeof(job.cfg));
    job.cfg.magic     = CFG_MAGIC;
    job.cfg.version   = 1;
    job.cfg.anti_dz   = anti_dz_pct_;
    job.cfg.spring    = spring_level_;
    job.cfg.rumble    = rumble_level_;
    job.cfg.steer_idx = steer_idx_;
    job.cfg.check     = cfg_check(job.cfg);

    if (cur != nullptr && std::memcmp(cur, &job.cfg, sizeof(StoredCfg)) == 0) {
        return; // unchanged (e.g. turned the dial back and forth)
    }
    job.erase_first = (free_slot >= CFG_SLOTS);
    job.slot = job.erase_first ? 0 : free_slot;

    const int rc = flash_safe_execute(cfg_write_cb, &job, 200);
    OGXM_LOG("G29: settings saved (slot %u, rc=%d)\n", static_cast<unsigned>(job.slot), rc);
    if (rc == PICO_OK) {
        led_flash(400, 0x00FF00u); // short green flash = saved
    }
}

void LogitechG29Host::factory_reset()
{
    anti_dz_pct_  = ANTI_DEADZONE_PCT;
    spring_level_ = SPRING_LEVEL_DEFAULT;
    rumble_level_ = RUMBLE_LEVEL_DEFAULT;
    steer_idx_    = STEER_IDX_DEFAULT;
    led_blink(3, 0xFF0000u); // 3x red = defaults restored
    settings_change_t_ = board_api::ms_since_boot() - SETTINGS_SAVE_DELAY_MS + 1500; // save after the blinks
    settings_dirty_ = true;
}

/* ---- Status LED patterns ---- */
void LogitechG29Host::led_blink(uint8_t count, uint32_t rgb)
{
    led_blinks_ = count;
    led_blink_rgb_ = rgb;
    led_blink_t_ = board_api::ms_since_boot();
}

void LogitechG29Host::led_flash(uint32_t ms, uint32_t rgb)
{
    led_flash_rgb_ = rgb;
    led_flash_until_ = board_api::ms_since_boot() + ms;
}

void LogitechG29Host::update_led()
{
    constexpr uint32_t PHASE_MS = 180;
    const uint32_t now = board_api::ms_since_boot();
    uint32_t rgb = 0x0000FFu; // normal: blue
    if (led_blinks_ != 0) {
        const uint32_t idx = (now - led_blink_t_) / PHASE_MS;
        if (idx >= 1u + uint32_t(led_blinks_) * 2u) {
            led_blinks_ = 0;
        } else {
            /* phase 0 = dark lead-in so a single blink is visible against blue */
            rgb = (idx % 2 == 1) ? led_blink_rgb_ : 0x000000u;
        }
    } else if (static_cast<int32_t>(led_flash_until_ - now) > 0) {
        rgb = led_flash_rgb_;
    }
    status_led(static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb));
}
