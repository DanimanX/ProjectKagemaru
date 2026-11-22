// Handheld SDL2 input mapping for Virtua Fighter on Anbernic RG34XX and
// compatible PortMaster Linux devices (with optional 2P USB/BT gamepad support).
#pragma once

#include <SDL.h>
#include <algorithm>
#include <cstdint>
#include <string>

namespace arm64 {

// Model 1 VF active-low input port bits (from src/host/main.cpp)
enum : uint8_t {
    IN0_COIN1 = 0x01, IN0_COIN2 = 0x02, IN0_TEST = 0x04, IN0_SERVICE = 0x08,
    IN0_START1 = 0x10, IN0_START2 = 0x20,
    PL_PUNCH = 0x01, PL_KICK = 0x02, PL_GUARD = 0x04,
    PL_DOWN = 0x10, PL_UP = 0x20, PL_RIGHT = 0x40, PL_LEFT = 0x80,
};

enum Button : uint32_t {
    BtnA = 1u << 0,      // SDL South
    BtnB = 1u << 1,      // SDL East
    BtnX = 1u << 2,      // SDL West
    BtnY = 1u << 3,      // SDL North
    Up = 1u << 4,
    Down = 1u << 5,
    Left = 1u << 6,
    Right = 1u << 7,
    L1 = 1u << 8,
    R1 = 1u << 9,
    L2 = 1u << 10,
    R2 = 1u << 11,
    Start = 1u << 12,
    Select = 1u << 13,
    Guide = 1u << 14,
};

enum Action : int {
    ActGuard = 0,
    ActPunch,
    ActKick,
    ActGuardPunch,
    ActPunchKick,
    ActGuardKick,
    ActAllThree,
    ActStart,
    ActCoin,
    ActionCount
};

inline constexpr uint32_t kBindableMask = 0x3fffu;

inline const char *action_name(int a) {
    static const char *names[ActionCount] = {
        "GUARD (G)", "PUNCH (P)", "KICK (K)",
        "GUARD + PUNCH (G+P)", "PUNCH + KICK (P+K)", "GUARD + KICK (G+K)",
        "ALL THREE (P+K+G)", "START", "COIN"
    };
    return (a >= 0 && a < ActionCount) ? names[a] : "?";
}

inline bool single_button(uint32_t b) {
    return b && !(b & ~kBindableMask) && !(b & (b - 1));
}

inline const char *button_name(uint32_t b) {
    static const char *names[] = {
        "A (SOUTH)", "B (EAST)", "X (WEST)", "Y (NORTH)",
        "D-UP", "D-DOWN", "D-LEFT", "D-RIGHT",
        "L1", "R1", "L2", "R2", "START", "SELECT"
    };
    if (!single_button(b)) return "NONE";
    int bit = 0;
    while (!(b & (1u << bit))) ++bit;
    return names[bit];
}

struct Bindings {
    // Saturn / RG34XX defaults: X=Guard, A=Punch, B=Kick, Y=G+P, R1=P+K, L1=G+K, R2=P+K+G
    uint32_t button[ActionCount] = {
        BtnX, BtnA, BtnB, BtnY, R1, L1, R2, Start, Select
    };

    void bind(int action, uint32_t value) {
        if (action < 0 || action >= ActionCount || !single_button(value)) return;
        for (uint32_t &other : button)
            if (other == value) other = button[action];
        button[action] = value;
    }

    void cycle(int action, int dir) {
        if (action < 0 || action >= ActionCount) return;
        int bit = 0;
        while (bit < 13 && !(button[action] & (1u << bit))) ++bit;
        bind(action, 1u << ((bit + (dir < 0 ? 13 : 1)) % 14));
    }

    void sanitize() {
        uint32_t seen = 0;
        for (uint32_t v : button) {
            if (!single_button(v) || (seen & v)) { *this = Bindings{}; return; }
            seen |= v;
        }
    }
};

inline bool menu_chord(uint32_t buttons) {
    return ((buttons & (Start | Select)) == (Start | Select)) || ((buttons & Guide) != 0);
}

class PadManager {
public:
    ~PadManager() { close(); }

    void init(const std::string &data_dir) {
        SDL_GameControllerAddMapping(
            "19000000010000000100000000010000,Anbernic RG34XX Controller,platform:Linux,"
            "b:b3,a:b4,dpdown:h0.4,leftx:a0,lefty:a1,lefttrigger:b12,dpleft:h0.8,"
            "rightshoulder:b8,leftshoulder:b7,righttrigger:b13,dpright:h0.2,"
            "back:b9,start:b10,dpup:h0.1,y:b6,x:b5,guide:b11,");
        const std::string db = data_dir + "/gamecontrollerdb.txt";
        SDL_GameControllerAddMappingsFromFile(db.c_str());
        for (int i = 0; i < SDL_NumJoysticks(); ++i) open_index(i);
    }

