#pragma once

namespace DietDrCamera::MenuViewCache
{
    // Install the GFxMovieDef::CreateInstance pre-warmer hook. No-op
    // on second call.
    //
    // Hook target: REL::ID(82325).address() + 0x1B8 (SE) — the
    // call-site that invokes GFxMovieDef::CreateInstance during menu
    // construction. Ported from SkyrimSoulsRE/src/MenuCache.cpp.
    void Install();

    // Drain all pending futures and clear the cache. Called on
    // kPreLoadGame so a save-load doesn't strand a Scaleform view
    // built against the previous game state. The wait()/release()
    // pair on each future is required to balance the AddRef each
    // worker thread holds on the GFxMovieDef.
    void ClearAndDrain();
}
