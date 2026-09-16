#include "PCH.h"
#include "Audio/AudioListenerProbe.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <RE/B/BSAudioManager.h>
#include <RE/N/NiPoint3.h>
#include <RE/P/PlayerCamera.h>
#include <RE/P/PlayerCharacter.h>

#include <Windows.h>

namespace DietDrCamera::AudioListenerProbe
{
    namespace
    {
        // Samples to take before reporting and shutting down for the session.
        constexpr int   kSamples           = 6;
        // A sample only counts if the camera has moved this far since the last
        // one. Correlation across MOVEMENT is the entire proof: standing still,
        // every constant in the block would "match".
        constexpr float kMinTravel         = 120.0f;
        constexpr float kSampleIntervalSec = 0.5f;
        // Real seconds of gameplay after which we report whatever we have.
        constexpr float kGiveUpSec         = 300.0f;
        // A sample matching more sites than this proves nothing; drop it.
        constexpr std::size_t kMaxSites    = 256;
        // Cap on how many survivors get named in the result line.
        constexpr std::size_t kMaxNamed    = 32;
        // How much of a pointed-to object to sweep. Generous, and read-only.
        constexpr std::size_t kObjScanBytes = 0x100;
        // Match tolerance in game units. The listener is written from the
        // camera transform, so an exact match is expected; the slack absorbs
        // our sample being a frame off the engine's own write.
        constexpr float kMatchTol = 4.0f;

        // A place a camera-shaped float triple was seen. slot < 0 means it sat
        // directly in the BSAudioManager block; otherwise it sat inside the
        // object pointed to by BSAudioManager+slot.
        struct Site
        {
            int         slot;    // byte offset of the owning pointer, or -1
            std::size_t off;     // byte offset of the triple within its object
            bool operator==(const Site& o) const { return slot == o.slot && off == o.off; }
        };

        std::vector<Site> s_candidates;
        bool              s_seeded      = false;
        int               s_sampleCount = 0;
        bool              s_done        = false;
        float             s_timer       = 0.0f;
        float             s_alive       = 0.0f;
        RE::NiPoint3      s_lastSamplePos{};
        bool              s_haveLastPos = false;

        // Copy under structured exception handling. VirtualQuery below is a
        // cheap pre-filter, but it is a check-then-use race: the game's audio
        // and streaming threads are live, and a page that was MEM_COMMIT when
        // we asked can be gone by the time we read. Since we hand ~50 arbitrary
        // qwords per sample to the pointer test, "almost certainly readable" is
        // not good enough — this is the only path that touches an address we
        // did not derive from a typed object, so it gets a real net under it.
        //
        // Its own function, no C++ objects: MSVC rejects __try in a frame that
        // needs unwinding, and noinline keeps the handler from being merged
        // into a caller that does.
        __declspec(noinline) bool SafeCopy(const void* a_src, void* a_dst, std::size_t a_n) noexcept
        {
            __try {
                std::memcpy(a_dst, a_src, a_n);
                return true;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                return false;
            }
        }

        bool SafeReadable(const void* a_p, std::size_t a_n)
        {
            if (!a_p) return false;
            const auto addr = reinterpret_cast<std::uintptr_t>(a_p);
            if (addr < 0x10000) return false;
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(a_p, &mbi, sizeof(mbi)) != sizeof(mbi)) return false;
            if (mbi.State != MEM_COMMIT) return false;
            // PAGE_EXECUTE is execute-ONLY (0x10); the readable execute
            // protections are distinct bits, so this can't reject them.
            constexpr DWORD kNoRead = PAGE_NOACCESS | PAGE_GUARD | PAGE_EXECUTE;
            if (mbi.Protect & kNoRead) return false;
            const auto regionEnd =
                reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            return addr + a_n <= regionEnd;
        }

        bool Near(float a, float b) { return std::abs(a - b) <= kMatchTol; }

