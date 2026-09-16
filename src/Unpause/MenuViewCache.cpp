#include "PCH.h"
#include "Unpause/MenuViewCache.h"

#include "Settings/SettingsManager.h"

#include <RE/G/GFxMovieDef.h>
#include <RE/G/GFxMovieDefImpl.h>
#include <RE/G/GFxMovieView.h>

#include <REL/Relocation.h>
#include <SKSE/SKSE.h>

#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace DietDrCamera::MenuViewCache
{
    namespace
    {
        // Lowercased SWF URLs we participate in. HUD/Console/loadscreen
        // intentionally absent — pre-warming those risks reusing a view
        // whose ActionScript timers were already running on the worker
        // thread. The menus listed below are all on the same swap path
        // as Inventory↔Magic and are the ones the user opens repeatedly
        // in a session.
        constexpr std::string_view kAllowlist[] = {
            "interface/inventorymenu.swf",
            "interface/magicmenu.swf",
            "interface/containermenu.swf",
            "interface/bartermenu.swf",
            "interface/tweenmenu.swf",
            "interface/favoritesmenu.swf",
        };

        bool IsAllowlisted(std::string_view a_lowered)
        {
            for (auto& entry : kAllowlist) {
                if (a_lowered == entry) return true;
            }
            return false;
        }

        // Hot map of pre-warmed views. Keyed by lowercased SWF URL.
        // SkyrimSouls's upstream uses unordered_map<string, future<View*>>
        // and accesses it from the main thread only — CreateInstance is
        // called from menu construction which always runs on the main
        // thread. We keep the same single-threaded access pattern; the
        // mutex below only protects the kPreLoadGame drain path.
        std::unordered_map<std::string, std::future<RE::GFxMovieView*>> sViewsCache;
        std::mutex                                                       sDrainMutex;
        bool                                                             sInstalled = false;

        // Block until every in-flight pre-warm worker finishes, WITHOUT
        // discarding the results (unlike ClearAndDrain). Used to serialize
        // Scaleform access: no worker may be constructing a GFxMovieView on a
        // background thread while the MAIN thread builds a non-allowlisted movie
        // (HUD / Console / loadscreen). The acute case is the HUD rebuild on
        // FAST TRAVEL — which does NOT fire kPreLoadGame, so ClearAndDrain never
        // runs — letting a worker race the HUD construction and corrupt it
        // (HUD fails to appear, activation dies). GFx is not thread-safe.
        void DrainWorkersKeepResults()
        {
            std::lock_guard<std::mutex> guard{ sDrainMutex };
            for (auto& kv : sViewsCache) {
                if (kv.second.valid()) kv.second.wait();
            }
        }

        // Trampoline original: the engine's GFxMovieDef::CreateInstance
        // call we replaced with our hook. Re-invoked for both the
        // synchronous return and the async pre-warm.
        using CreateInstance_t = RE::GFxMovieView* (*)(
            RE::GFxMovieDefImpl*,
            const RE::GFxMovieDef::MemoryParams&,
            bool);

        RE::GFxMovieView* CreateInstance_Hook(
            RE::GFxMovieDefImpl*                 a_this,
            const RE::GFxMovieDef::MemoryParams& a_memParams,
            bool                                 a_initFirstFrame)
        {
            if (!a_this) {
                return nullptr;
            }

            // GetFileURL is virtual slot 0x0C on GFxMovieDef and returns
            // a stable C string for the SWF this def loaded from. Lower
            // it for case-insensitive comparison against the allowlist.
            std::string url = a_this->GetFileURL();
            for (char& c : url) {
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }

            // Top-level probe with URL — confirms what string the
            // allowlist needs to match on this runtime. Slash style,
            // prefix, and casing all vary across engine builds.
            spdlog::debug("[DIAG-cache] HOOK-ENTRY url='{}' firstFrame={}",
                         url, a_initFirstFrame);

            // Non-allowlisted SWF — bypass the cache entirely and call
            // the engine's CreateInstance directly. Critical: do NOT
            // touch sViewsCache for unknown URLs; HUD and Console run
            // through this path too and we don't want their state in
            // our map.
            if (!IsAllowlisted(url)) {
                // This movie (HUD / Console / loadscreen, etc.) is built on the
                // MAIN thread. Our pre-warm workers build views on BACKGROUND
                // threads, and Scaleform is not thread-safe — a worker running
                // right now can corrupt this movie. Most visible on the
                // fast-travel HUD rebuild (no kPreLoadGame -> ClearAndDrain is
                // skipped), matching the "HUD gone + can't activate after fast
                // travel" report. Wait out any in-flight workers first so GFx
                // access is serialized, then build normally (warmed results kept).
                DrainWorkersKeepResults();
                return a_this->CreateInstance(a_memParams, a_initFirstFrame);
            }

            std::lock_guard<std::mutex> guard{ sDrainMutex };

            auto it = sViewsCache.find(url);
            if (it != sViewsCache.end()) {
                it->second.wait();
                if (it->second.valid()) {
                    RE::GFxMovieView* asyncView = it->second.get();
                    sViewsCache.erase(it);
                    spdlog::debug("[DIAG-cache] HIT url='{}' returning warm view", url);

                    // Re-prime for the NEXT open. AddRef so the worker
                    // can keep this GFxMovieDef alive past our return;
                    // Release inside the lambda once construction is
                    // done. (Matches the async path in SkyrimSouls's
                    // MenuCache.cpp:35-42.)
                    a_this->AddRef();
                    std::future<RE::GFxMovieView*> nextPrime =
                        std::async(std::launch::async,
                                   [a_this, a_memParams, a_initFirstFrame]() {
                                       auto* view = a_this->CreateInstance(a_memParams, a_initFirstFrame);
                                       a_this->Release();
                                       return view;
                                   });
                    sViewsCache[url] = std::move(nextPrime);
                    return asyncView;
                }
                // Future was invalidated somehow — fall through to a
                // fresh sync construction without leaving a dead slot
                // behind.
                sViewsCache.erase(it);
            } else {
                // First miss for this URL in this session — prime the
                // slot in the background AND fall through to a sync
                // construction so the caller still gets a view this
                // tick. From the second open onward we'll take the HIT
                // branch above.
                a_this->AddRef();
                std::future<RE::GFxMovieView*> firstPrime =
                    std::async(std::launch::async,
                               [a_this, a_memParams, a_initFirstFrame]() {
                                   auto* view = a_this->CreateInstance(a_memParams, a_initFirstFrame);
                                   a_this->Release();
                                   return view;
                               });
                sViewsCache[url] = std::move(firstPrime);
                spdlog::debug("[DIAG-cache] MISS-PRIME url='{}' building sync + async", url);
            }

            RE::GFxMovieView* syncView = a_this->CreateInstance(a_memParams, a_initFirstFrame);
            return syncView;
        }
    }

    void Install()
    {
        if (sInstalled) return;
        // SkyrimSouls's hook site: REL::ID(82325).address() + 0x1B8. The
        // 6-byte write_call slot is from upstream; the displaced call
        // is the engine's invocation of GFxMovieDef::CreateInstance
        // during menu construction.
        auto& trampoline = SKSE::GetTrampoline();
        trampoline.write_call<6>(
            REL::ID(82325).address() + 0x1B8,
            reinterpret_cast<std::uintptr_t>(CreateInstance_Hook));
        sInstalled = true;
        spdlog::warn("[MenuViewCache] EXPERIMENTAL pre-warmer installed at REL::ID(82325)+0x1B8");
    }

    void ClearAndDrain()
    {
        std::lock_guard<std::mutex> guard{ sDrainMutex };
        // Wait for each outstanding worker so the GFxMovieDef AddRef it
        // holds is balanced by the Release inside the lambda — calling
        // .get() drains the future. We discard the returned view; the
        // engine never registered it, so its ref-1 from CreateInstance
        // drops to zero when the GPtr/temporary dies here. (If the
        // GFxMovieView is leaked it's bounded by allowlist size — five
        // entries — and disappears on process exit.)
        for (auto& kv : sViewsCache) {
            try {
                kv.second.wait();
                if (kv.second.valid()) {
                    auto* view = kv.second.get();
                    (void)view;  // ref drops via temporary destruction
                }
            } catch (...) {
                // Worker exception is best-effort; we still want to
                // clear the slot.
            }
        }
        const auto n = sViewsCache.size();
        sViewsCache.clear();
        spdlog::info("[MenuViewCache] drained {} pre-warmed view(s) on save-load", n);
    }
}
