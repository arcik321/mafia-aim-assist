#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_M_IX86)
#error MafiaAimLogic must be built for Win32/x86.
#endif

/* Game.exe is loaded at its preferred base, so RVAs are used for its globals. */
#define WORLD_POINTER_RVA        0x247E1Cu
#define PED_VTABLE_RVA           0x225090u
#define LS3DF_CAMERA_GLOBAL_RVA  0x1C4CF8u
#define WORLD_PLAYER_OFFSET      0x54u
#define WORLD_VEHICLE_OFFSET     0x58u
#define WORLD_LIST_BEGIN_OFFSET  0xF0u
#define WORLD_LIST_END_OFFSET    0xF4u
#define ENTITY_POSITION_OFFSET   0x24u
#define ENTITY_HEALTH_OFFSET     0x644u
#define CAMERA_FORWARD_OFFSET    0x30u
#define CAMERA_POSITION_OFFSET   0x40u

#define MAX_ENTITIES        512u
#define MAX_TARGET_DISTANCE 80.0f
#define MIN_FORWARD_DOT     0.64f
static float g_aimHeight = 0.95f; /* metres above the ped's origin; low enough to hit a crouching target */
#define MIN_STEP_MS         5
#define DEADZONE_RAD        0.004f
#define MAX_STEP_COUNTS     40
#define K_INIT              0.18f
#define K_MIN               0.06f
#define K_MAX               0.24f
#define AIM_BRAKE_ANGLE     0.12f
#define AIM_BRAKE_FLOOR     0.25f
#define PI_F                3.14159265f

/* Mouse sensitivity measured on this game: radians of camera turn per mouse count. */
#define SEED_GAIN_X   0.002039
#define SEED_GAIN_Y  -0.001105

typedef struct Vector3 { float x, y, z; } Vector3;

static HANDLE g_log = INVALID_HANDLE_VALUE;
static LONG   g_logLines;

static void Log(const char *format, ...)
{
    char line[320];
    DWORD written;
    int length;
    va_list args;
    if (g_logLines > 4000)
        return;
    if (g_log == INVALID_HANDLE_VALUE)
    {
        char path[MAX_PATH];
        DWORD n = GetTempPathA(MAX_PATH, path);
        if (!n || n > MAX_PATH - 32)
            return;
        lstrcatA(path, "MafiaAimLogic.log");
        g_log = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (g_log == INVALID_HANDLE_VALUE)
            return;
    }
    va_start(args, format);
    length = _vsnprintf(line, sizeof(line) - 3, format, args);
    va_end(args);
    if (length < 0 || length > (int)sizeof(line) - 3)
        length = (int)sizeof(line) - 3;
    line[length++] = '\r';
    line[length++] = '\n';
    WriteFile(g_log, line, (DWORD)length, &written, NULL);
    FlushFileBuffers(g_log);
    ++g_logLines;
}

static LONGLONG NowMs(void)
{
    static LARGE_INTEGER frequency;
    LARGE_INTEGER counter;
    if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&counter);
    return counter.QuadPart * 1000 / frequency.QuadPart;
}

/* ---- safe memory access ------------------------------------------------ */