    void handle_event(const SDL_Event &e) {
        if (e.type == SDL_CONTROLLERDEVICEADDED) {
            open_index(e.cdevice.which);
        } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
            for (int p = 0; p < 2; ++p) {
                if (ctrl_[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctrl_[p])) == e.cdevice.which) {
                    SDL_GameControllerClose(ctrl_[p]);
                    ctrl_[p] = nullptr;
                }
            }
        } else if (e.type == SDL_JOYDEVICEADDED && !ctrl_[0] && !joy0_) {
            open_index(e.jdevice.which);
        } else if (e.type == SDL_JOYDEVICEREMOVED) {
            if (joy0_ && SDL_JoystickInstanceID(joy0_) == e.jdevice.which) {
                SDL_JoystickClose(joy0_);
                joy0_ = nullptr;
            }
        }
    }

    uint32_t poll_player(int player, const uint8_t *keys) const {
        uint32_t buttons = 0;
        if (player == 0 && keys) {
            if (keys[SDL_SCANCODE_UP]) buttons |= Up;
            if (keys[SDL_SCANCODE_DOWN]) buttons |= Down;
            if (keys[SDL_SCANCODE_LEFT]) buttons |= Left;
            if (keys[SDL_SCANCODE_RIGHT]) buttons |= Right;
            if (keys[SDL_SCANCODE_Z]) buttons |= BtnX;      // Guard
            if (keys[SDL_SCANCODE_X]) buttons |= BtnA;      // Punch
            if (keys[SDL_SCANCODE_C]) buttons |= BtnB;      // Kick
            if (keys[SDL_SCANCODE_A]) buttons |= BtnY;      // G+P
            if (keys[SDL_SCANCODE_S]) buttons |= R1;        // P+K
            if (keys[SDL_SCANCODE_D]) buttons |= L1;        // G+K
            if (keys[SDL_SCANCODE_V]) buttons |= R2;        // P+K+G
            if (keys[SDL_SCANCODE_1] || keys[SDL_SCANCODE_RETURN]) buttons |= Start;
            if (keys[SDL_SCANCODE_5] || keys[SDL_SCANCODE_RSHIFT]) buttons |= Select;
            if (keys[SDL_SCANCODE_ESCAPE]) buttons |= Guide;
        }
        if (player == 1 && keys) {
            if (keys[SDL_SCANCODE_I]) buttons |= Up;
            if (keys[SDL_SCANCODE_K]) buttons |= Down;
            if (keys[SDL_SCANCODE_J]) buttons |= Left;
            if (keys[SDL_SCANCODE_L]) buttons |= Right;
            if (keys[SDL_SCANCODE_B]) buttons |= BtnX;
            if (keys[SDL_SCANCODE_N]) buttons |= BtnA;
            if (keys[SDL_SCANCODE_M]) buttons |= BtnB;
            if (keys[SDL_SCANCODE_2]) buttons |= Start;
            if (keys[SDL_SCANCODE_6]) buttons |= Select;
        }
        SDL_GameController *c = (player >= 0 && player < 2) ? ctrl_[player] : nullptr;
        if (c) {
            auto btn = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(c, b) != 0; };
            if (btn(SDL_CONTROLLER_BUTTON_A)) buttons |= BtnA;
            if (btn(SDL_CONTROLLER_BUTTON_B)) buttons |= BtnB;
            if (btn(SDL_CONTROLLER_BUTTON_X)) buttons |= BtnX;
            if (btn(SDL_CONTROLLER_BUTTON_Y)) buttons |= BtnY;
            if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) buttons |= Up;
            if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) buttons |= Down;
            if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) buttons |= Left;
            if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) buttons |= Right;
            if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) buttons |= L1;
            if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) buttons |= R1;
            if (btn(SDL_CONTROLLER_BUTTON_START)) buttons |= Start;
            if (btn(SDL_CONTROLLER_BUTTON_BACK)) buttons |= Select;
            if (btn(SDL_CONTROLLER_BUTTON_GUIDE)) buttons |= Guide;
            const int dead = 8000;
            const int lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
            const int ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
            if (lx < -dead) buttons |= Left;
            if (lx > dead) buttons |= Right;
            if (ly < -dead) buttons |= Up;
            if (ly > dead) buttons |= Down;
            if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8000) buttons |= L2;
            if (SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8000) buttons |= R2;
        } else if (player == 0 && joy0_) {
            const Uint8 hat = SDL_JoystickNumHats(joy0_) > 0 ? SDL_JoystickGetHat(joy0_, 0) : 0;
            if (hat & SDL_HAT_UP) buttons |= Up;
            if (hat & SDL_HAT_DOWN) buttons |= Down;
            if (hat & SDL_HAT_LEFT) buttons |= Left;
            if (hat & SDL_HAT_RIGHT) buttons |= Right;
            const int dead = 8000;
            if (SDL_JoystickNumAxes(joy0_) >= 2) {
                const int lx = SDL_JoystickGetAxis(joy0_, 0);
                const int ly = SDL_JoystickGetAxis(joy0_, 1);
                if (lx < -dead) buttons |= Left;
                if (lx > dead) buttons |= Right;
                if (ly < -dead) buttons |= Up;
                if (ly > dead) buttons |= Down;
            }
            const int nb = SDL_JoystickNumButtons(joy0_);
            auto jb = [&](int idx) { return idx >= 0 && idx < nb && SDL_JoystickGetButton(joy0_, idx) != 0; };
            if (jb(3)) buttons |= BtnB;
            if (jb(4)) buttons |= BtnA;
            if (jb(5)) buttons |= BtnX;
            if (jb(6)) buttons |= BtnY;
            if (jb(7)) buttons |= L1;
            if (jb(8)) buttons |= R1;
            if (jb(9)) buttons |= Select;
            if (jb(10)) buttons |= Start;
            if (jb(11)) buttons |= Guide;
            if (jb(12)) buttons |= L2;
            if (jb(13)) buttons |= R2;
        }
        return buttons;
    }

    void close() {
        for (int p = 0; p < 2; ++p) {
            if (ctrl_[p]) SDL_GameControllerClose(ctrl_[p]);
            ctrl_[p] = nullptr;
        }
        if (joy0_) SDL_JoystickClose(joy0_);
        joy0_ = nullptr;
    }

    bool has_p2() const { return ctrl_[1] != nullptr; }

