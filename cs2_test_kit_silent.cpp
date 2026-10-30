// cs2_test_kit_silent.cpp
// build: cl /O2 /EHsc /DNDEBUG cs2_test_kit_silent.cpp user32.lib
// or in VS: Configuration Properties > Linker > System > SubSystem = Windows

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <cmath>
#include <vector>
#include <algorithm>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(lib, "user32.lib")

namespace off {
    constexpr DWORD dwEntityList    = 0x18E5A68;
    constexpr DWORD dwLocalPlayer   = 0x18A8838;
    constexpr DWORD dwViewMatrix    = 0x1A60370;

    constexpr DWORD m_iTeamNum      = 0x3EB;
    constexpr DWORD m_iHealth       = 0x100;
    constexpr DWORD m_bDormant      = 0x10C;
    constexpr DWORD m_vecOrigin     = 0x78;
    constexpr DWORD m_pGameSceneNode= 0x318;
    constexpr DWORD m_boneArray     = 0x0AF8;
    constexpr DWORD m_aimPunch      = 0x1480;
}

enum Bone : int {
    HEAD = 6, NECK = 5, CHEST = 4, UPPER_CHEST = 3,
    PELVIS = 2, L_SHOULDER = 8, R_SHOULDER = 13,
    L_ELBOW = 9, R_ELBOW = 14, L_HAND = 11, R_HAND = 16
};

struct Vec2 { float x, y; };
struct Vec3 { float x, y, z; };
struct BoneMatrix { float m[3][4]; };

static inline Vec3 bone_pos(const BoneMatrix& b) { return { b.m[0][3], b.m[1][3], b.m[2][3] }; }

struct Config {
    bool  aimbot_enabled  = true;
    int   aimbot_key      = VK_XBUTTON1;
    float aimbot_fov      = 12.0f;
    float aimbot_smooth   = 3.0f;
    int   aimbot_bone     = CHEST;
    float aimbot_sens     = 0.85f;
    bool  aimbot_rcs      = true;
    float rcs_scale       = 1.4f;

    bool  assist_enabled  = true;
    int   assist_key      = VK_XBUTTON2;
    float assist_fov      = 6.0f;
    float assist_smooth   = 10.0f;
    int   assist_bone     = NECK;

    bool  trigger_enabled = true;
    int   trigger_key     = VK_MENU;
    float trigger_fov     = 4.0f;
    int   trigger_delay_ms= 30;
    int   trigger_burst   = 1;
    int   trigger_bone    = CHEST;
};

static Config cfg;
static HANDLE g_proc = nullptr;
static DWORD  g_pid  = 0;

static DWORD find_pid(const char* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (!_stricmp(pe.szExeFile, exe)) { pid = pe.th32ProcessID; break; }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

template <typename T>
static inline T rd(DWORD addr) {
    T v{};
    ReadProcessMemory(g_proc, (LPCVOID)addr, &v, sizeof(T), nullptr);
    return v;
}

static Vec2 world_to_screen(const Vec3& p, const float vm[16], int sw, int sh) {
    float cx = p.x * vm[0] + p.y * vm[4] + p.z * vm[8]  + vm[12];
    float cy = p.x * vm[1] + p.y * vm[5] + p.z * vm[9]  + vm[13];
    float cw = p.x * vm[3] + p.y * vm[7] + p.z * vm[11] + vm[15];
    if (cw < 0.01f) return { -1, -1 };
    return { (sw * 0.5f) * (1.f + cx / cw), (sh * 0.5f) * (1.f - cy / cw) };
}

struct Entity {
    DWORD pawn = 0;
    int   team = 0;
    int   hp   = 0;
    bool  dormant = false;
    Vec3  origin{};
    BoneMatrix bones[128]{};
};

static bool read_bones(DWORD pawn, BoneMatrix* out) {
    DWORD scene = rd<DWORD>(pawn + off::m_pGameSceneNode);
    if (!scene) return false;
    DWORD arr = rd<DWORD>(scene + off::m_boneArray);
    if (!arr) return false;
    return ReadProcessMemory(g_proc, (LPCVOID)arr, out, sizeof(BoneMatrix) * 128, nullptr);
}

static void collect_entities(int local_team, std::vector<Entity>& out) {
    out.clear();
    DWORD list = rd<DWORD>(off::dwEntityList);
    if (!list) return;

    for (int i = 1; i < 64; ++i) {
        DWORD ent = rd<DWORD>(list + i * 0x8);
        if (!ent) continue;

        Entity e{};
        e.pawn = ent;
        e.dormant = rd<bool>(ent + off::m_bDormant);
        if (e.dormant) continue;

        e.hp   = rd<int>(ent + off::m_iHealth);
        e.team = rd<int>(ent + off::m_iTeamNum);
        if (e.hp <= 0 || e.hp > 100 || e.team == local_team) continue;

        e.origin = rd<Vec3>(ent + off::m_vecOrigin);
        if (!read_bones(ent, e.bones)) continue;

        out.push_back(e);
    }
}

static void move_mouse(float dx, float dy) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)dx;
    in.mi.dy = (LONG)dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