static int IsReadable(const void *address, SIZE_T size)
{
    MEMORY_BASIC_INFORMATION info;
    uintptr_t start = (uintptr_t)address;
    uintptr_t end = start + size;
    DWORD protection;
    if (!address || size == 0 || end < start || start < 0x10000u || start > 0x7FFF0000u)
        return 0;
    if (VirtualQuery(address, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT)
        return 0;
    protection = info.Protect & 0xFFu;
    if (protection == PAGE_NOACCESS || protection == PAGE_EXECUTE || (info.Protect & PAGE_GUARD))
        return 0;
    return end <= (uintptr_t)info.BaseAddress + info.RegionSize;
}

static uint32_t ReadU32(uintptr_t address)
{
    return IsReadable((const void *)address, 4) ? *(volatile uint32_t *)address : 0;
}

static int IsFinite(float value)
{
    return value == value && value < 1.0e20f && value > -1.0e20f;
}

static int ReadVector(uintptr_t address, Vector3 *out)
{
    if (!IsReadable((const void *)address, sizeof(Vector3)))
        return 0;
    out->x = *(volatile float *)(address);
    out->y = *(volatile float *)(address + 4u);
    out->z = *(volatile float *)(address + 8u);
    return IsFinite(out->x) && IsFinite(out->y) && IsFinite(out->z);
}

/* ---- vector helpers ---------------------------------------------------- */

static float Dot(Vector3 a, Vector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

static Vector3 Subtract(Vector3 a, Vector3 b)
{
    Vector3 r = {a.x - b.x, a.y - b.y, a.z - b.z};
    return r;
}

static int Normalize(Vector3 *v)
{
    float lengthSquared = Dot(*v, *v);
    float inverse;
    if (!IsFinite(lengthSquared) || lengthSquared < 0.0001f)
        return 0;
    inverse = 1.0f / sqrtf(lengthSquared);
    v->x *= inverse; v->y *= inverse; v->z *= inverse;
    return 1;
}

static float WrapAngle(float angle)
{
    while (angle > PI_F) angle -= 2.0f * PI_F;
    while (angle < -PI_F) angle += 2.0f * PI_F;
    return angle;
}

static float ClampUnit(float v) { return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v); }

/* ---- game state -------------------------------------------------------- */

static int GetWorld(uintptr_t *world, uintptr_t *player)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    uintptr_t w = ReadU32(base + WORLD_POINTER_RVA);
    uintptr_t p;
    if (w < 0x10000u)
        return 0;
    p = ReadU32(w + WORLD_PLAYER_OFFSET);
    if (p < 0x10000u || ReadU32(w + WORLD_VEHICLE_OFFSET) != 0)
        return 0; /* no player, or driving: no aim assist */
    *world = w;
    *player = p;
    return 1;
}

static int ReadCamera(Vector3 *position, Vector3 *forward)
{
    HMODULE ls3df = GetModuleHandleA("LS3DF.dll");
    uintptr_t camera;
    if (!ls3df)
        return 0;
    camera = ReadU32((uintptr_t)ls3df + LS3DF_CAMERA_GLOBAL_RVA);
    if (camera < 0x10000u)
        return 0;
    if (!ReadVector(camera + CAMERA_FORWARD_OFFSET, forward) ||
        !ReadVector(camera + CAMERA_POSITION_OFFSET, position))
        return 0;
    return Normalize(forward);
}

static int IsLivePed(uintptr_t object, Vector3 *position)
{
    uintptr_t base = (uintptr_t)GetModuleHandleA(NULL);
    float health;
    if (object < 0x10000u || !IsReadable((const void *)object, ENTITY_HEALTH_OFFSET + 4u))
        return 0;
    if (ReadU32(object) != base + PED_VTABLE_RVA)
        return 0;
    health = *(volatile float *)(object + ENTITY_HEALTH_OFFSET);
    if (!IsFinite(health) || health < 0.1f || health > 1000.0f)
        return 0;
    return ReadVector(object + ENTITY_POSITION_OFFSET, position);
}

static int ListBounds(uintptr_t world, uintptr_t *begin, uintptr_t *count)
{
    uintptr_t b = ReadU32(world + WORLD_LIST_BEGIN_OFFSET);
    uintptr_t e = ReadU32(world + WORLD_LIST_END_OFFSET);
    if (b < 0x10000u || e < b || ((e - b) & 3u))
        return 0;
    *count = (e - b) / 4u;
    if (*count == 0 || *count > MAX_ENTITIES || !IsReadable((const void *)b, *count * 4u))
        return 0;
    *begin = b;
    return 1;
}

