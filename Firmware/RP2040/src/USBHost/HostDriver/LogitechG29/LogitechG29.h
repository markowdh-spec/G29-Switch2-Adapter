/*
 * Logitech G29 host driver for OGX-Mini (G29-Switch2-Adapter)
 * Copyright (c) 2026 markowdh-spec
 * SPDX-License-Identifier: MIT
 */
#ifndef _LOGITECH_G29_HOST_H_
#define _LOGITECH_G29_HOST_H_

#include <cstdint>
#include <cstring>

#include "USBHost/HostDriver/HostDriver.h"

/**
 * Logitech G29 Driving Force racing wheel (PS3/PS4 switch set to "PS3").
 *
 * The wheel powers up in a Driving Force compatibility mode (046D:C294). This
 * driver sends the documented "switch to native G29 mode" command; the wheel
 * then detaches and re-enumerates as 046D:C24F with a 12-byte input report.
 *
 * Default mapping (tuned for Mario Kart on Switch Pro output):
 *   wheel  -> left stick X (±STEER_DEGREES = full deflection)
 *   gas    -> A (Switch)      brake -> B (Switch)      clutch -> R (drift)
 *   paddles/L1/R1 -> L/R, L2/R2 -> ZL/ZR, face buttons by position,
 *   Options -> +, Share -> -, PS -> Home, shifter reverse gear -> B.
 */
namespace LogitechG29
{
    static constexpr uint16_t VID              = 0x046D;
    static constexpr uint16_t PID_NATIVE       = 0xC24F; // G29 native mode (after mode switch)
    static constexpr uint16_t PID_COMPAT       = 0xC294; // Driving Force EX compatibility mode (power-up)
    static constexpr uint16_t PID_G923         = 0xC266; // G923 classic mode (same 12-byte layout as G29)
    static constexpr uint16_t PID_G923_PS      = 0xC267; // G923 PlayStation FFB mode
    static constexpr uint16_t PID_G920         = 0xC262; // G920 HID mode

    static constexpr uint16_t REPORT_LEN       = 12;
    static constexpr uint8_t  CMD_LEN          = 7;

    /* ---- Tuning (change here, rebuild) ---- */
    static constexpr uint16_t WHEEL_LOCK_DEGREES = 900;  // G29 native full range
    static constexpr uint16_t STEER_DEGREES      = 120;  // ±deg for full stick deflection
    static constexpr uint16_t STEER_DEADZONE_DEG = 1;    // ignore tiny wobble around centre
    static constexpr uint8_t  ANTI_DEADZONE_PCT  = 15;   // start value; +/- rocker changes it live
    static constexpr uint8_t  PEDAL_THRESHOLD    = 64;   // 0..255 pressed amount for a button press
    static constexpr bool     ENABLE_AUTOCENTER  = false; // gentle spring back to centre
    static constexpr uint16_t AUTOCENTER_STRENGTH = 0x4000; // 0..0xFFFF
    static constexpr uint8_t  SPRING_LEVEL_MAX     = 8;  // red dial steps
    static constexpr uint8_t  SPRING_LEVEL_DEFAULT = 1;  // soft (kid friendly); 0 = no spring
    static constexpr uint8_t  RUMBLE_LEVEL_DEFAULT = 1;  // 0 off, 1 light, 2 medium, 3 strong (Enter button cycles)
    static constexpr uint32_t RUMBLE_TICK_MS       = 25; // pulse rate (alternating direction)
    static constexpr uint8_t  RUMBLE_MIN_AMP       = 12; // ignore tiny console rumble values
    static constexpr uint32_t RUMBLE_LIGHT         = 15; // max force deflection (of 127)
    static constexpr uint32_t RUMBLE_MEDIUM        = 28;
    static constexpr uint32_t RUMBLE_STRONG        = 50;
    static constexpr uint32_t SHIFT_PRESS_MS       = 75; // paddle press length for shifter emulation
    static constexpr uint32_t SHIFT_GAP_MS         = 75;
    static constexpr uint8_t  SPRING_CENTER_LEVEL  = 5;  // firm pull to centre right after start-up
    static constexpr uint32_t SPRING_START_DELAY_MS = 2500; // wait for the wheel's own calibration
    static constexpr uint32_t SPRING_CENTER_MS      = 3000; // how long the firm centring lasts

    /* Steering range presets (Enter held + "+"/"-" cycles them); LED blinks 1..4 times. */
    static constexpr uint16_t STEER_OPTIONS[]      = { 90, 120, 180, 270 };
    static constexpr uint8_t  STEER_OPTION_COUNT   = 4;
    static constexpr uint8_t  STEER_IDX_DEFAULT    = 1;    // 120 deg
    static constexpr uint32_t SETTINGS_SAVE_DELAY_MS = 3000; // save this long after the last change
    static constexpr uint32_t FACTORY_RESET_HOLD_MS  = 5000; // hold Enter this long = defaults