        // Every float triple in [base, base+len) that equals the camera
        // position this frame.
        void CollectTriples(const std::uint8_t* a_base, std::size_t a_len, int a_slot,
                            const RE::NiPoint3& a_cam, std::vector<Site>& a_out)
        {
            for (std::size_t off = 0; off + 3 * sizeof(float) <= a_len; off += sizeof(float)) {
                float f[3];
                std::memcpy(f, a_base + off, sizeof(f));
                if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) continue;
                if (Near(f[0], a_cam.x) && Near(f[1], a_cam.y) && Near(f[2], a_cam.z))
                    a_out.push_back(Site{ a_slot, off });
            }
        }

        std::string Describe(const Site& s)
        {
            return s.slot < 0 ? fmt::format("BSAudioManager+{:#x}", s.off)
                              : fmt::format("[BSAudioManager+{:#x}]+{:#x}",
                                            static_cast<std::size_t>(s.slot), s.off);
        }

        void Sample(const RE::NiPoint3& a_cam, const RE::NiPoint3& a_player)
        {
            auto* am = RE::BSAudioManager::GetSingleton();
            if (!am) return;
            const auto* base = reinterpret_cast<const std::uint8_t*>(am);

            // Everything below works off private SNAPSHOTS, never off live
            // memory: one guarded copy in, then all the scanning happens on our
            // own stack where nothing can be unmapped underneath it.
            std::uint8_t mgr[sizeof(RE::BSAudioManager)];
            if (!SafeReadable(base, sizeof(mgr)) || !SafeCopy(base, mgr, sizeof(mgr))) return;

            std::vector<Site> hits;

            // Level 0 — the manager block itself.
            CollectTriples(mgr, sizeof(mgr), -1, a_cam, hits);

            // Level 1 — one hop through every qword in the block that turns out
            // to be a readable pointer. The listener is an object the manager
            // owns, not a field of it, so the position almost certainly lives
            // one dereference away.
            for (std::size_t slot = 0; slot + sizeof(void*) <= sizeof(mgr);
                 slot += sizeof(void*))
            {
                std::uintptr_t cand = 0;
                std::memcpy(&cand, mgr + slot, sizeof(cand));
                if (cand < 0x10000 || (cand & 7) != 0) continue;
                const auto* obj = reinterpret_cast<const std::uint8_t*>(cand);
                if (!SafeReadable(obj, kObjScanBytes)) continue;
                std::uint8_t buf[kObjScanBytes];
                if (!SafeCopy(obj, buf, sizeof(buf))) continue;
                CollectTriples(buf, sizeof(buf), static_cast<int>(slot), a_cam, hits);
            }
            // A first sample taken somewhere degenerate (near the origin, say)
            // could seed thousands of sites and turn the intersection below into
            // a needless O(n*m). The listener is one field; a sample that names
            // half the heap is not evidence, so throw it away rather than carry
            // it.
            if (hits.size() > kMaxSites) {
                spdlog::info("[AUDIO] probe: discarding a sample with {} matches "
                             "(camera position is too unremarkable to prove anything)",
                             hits.size());
                return;
            }

            if (!s_seeded) {
                s_candidates = hits;
                s_seeded     = true;
            } else {
                std::vector<Site> keep;
                keep.reserve(s_candidates.size());
                for (const auto& c : s_candidates) {
                    for (const auto& h : hits) {
                        if (h == c) { keep.push_back(c); break; }
                    }
                }
                s_candidates.swap(keep);
            }

            // The cheap hypothesis, reported alongside: PlayerCamera::pos is a
            // declared field bracketed by identified members, so writing it
            // carries no layout risk at all. If it tracks the camera it is by
            // far the least invasive place to put a clamped listener position.
            const char* pcPosVerdict = "n/a";
            if (auto* pc = RE::PlayerCamera::GetSingleton()) {
                const auto& p = pc->pos;
                if (Near(p.x, a_cam.x) && Near(p.y, a_cam.y) && Near(p.z, a_cam.z))
                    pcPosVerdict = "TRACKS-CAMERA";
                else if (Near(p.x, a_player.x) && Near(p.y, a_player.y))
                    pcPosVerdict = "tracks-player";
                else
                    pcPosVerdict = "unrelated";
            }

            const float dx = a_cam.x - a_player.x;
            const float dy = a_cam.y - a_player.y;
            const float dz = a_cam.z - a_player.z;

            ++s_sampleCount;
            spdlog::info("[AUDIO] probe sample {}/{}: cam=({:.0f},{:.0f},{:.0f}) "
                         "camDist={:.0f} hits={} survivors={} PlayerCamera::pos={}",
                         s_sampleCount, kSamples, a_cam.x, a_cam.y, a_cam.z,
                         std::sqrt(dx * dx + dy * dy + dz * dz),
                         hits.size(), s_candidates.size(), pcPosVerdict);
        }

        void Report()
        {
            s_done = true;
            if (s_candidates.empty()) {
                spdlog::info("[AUDIO] probe RESULT: nothing reachable from BSAudioManager "
                             "(directly or one dereference deep) tracked the camera across "
                             "all {} moving samples. The listener is not a plain float triple "
                             "there — see the PlayerCamera::pos verdicts above, otherwise the "
                             "setter has to be hooked.", kSamples);
                return;
            }
            std::string list;
            std::size_t named = 0;
            for (const auto& c : s_candidates) {
                if (named++ >= kMaxNamed) {
                    list += fmt::format(", +{} more", s_candidates.size() - kMaxNamed);
                    break;
                }
                if (!list.empty()) list += ", ";
                list += Describe(c);
            }
            spdlog::info("[AUDIO] probe RESULT: {} site(s) tracked the camera across all {} "
                         "moving samples: {}. Writing a player-clamped position to one of "
                         "these is the fix for far-zoom audio.",
                         s_candidates.size(), kSamples, list);
        }
    }

    void Update()
    {
        if (s_done) return;

        auto* pc = RE::PlayerCamera::GetSingleton();
        auto* pl = RE::PlayerCharacter::GetSingleton();
        if (!pc || !pc->cameraRoot || !pl) return;

        // Real time, not game time: this must keep working while slowed and
        // must not share a clock with anything the camera does.
        using clock = std::chrono::steady_clock;
        static clock::time_point sLast = clock::now();
        const auto  now = clock::now();
        const float dt  = std::chrono::duration<float>(now - sLast).count();
        sLast = now;
        // Give up eventually. The travel gate below means a player who never
        // moves the camera far would otherwise leave the probe armed for the
        // whole session and never print a conclusion — an inconclusive result
        // still has to reach the log, or the next session can't tell "found
        // nothing" apart from "never ran".
        s_alive += (dt > 0.0f && dt < 1.0f) ? dt : 0.0f;
        if (s_alive > kGiveUpSec) {
            if (s_sampleCount >= 2) {
                Report();
            } else {
                s_done = true;
                spdlog::info("[AUDIO] probe INCONCLUSIVE: only {} usable sample(s) in {:.0f}s — "
                             "the camera never moved {:.0f}+ units between samples, so nothing "
                             "could be correlated.", s_sampleCount, s_alive, kMinTravel);
            }
            return;
        }

        s_timer += (dt > 0.0f && dt < 1.0f) ? dt : 0.0f;
        if (s_timer < kSampleIntervalSec) return;
        s_timer = 0.0f;

        const RE::NiPoint3 cam    = pc->cameraRoot->world.translate;
        const RE::NiPoint3 player = pl->GetPosition();

        if (s_haveLastPos) {
            const float dx = cam.x - s_lastSamplePos.x;
            const float dy = cam.y - s_lastSamplePos.y;
            const float dz = cam.z - s_lastSamplePos.z;
            if (dx * dx + dy * dy + dz * dz < kMinTravel * kMinTravel) return;
        }
        s_lastSamplePos = cam;
        s_haveLastPos   = true;

        Sample(cam, player);
        if (s_sampleCount >= kSamples) Report();
    }
}