static int TargetStillValid(uintptr_t world, uintptr_t player, uintptr_t target, Vector3 *position)
{
    uintptr_t begin, count, i;
    if (!ListBounds(world, &begin, &count))
        return 0;
    for (i = 0; i < count; ++i)
        if (ReadU32(begin + i * 4u) == target && target != player)
            return IsLivePed(target, position);
    return 0;
}

static uintptr_t FindNearestPed(uintptr_t world, uintptr_t player, Vector3 cameraPosition,
                                Vector3 cameraForward, Vector3 *targetPosition)
{
    uintptr_t begin, count, i, nearest = 0;
    float best = MAX_TARGET_DISTANCE * MAX_TARGET_DISTANCE;
    if (!ListBounds(world, &begin, &count))
        return 0;
    for (i = 0; i < count; ++i)
    {
        uintptr_t object = ReadU32(begin + i * 4u);
        Vector3 position, direction;
        float distanceSquared;
        if (object == player || !IsLivePed(object, &position))
            continue;
        position.y += g_aimHeight;
        direction = Subtract(position, cameraPosition);
        distanceSquared = Dot(direction, direction);
        if (distanceSquared < 0.25f || distanceSquared > best || !Normalize(&direction))
            continue;
        if (Dot(direction, cameraForward) < MIN_FORWARD_DOT)
            continue;
        best = distanceSquared;
        nearest = object;
        *targetPosition = position;
    }
    if (nearest)
        targetPosition->y -= g_aimHeight;
    return nearest;
}

/* ---- settings (MafiaAimAssist.ini beside Game.exe, re-read every second) -- */

typedef struct Config
{
    int stickLook;   /* right stick moves the camera like a mouse */
    int xSpeed;      /* mouse counts per second at full deflection */
    int ySpeed;
    int invertY;
    int deadzone;    /* percent of stick travel */
    int aimResponse; /* percentage scaling the lock-on controller */
} Config;

static Config g_cfg = {1, 1100, 1000, 0, 15, 70};
static char g_iniPath[MAX_PATH];
static LONGLONG g_configNext;