    /* ---- Native report layout (bit fields, LSB first) ---- */
    namespace B0 { // byte 0: hat (low nibble) + face buttons
        static constexpr uint8_t HAT_MASK = 0x0F;
        static constexpr uint8_t CROSS    = 0x10;
        static constexpr uint8_t SQUARE   = 0x20;
        static constexpr uint8_t CIRCLE   = 0x40;
        static constexpr uint8_t TRIANGLE = 0x80;
    }
    namespace B1 {
        static constexpr uint8_t R1      = 0x01;
        static constexpr uint8_t L1      = 0x02;
        static constexpr uint8_t R2      = 0x04;
        static constexpr uint8_t L2      = 0x08;
        static constexpr uint8_t SHARE   = 0x10;
        static constexpr uint8_t OPTIONS = 0x20;
        static constexpr uint8_t R3      = 0x40;
        static constexpr uint8_t L3      = 0x80;
    }
    namespace B2 { // H-shifter gears + plus
        static constexpr uint8_t GEAR_R  = 0x40;
        static constexpr uint8_t PLUS    = 0x80;
    }
    namespace B3 {
        static constexpr uint8_t MINUS    = 0x01;
        static constexpr uint8_t DIAL_CW  = 0x02;
        static constexpr uint8_t DIAL_CCW = 0x04;
        static constexpr uint8_t ENTER    = 0x08;
        static constexpr uint8_t PS       = 0x10;
    }
    static constexpr uint8_t IDX_WHEEL_LO = 4;
    static constexpr uint8_t IDX_WHEEL_HI = 5;
    static constexpr uint8_t IDX_GAS      = 6;  // 255 = released
    static constexpr uint8_t IDX_BRAKE    = 7;  // 255 = released
    static constexpr uint8_t IDX_CLUTCH   = 8;  // 255 = released
    static constexpr uint8_t IDX_FLAGS    = 11;
    static constexpr uint8_t FLAG_PEDALS_DISCONNECTED = 0x02;
}

class LogitechG29Host : public HostDriver
{
public:
    explicit LogitechG29Host(uint8_t idx)
        : HostDriver(idx) {}

    void initialize(Gamepad& gamepad, uint8_t address, uint8_t instance, const uint8_t* report_desc,
                    uint16_t desc_len) override;
    void process_report(Gamepad& gamepad, uint8_t address, uint8_t instance, const uint8_t* report,
                        uint16_t len) override;
    bool send_feedback(Gamepad& gamepad, uint8_t address, uint8_t instance) override;
    /** Called on unplug: release everything so no pedal/button stays "held". */
    ~LogitechG29Host() override { stop_rumble_timer(); }

    void disconnect_cb(Gamepad& gamepad, uint8_t address, uint8_t instance) override
    {
        (void)address;
        (void)instance;
        stop_rumble_timer();
        rumble_gamepad_ = nullptr;
        gamepad.set_pad_in(Gamepad::PadIn{});
    }

    static bool is_known_id(uint16_t vid, uint16_t pid)
    {
        return vid == LogitechG29::VID &&
               (pid == LogitechG29::PID_NATIVE || pid == LogitechG29::PID_COMPAT ||
                pid == LogitechG29::PID_G923 || pid == LogitechG29::PID_G923_PS);
    }

    /** Accept HID interfaces whose top collection is Generic Desktop Joystick or Game Pad. */
    static bool is_joystick_interface(const uint8_t* report_desc, uint16_t desc_len);

private:
    /* Commands are sent one per send_feedback() tick (~200 ms apart). */
    static constexpr uint8_t MAX_CMDS = 3;
    uint8_t cmds_[MAX_CMDS][LogitechG29::CMD_LEN]{};
    uint8_t cmd_count_{0};
    uint8_t cmd_next_{0};

    bool native_{false};
    uint16_t pid_{0};
    uint8_t cmd_report_id_{0};
    uint8_t anti_dz_pct_{LogitechG29::ANTI_DEADZONE_PCT};
    bool plus_prev_{false};
    bool minus_prev_{false};
    uint8_t spring_level_{LogitechG29::SPRING_LEVEL_DEFAULT};
    uint32_t start_ms_{0};
    uint8_t applied_level_{0xFF};
    uint8_t pending_level_{0};
    uint8_t spring_step_{0};
    bool cw_prev_{false};
    bool ccw_prev_{false};
    bool enter_prev_{false};

    /* vibration */
    uint8_t rumble_level_{LogitechG29::RUMBLE_LEVEL_DEFAULT};
    uint32_t rumble_tid_{0};
    Gamepad* rumble_gamepad_{nullptr};
    uint8_t rumble_addr_{0};
    uint8_t rumble_inst_{0};
    bool rumble_active_{false};
    bool rumble_phase_{false};
    void start_rumble(Gamepad& gamepad, uint8_t address, uint8_t instance);
    void stop_rumble_timer();
    void rumble_tick();

    /* H-shifter */
    int knob_gear_{0};          // 0 neutral, 1..6, 7 reverse
    int game_gear_{1};          // our estimate of the gear the game is in
    uint8_t shift_up_pending_{0};
    uint8_t shift_down_pending_{0};
    uint16_t shift_btn_{0};
    uint32_t shift_t_{0};
    Gamepad::PadIn last_pad_{};
    bool has_last_pad_{false};
    uint8_t prev_report_[LogitechG29::REPORT_LEN]{};
    uint16_t prev_len_{0};

    /* steering range + persistent settings */
    uint8_t steer_idx_{LogitechG29::STEER_IDX_DEFAULT};
    uint32_t enter_down_t_{0};
    bool enter_combo_used_{false};
    bool settings_dirty_{false};
    uint32_t settings_change_t_{0};
    void mark_dirty();
    void load_settings();
    void save_settings();
    void factory_reset();

    /* status LED patterns (blinks have priority over a single flash) */
    uint8_t led_blinks_{0};
    uint32_t led_blink_t_{0};
    uint32_t led_blink_rgb_{0};
    uint32_t led_flash_until_{0};
    uint32_t led_flash_rgb_{0};
    void led_blink(uint8_t count, uint32_t rgb);
    void led_flash(uint32_t ms, uint32_t rgb);
    void update_led();

    void queue_cmd(const uint8_t (&cmd)[LogitechG29::CMD_LEN]);
};

#endif // _LOGITECH_G29_HOST_H_