private:
    void open_index(int index) {
        if (SDL_IsGameController(index)) {
            SDL_GameController *c = SDL_GameControllerOpen(index);
            if (!c) return;
            const SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(c));
            for (int p = 0; p < 2; ++p) {
                if (ctrl_[p] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(ctrl_[p])) == id) {
                    SDL_GameControllerClose(c);
                    return;
                }
            }
            if (!ctrl_[0]) {
                ctrl_[0] = c;
                if (joy0_) { SDL_JoystickClose(joy0_); joy0_ = nullptr; }
            } else if (!ctrl_[1]) {
                ctrl_[1] = c;
            } else {
                SDL_GameControllerClose(c);
            }
            return;
        }
        if (!ctrl_[0] && !joy0_) joy0_ = SDL_JoystickOpen(index);
    }

    SDL_GameController *ctrl_[2] = {nullptr, nullptr};
    SDL_Joystick *joy0_ = nullptr;
};

inline void apply_player_buttons(uint32_t buttons, const Bindings &b, bool is_p2,
                                 uint8_t &in0, uint8_t &in_pl, bool block_start_coin) {
    if (buttons & Up) in_pl &= ~PL_UP;
    if (buttons & Down) in_pl &= ~PL_DOWN;
    if (buttons & Left) in_pl &= ~PL_LEFT;
    if (buttons & Right) in_pl &= ~PL_RIGHT;
    if (buttons & b.button[ActGuard]) in_pl &= ~PL_GUARD;
    if (buttons & b.button[ActPunch]) in_pl &= ~PL_PUNCH;
    if (buttons & b.button[ActKick]) in_pl &= ~PL_KICK;
    if (buttons & b.button[ActGuardPunch]) in_pl &= ~(PL_GUARD | PL_PUNCH);
    if (buttons & b.button[ActPunchKick]) in_pl &= ~(PL_PUNCH | PL_KICK);
    if (buttons & b.button[ActGuardKick]) in_pl &= ~(PL_GUARD | PL_KICK);
    if (buttons & b.button[ActAllThree]) in_pl &= ~(PL_GUARD | PL_PUNCH | PL_KICK);
    if (!block_start_coin) {
        if (buttons & b.button[ActStart]) in0 &= is_p2 ? ~IN0_START2 : ~IN0_START1;
        if (buttons & b.button[ActCoin]) in0 &= is_p2 ? ~IN0_COIN2 : ~IN0_COIN1;
    }
}

} // namespace arm64