static int ClampInt(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void ReloadConfig(LONGLONG now)
{
    if (now < g_configNext)
        return;
    g_configNext = now + 1000;
    if (!g_iniPath[0])
    {
        DWORD n = GetModuleFileNameA(NULL, g_iniPath, MAX_PATH);
        if (!n || n > MAX_PATH - 32)
            return;
        while (n && g_iniPath[n - 1] != '\\' && g_iniPath[n - 1] != '/')
            --n;
        g_iniPath[n] = '\0';
        lstrcatA(g_iniPath, "MafiaAimAssist.ini");
    }
    g_cfg.stickLook = GetPrivateProfileIntA("aim", "right_stick_look", 1, g_iniPath) != 0;
    g_cfg.xSpeed = ClampInt((int)GetPrivateProfileIntA("aim", "look_x_speed", 1100, g_iniPath), 100, 6000);
    g_cfg.ySpeed = ClampInt((int)GetPrivateProfileIntA("aim", "look_y_speed", 1000, g_iniPath), 100, 6000);
    g_cfg.invertY = GetPrivateProfileIntA("aim", "invert_y", 0, g_iniPath) != 0;
    g_cfg.deadzone = ClampInt((int)GetPrivateProfileIntA("aim", "stick_deadzone", 15, g_iniPath), 0, 60);
    g_cfg.aimResponse = ClampInt((int)GetPrivateProfileIntA("aim", "aim_response_percent", 70, g_iniPath), 25, 150);
    g_aimHeight = (float)ClampInt((int)GetPrivateProfileIntA("aim", "aim_height_cm", 95, g_iniPath), 40, 180) / 100.0f;
}

/* ---- controller (XInput) ------------------------------------------------- */

typedef struct XiGamepad
{
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT thumbLX, thumbLY, thumbRX, thumbRY;
} XiGamepad;

typedef struct XiState
{
    DWORD packet;
    XiGamepad gamepad;
} XiState;

typedef DWORD (WINAPI *XInputGetStateFn)(DWORD index, XiState *state);

#define TRIGGER_PRESS   60
#define TRIGGER_RELEASE 20

static XiState g_pad;
static int     g_padOk;

static void PollPad(LONGLONG now)
{
    static XInputGetStateFn getState;
    static int tried, slot = -1;
    static LONGLONG nextPoll, nextScan;
    XiState state;
    int i;

    if (!tried)
    {
        static const char *libraries[] = {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"};
        tried = 1;
        for (i = 0; i < 3 && !getState; ++i)
        {
            HMODULE library = LoadLibraryA(libraries[i]);
            if (library)
            {
                getState = (XInputGetStateFn)GetProcAddress(library, "XInputGetState");
                if (getState)
                    Log("using %s", libraries[i]);
            }
        }
    }
    if (!getState || now < nextPoll)
        return;
    nextPoll = now + 3;

    if (slot < 0)
    {
        g_padOk = 0;
        if (now < nextScan)
            return;
        nextScan = now + 2000;
        for (i = 0; i < 4 && slot < 0; ++i)
            if (getState((DWORD)i, &state) == ERROR_SUCCESS)
                slot = i;
        if (slot < 0)
            return;
        Log("xinput controller found in slot %d", slot);
    }
    if (getState((DWORD)slot, &state) != ERROR_SUCCESS)
    {
        slot = -1;
        g_padOk = 0;
        return;
    }
    g_pad = state;
    g_padOk = 1;
}

static int TriggerHeld(void)
{
    static int held;
    if (!g_padOk)
        return held = 0;
    held = held ? g_pad.gamepad.leftTrigger > TRIGGER_RELEASE : g_pad.gamepad.leftTrigger > TRIGGER_PRESS;
    return held;
}

static int AimButtonHeld(void)
{
    return TriggerHeld() || (GetAsyncKeyState('O') & 0x8000) != 0;
}

/* Right stick drives the mouse axes: radial deadzone, squared response, counts per second. */
static void AddStickLook(LONG *lx, LONG *ly, LONG dtMs)
{
    static float remX, remY;
    static int logged;
    float sx = (float)g_pad.gamepad.thumbRX / 32767.0f;
    float sy = (float)g_pad.gamepad.thumbRY / 32767.0f;
    float magnitude = sqrtf(sx * sx + sy * sy);
    float deadzone = (float)g_cfg.deadzone / 100.0f;
    float scaled, curve, dt, cx, cy;
    LONG ix, iy;

    if (magnitude <= deadzone)
    {
        remX = remY = 0.0f;
        return;
    }
    scaled = (magnitude - deadzone) / (1.0f - deadzone);
    if (scaled > 1.0f)
        scaled = 1.0f;
    curve = scaled * scaled;
    dt = (float)dtMs / 1000.0f;
    cx = (sx / magnitude) * curve * (float)g_cfg.xSpeed * dt + remX;
    cy = -(sy / magnitude) * curve * (float)g_cfg.ySpeed * dt * (g_cfg.invertY ? -1.0f : 1.0f) + remY;
    ix = (LONG)floorf(cx + 0.5f);
    iy = (LONG)floorf(cy + 0.5f);
    remX = cx - (float)ix;
    remY = cy - (float)iy;
    *lx += ix;
    *ly += iy;
    if (!logged)
    {
        logged = 1;
        Log("right stick look active");
    }
}

/* ---- sensitivity: learned online from how the camera really responds ------ */

typedef struct AxisModel
{
    double gain;      /* signed radians of camera turn per mouse count */
    double sxy, sxx;  /* decayed regression sums of turn against counts */
    int votes;        /* consecutive estimates that disagree in sign */
    int updates;
} AxisModel;

static AxisModel g_axis[2];

static void SeedGains(void)
{
    memset(g_axis, 0, sizeof(g_axis));
    g_axis[0].gain = SEED_GAIN_X;
    g_axis[1].gain = SEED_GAIN_Y;
}

static void GainPath(char *path)
{
    GetTempPathA(MAX_PATH - 32, path);
    lstrcatA(path, "MafiaAimGain.cal");
}

static void LoadGains(void)
{
    char path[MAX_PATH];
    FILE *file;
    long gx = 0, gy = 0;
    SeedGains();
    GainPath(path);
    file = fopen(path, "r");
    if (!file)
        return;
    if (fscanf(file, "%ld %ld", &gx, &gy) == 2)
    {
        double x = (double)gx * 1.0e-7, y = (double)gy * 1.0e-7;
        if (fabs(x) > 0.0007 && fabs(x) < 0.006 && fabs(y) > 0.0007 && fabs(y) < 0.006)
        {
            g_axis[0].gain = x;
            g_axis[1].gain = y;
        }
    }
    fclose(file);
}

static void SaveGains(void)
{
    char path[MAX_PATH];
    FILE *file;
    GainPath(path);
    file = fopen(path, "w");
    if (!file)
        return;
    fprintf(file, "%ld %ld\n", (long)(g_axis[0].gain * 1.0e7), (long)(g_axis[1].gain * 1.0e7));
    fclose(file);
}

static void EstimateAxis(AxisModel *axis, const char *name, float turn, LONG counts)
{
    double estimate;
    LONG magnitude = counts < 0 ? -counts : counts;

    axis->sxy *= 0.97;
    axis->sxx *= 0.97;
    if (magnitude < 3)
        return;
    if (magnitude >= 15 && fabsf(turn) < 0.0002f)
        return; /* the camera did not move at all: clamped, not informative */
    axis->sxy += (double)turn * (double)counts;
    axis->sxx += (double)counts * (double)counts;
    if (axis->sxx < 6000.0)
        return;
    estimate = axis->sxy / axis->sxx;
    if (fabs(estimate) < 0.0004 || fabs(estimate) > 0.01)
        return;
    if ((estimate > 0.0) == (axis->gain > 0.0))
    {
        axis->votes = 0;
        axis->gain += 0.2 * (estimate - axis->gain);
        ++axis->updates;
    }
    else if (++axis->votes >= 12)
    {
        Log("%s gain sign corrected: %.6f -> %.6f", name, axis->gain, estimate);
        axis->gain = estimate;
        axis->sxy = axis->sxx = 0.0;
        axis->votes = 0;
    }
}

/* ---- aim controller ------------------------------------------------------ */

static uintptr_t g_target;
static int       g_held;
static float     g_K = K_INIT, g_prevError = -1.0f, g_remX, g_remY;
static int       g_growSteps, g_slowSteps, g_floorHits, g_logSteps, g_stallX, g_stallY, g_stepsSinceRetry;
static LONG      g_lastAssistX, g_lastAssistY;
static LONGLONG  g_noTargetLogAt;

static void ReleaseAim(void)
{
    if (g_held)
    {
        Log("release (gain %.6f,%.6f, updates %d,%d)", g_axis[0].gain, g_axis[1].gain,
            g_axis[0].updates, g_axis[1].updates);
        if (g_axis[0].updates >= 10 && g_axis[1].updates >= 10)
            SaveGains();
    }
    g_held = 0;
    g_target = 0;
    g_prevError = -1.0f;
    g_K = K_INIT;
    g_growSteps = g_slowSteps = g_floorHits = g_logSteps = 0;
    g_stallX = g_stallY = g_stepsSinceRetry = 0;
    g_lastAssistX = g_lastAssistY = 0;
    g_remX = g_remY = 0.0f;
}

static void RunAim(LONG *lx, LONG *ly, uintptr_t world, uintptr_t player, Vector3 cameraPosition,
                   Vector3 cameraForward, float yaw, float pitch, float turnYaw, float turnPitch,
                   LONGLONG now)
{
    Vector3 targetPosition, direction;
    float errYaw, errPitch, error, cx, cy, brakeX, brakeY;
    int blockX, blockY;
    LONG ix = 0, iy = 0;
    LONG prevAssistX = g_lastAssistX, prevAssistY = g_lastAssistY;

    g_lastAssistX = g_lastAssistY = 0;
    if (g_axis[0].gain == 0.0 || g_axis[1].gain == 0.0)
        return;

    if (!g_target || !TargetStillValid(world, player, g_target, &targetPosition))
    {
        g_target = FindNearestPed(world, player, cameraPosition, cameraForward, &targetPosition);
        g_prevError = -1.0f;
        g_stallX = g_stallY = 0;
        if (!g_target)
        {
            if (now > g_noTargetLogAt)
            {
                Log("no target in view");
                g_noTargetLogAt = now + 1500;
            }
            return;
        }
        Log("target 0x%08lX at %.1f %.1f %.1f", (unsigned long)g_target, targetPosition.x,
            targetPosition.y, targetPosition.z);
    }

    targetPosition.y += g_aimHeight;
    direction = Subtract(targetPosition, cameraPosition);
    if (!Normalize(&direction))
        return;
    errYaw = WrapAngle(atan2f(direction.x, direction.z) - yaw);
    errPitch = asinf(ClampUnit(direction.y)) - pitch;
    brakeX = fabsf(errYaw) / AIM_BRAKE_ANGLE;
    brakeY = fabsf(errPitch) / AIM_BRAKE_ANGLE;
    if (brakeX < AIM_BRAKE_FLOOR) brakeX = AIM_BRAKE_FLOOR;
    if (brakeY < AIM_BRAKE_FLOOR) brakeY = AIM_BRAKE_FLOOR;
    if (brakeX > 1.0f) brakeX = 1.0f;
    if (brakeY > 1.0f) brakeY = 1.0f;

    /* An axis that does not move although we push it is at a hard limit: stop pushing it. */
    if (labs(prevAssistX) >= 40 && fabsf(turnYaw) < 0.0006f) ++g_stallX; else g_stallX = 0;
    if (labs(prevAssistY) >= 40 && fabsf(turnPitch) < 0.0006f) ++g_stallY; else g_stallY = 0;
    if (++g_stepsSinceRetry > 90)
    {
        g_stepsSinceRetry = 0;
        g_stallX = g_stallY = 0;
    }
    blockX = g_stallX >= 6;
    blockY = g_stallY >= 6;
    error = sqrtf((blockX ? 0.0f : errYaw * errYaw) + (blockY ? 0.0f : errPitch * errPitch));

    if (g_prevError >= 0.0f)
    {
        if (error > g_prevError * 1.08f + 0.002f)
        {
            if (++g_growSteps >= 3)
            {
                g_growSteps = 0;
                g_K *= 0.6f;
                if (g_K < K_MIN)
                {
                    g_K = K_MIN;
                    if (++g_floorHits >= 3)
                    {
                        Log("controller diverging at minimum gain: back to seed sensitivity");
                        SeedGains();
                        g_floorHits = 0;
                        return;
                    }
                }
            }
        }
        else
        {
            g_growSteps = 0;
        }
        if (error > g_prevError * 0.93f && error > 0.01f && g_growSteps == 0)
        {
            if (++g_slowSteps >= 6)
            {
                g_slowSteps = 0;
                g_K = g_K * 1.25f > K_MAX ? K_MAX : g_K * 1.25f;
            }
        }
        else
        {
            g_slowSteps = 0;
        }
    }
    g_prevError = error;

    if (error < DEADZONE_RAD)
    {
        g_remX = g_remY = 0.0f;
        return;
    }

    cx = blockX ? 0.0f : brakeX * g_K * (float)g_cfg.aimResponse * 0.01f *
        errYaw / (float)g_axis[0].gain + g_remX;
    cy = blockY ? 0.0f : brakeY * g_K * (float)g_cfg.aimResponse * 0.01f *
        errPitch / (float)g_axis[1].gain + g_remY;
    ix = (LONG)floorf(cx + 0.5f);
    iy = (LONG)floorf(cy + 0.5f);
    g_remX = cx - (float)ix;
    g_remY = cy - (float)iy;
    if (ix > MAX_STEP_COUNTS) { ix = MAX_STEP_COUNTS; g_remX = 0.0f; }
    if (ix < -MAX_STEP_COUNTS) { ix = -MAX_STEP_COUNTS; g_remX = 0.0f; }
    if (iy > MAX_STEP_COUNTS) { iy = MAX_STEP_COUNTS; g_remY = 0.0f; }
    if (iy < -MAX_STEP_COUNTS) { iy = -MAX_STEP_COUNTS; g_remY = 0.0f; }
    *lx += ix;
    *ly += iy;
    g_lastAssistX = ix;
    g_lastAssistY = iy;

    if (g_logSteps < 90)
    {
        ++g_logSteps;
        Log("step err=%.2f,%.2f deg  counts=%ld,%ld  K=%.2f%s%s", errYaw * 57.29578f,
            errPitch * 57.29578f, ix, iy, g_K, blockX ? " [x blocked]" : "", blockY ? " [y blocked]" : "");
    }
}

/* ---- entry point: called for every mouse state the game reads ---------------- */

static LONGLONG g_lastCall, g_lastStep;
static int      g_haveAngles;
static float    g_prevYaw, g_prevPitch;
static LONG     g_curX, g_curY; /* counts delivered since the previous measurement */

static void HandleMouse(LONG *lx, LONG *ly)
{
    uintptr_t world, player;
    Vector3 cameraPosition, cameraForward;
    float yaw, pitch;
    LONGLONG now = NowMs();
    LONG dt = (LONG)(now - g_lastCall);
    int held;

    g_lastCall = now;
    if (dt < 1) dt = 1;
    if (dt > 50) dt = 50;

    ReloadConfig(now);
    PollPad(now);
    if (g_cfg.stickLook && g_padOk)
        AddStickLook(lx, ly, dt);

    held = AimButtonHeld();
    if (!held)
        ReleaseAim();
    else if (!g_held)
    {
        g_held = 1;
        Log("press: gain=%.6f,%.6f", g_axis[0].gain, g_axis[1].gain);
    }

    if (!GetWorld(&world, &player) || !ReadCamera(&cameraPosition, &cameraForward))
    {
        g_haveAngles = 0;
        g_curX = g_curY = 0;
        return;
    }
    yaw = atan2f(cameraForward.x, cameraForward.z);
    pitch = asinf(ClampUnit(cameraForward.y));

    if (now - g_lastStep >= MIN_STEP_MS)
    {
        float turnYaw = 0.0f, turnPitch = 0.0f;
        if (g_haveAngles)
        {
            turnYaw = WrapAngle(yaw - g_prevYaw);
            turnPitch = pitch - g_prevPitch;
            EstimateAxis(&g_axis[0], "x", turnYaw, g_curX);
            EstimateAxis(&g_axis[1], "y", turnPitch, g_curY);
        }
        g_prevYaw = yaw;
        g_prevPitch = pitch;
        g_haveAngles = 1;
        g_curX = g_curY = 0;
        g_lastStep = now;
        if (held)
            RunAim(lx, ly, world, player, cameraPosition, cameraForward, yaw, pitch, turnYaw,
                   turnPitch, now);
    }
    g_curX += *lx;
    g_curY += *ly;
}

__declspec(dllexport) void __cdecl AimInit(void)
{
    LoadGains();
    Log("logic loaded: gain=%.6f,%.6f", g_axis[0].gain, g_axis[1].gain);
}

__declspec(dllexport) void __cdecl AimMouse(LONG *lx, LONG *ly)
{
    HandleMouse(lx, ly);
}