static void mouse_click(int delay_ms) {
    INPUT down{};
    down.type = INPUT_MOUSE;
    down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    SendInput(1, &down, sizeof(INPUT));
    Sleep(delay_ms);
    INPUT up{};
    up.type = INPUT_MOUSE;
    up.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &up, sizeof(INPUT));
}

struct Target { Entity* ent = nullptr; Vec2 screen{}; float dist = 0; int bone = 0; };

static bool find_best(std::vector<Entity>& ents, const float vm[16], int sw, int sh,
                      float fov, int bone, Target& out)
{
    Vec2 cross{ sw * 0.5f, sh * 0.5f };
    float best = fov * fov;
    bool  found = false;

    for (auto& e : ents) {
        Vec2 sp = world_to_screen(bone_pos(e.bones[bone]), vm, sw, sh);
        if (sp.x < 0 || sp.y < 0 || sp.x > sw || sp.y > sh) continue;

        float d2 = (sp.x - cross.x) * (sp.x - cross.x)
                 + (sp.y - cross.y) * (sp.y - cross.y);
        if (d2 < best) {
            best = d2;
            out = { &e, sp, std::sqrt(d2), bone };
            found = true;
        }
    }
    return found;
}

static void run_aimbot(std::vector<Entity>& ents, const float vm[16], int sw, int sh, DWORD local) {
    Target t{};
    if (!find_best(ents, vm, sw, sh, cfg.aimbot_fov, cfg.aimbot_bone, t)) return;

    Vec2 cross{ sw * 0.5f, sh * 0.5f };
    float dx = t.screen.x - cross.x;
    float dy = t.screen.y - cross.y;

    if (cfg.aimbot_rcs) {
        Vec3 punch = rd<Vec3>(local + off::m_aimPunch);
        dx -= punch.x * cfg.rcs_scale;
        dy -= punch.y * cfg.rcs_scale;
    }

    dx = (dx / cfg.aimbot_smooth) * cfg.aimbot_sens;
    dy = (dy / cfg.aimbot_smooth) * cfg.aimbot_sens;
    move_mouse(dx, dy);
}

static void run_assist(std::vector<Entity>& ents, const float vm[16], int sw, int sh) {
    Target t{};
    if (!find_best(ents, vm, sw, sh, cfg.assist_fov, cfg.assist_bone, t)) return;

    Vec2 cross{ sw * 0.5f, sh * 0.5f };
    move_mouse((t.screen.x - cross.x) / cfg.assist_smooth,
               (t.screen.y - cross.y) / cfg.assist_smooth);
}

static void run_trigger(std::vector<Entity>& ents, const float vm[16], int sw, int sh) {
    Target t{};
    if (!find_best(ents, vm, sw, sh, cfg.trigger_fov, cfg.trigger_bone, t)) return;
    for (int i = 0; i < cfg.trigger_burst; ++i) mouse_click(cfg.trigger_delay_ms);
    Sleep(80);
}

static int run() {
    // single instance
    HANDLE mtx = CreateMutexA(nullptr, FALSE, "Global\\sys_input_helper_mtx");
    if (!mtx || GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    while (!(g_pid = find_pid("cs2.exe"))) Sleep(1000);

    g_proc = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, g_pid);
    if (!g_proc) return 1;

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);

    bool prev_trigger = false;

    for (;;) {
        DWORD local = rd<DWORD>(off::dwLocalPlayer);
        if (!local) { Sleep(32); continue; }

        int local_team = rd<int>(local + off::m_iTeamNum);

        float vm[16]{};
        ReadProcessMemory(g_proc, (LPCVOID)off::dwViewMatrix, vm, sizeof(vm), nullptr);

        std::vector<Entity> ents;
        collect_entities(local_team, ents);

        bool a = (GetAsyncKeyState(cfg.aimbot_key)  & 0x8000) != 0;
        bool s = (GetAsyncKeyState(cfg.assist_key)  & 0x8000) != 0;
        bool t = (GetAsyncKeyState(cfg.trigger_key) & 0x8000) != 0;

        if (cfg.aimbot_enabled  && a) run_aimbot(ents, vm, sw, sh, local);
        if (cfg.assist_enabled  && s) run_assist(ents, vm, sw, sh);
        if (cfg.trigger_enabled && t && !prev_trigger) run_trigger(ents, vm, sw, sh);

        prev_trigger = t;
        Sleep(1);
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    run();
    return 0;
}