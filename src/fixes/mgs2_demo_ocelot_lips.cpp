#include "stdafx.h"
#include "mgs2_demo_ocelot_lips.hpp"

#include "common.hpp"
#include "gamevars.hpp"
#include "game_stages.hpp"
#include "helper.hpp"
#include "logging.hpp"

#include <cmath>

namespace
{
    // demo_mtn.c's expanded DEMO_MOTION, one per object; the object actor reads it every frame.
    struct DemoMotion
    {
        uint8_t header[0x10];
        int objectId, motionType, startJoint, nJoints;
        float pos[4];
        int16_t rot[4];
        int pad[2];
        float motion[1][4];     // per joint: quat xyzw, then translation
    };

    constexpr int kOcelot = 49;
    constexpr int kJoints = 88;
    constexpr int kMouth[] = { 22, 23, 24, 25, 34, 35, 36, 37, 42 };    // lips, mouth corners, jaw
    constexpr int kRestFirst = 280;     // mouth shut, just before the line
    constexpr int kFirst = 295;
    constexpr int kLast = 421;          // back at rest
    constexpr float kGain = 3.0f;       // brings it level with his other lines

    // "If you wish to" holds one small mouth shape with the jaw shut, so gain alone leaves it still.
    // Four syllable beats open the jaw and drop the lower lip as far as his other lines do.
    constexpr int kBeatFirst = 300;
    constexpr int kBeatLast = 331;
    constexpr float kBeats = 4.0f;
    constexpr float kJawBeat = 7.0f;
    constexpr float kLipBeat = 5.0f;

    struct Joint { float quat[4]; float trans[4]; };
    Joint gRest[std::size(kMouth)];
    Joint gTrue[std::size(kMouth)];     // what the demo wrote, before we enlarged it
    bool gHaveRest = false;
    bool gEnlarged = false;

    DemoMotion* (*DM_GetMotionData)(int objectId) = nullptr;

    Joint* At(DemoMotion* m, int joint)
    {
        return reinterpret_cast<Joint*>(m->motion[joint * 2]);
    }

    void Mul(float out[4], const float a[4], const float b[4])
    {
        const float r[4] = {
            a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
        };
        memcpy(out, r, sizeof(r));
    }

    // rest * (rest^-1 * q)^gain: the same turn away from rest, only bigger.
    void Enlarge(Joint& out, const Joint& rest, const Joint& now, float turnGain, float slideGain)
    {
        const float inv[4] = { -rest.quat[0], -rest.quat[1], -rest.quat[2], rest.quat[3] };
        float d[4];
        Mul(d, inv, now.quat);
        if (d[3] < 0.0f)
        {
            for (float& c : d) c = -c;
        }

        const float s = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (s > 1e-6f)
        {
            const float half = std::atan2(s, d[3]) * turnGain;
            const float k = std::sin(half) / s;
            const float big[4] = { d[0] * k, d[1] * k, d[2] * k, std::cos(half) };
            Mul(out.quat, rest.quat, big);
        }
        for (int c = 0; c < 3; c++)
        {
            out.trans[c] = rest.trans[c] + (now.trans[c] - rest.trans[c]) * slideGain;
        }
    }

    SafetyHookInline h_ExecDemoStream{};
    void ExecDemoStream_hook(void* work, uint8_t* stream, int exec)
    {
        const bool ours = g_GameVars.IsStage(MGS2Stages::D12T3);       // t12a3d.sdt
        const int frame = *reinterpret_cast<int*>(stream - 8) / 5;      // STREAM_TAG.time, 300 Hz
        DemoMotion* m = ours ? DM_GetMotionData(kOcelot) : nullptr;
        if (m && m->nJoints != kJoints)
        {
            m = nullptr;
        }

        // The packets are deltas on this buffer, so it must hold the demo's own values when they land.
        if (m && gEnlarged && frame > kFirst)
        {
            for (size_t i = 0; i < std::size(kMouth); i++)
            {
                *At(m, kMouth[i]) = gTrue[i];
            }
        }
        gEnlarged = false;

        h_ExecDemoStream.call<void>(work, stream, exec);

        if (!ours || !(m = DM_GetMotionData(kOcelot)) || m->nJoints != kJoints)
        {
            gHaveRest = false;
            return;
        }

        if (frame < kRestFirst)
        {
            gHaveRest = false;
        }
        else if (frame < kFirst)
        {
            for (size_t i = 0; i < std::size(kMouth); i++)
            {
                gRest[i] = *At(m, kMouth[i]);
            }
            gHaveRest = true;
        }
        else if (frame <= kLast && gHaveRest)
        {
            float beat = 0.0f;
            if (frame >= kBeatFirst && frame < kBeatLast)
            {
                const float t = static_cast<float>(frame - kBeatFirst) / (kBeatLast - kBeatFirst);
                beat = 0.5f - 0.5f * std::cos(t * kBeats * 6.2831853f);
            }

            for (size_t i = 0; i < std::size(kMouth); i++)
            {
                Joint* joint = At(m, kMouth[i]);
                gTrue[i] = *joint;
                const bool jaw = kMouth[i] == 42;
                const bool lowerLip = kMouth[i] >= 22 && kMouth[i] <= 25;
                Enlarge(*joint, gRest[i], gTrue[i],
                    kGain + (jaw ? kJawBeat * beat : 0.0f),
                    kGain + (lowerLip ? kLipBeat * beat : 0.0f));
            }
            gEnlarged = true;
        }
    }
}

void MGS2_DemoOcelotLips::Initialize()
{
    if (!(eGameType & MGS2) || !bEnabled)
    {
        return;
    }

    uint8_t* getMotion = Memory::PatternScan(baseModule,
        "40 53 48 83 EC ?? 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8D 90",
        "MGS 2: Ocelot Lips | demo_mtn.c -> DM_GetMotionData()");
    uint8_t* exec = Memory::PatternScan(baseModule,
        "48 89 5C 24 ?? 48 89 6C 24 ?? 48 89 74 24 ?? 57 48 83 EC ?? 33 C0 48 8D 2D",
        "MGS 2: Ocelot Lips | demo_pkt.c -> DM_ExecDemoStream()");
    if (!getMotion || !exec)
    {
        return;
    }

    DM_GetMotionData = reinterpret_cast<DemoMotion* (*)(int)>(getMotion);
    h_ExecDemoStream = safetyhook::create_inline(reinterpret_cast<void*>(exec), ExecDemoStream_hook);
    LOG_HOOK(h_ExecDemoStream, "MGS 2: Ocelot Lips | demo_pkt.c -> DM_ExecDemoStream()")
}
