#pragma once

#include <atomic>
#include <cstdint>

#include <RE/B/BSTEvent.h>
#include <RE/I/InputEvent.h>

namespace DietDrCamera
{
    // High bit tag stuffed into stored hotkey codes to flag "this is an
    // XInput gamepad button mask, not a keyboard/mouse scancode". XInput
    // masks (0x0100 = LB, etc.) collide with keyboard/mouse codes, so
    // we OR this tag in at storage time and strip it at compare time.
    inline constexpr std::uint32_t kInputGamepadTag = 0x10000000u;

    // Input event sink that handles dialogue preset cycling hotkeys.
    // Listens on the global input device manager, fires only while the
    // Dialogue Menu is open and matches the user-configured scancodes.
    //
    // Also supports a "capture mode" used by the UI: when the user clicks
    // an "Assign Hotkey" button, the UI calls BeginCapture(targetField)
    // and the next keydown writes its scancode into that field instead
    // of triggering the cycle. This avoids needing a separate text input
    // for hex codes — user just presses the key they want.
    class DialogueInputSink : public RE::BSTEventSink<RE::InputEvent*>
    {
    public:
        static DialogueInputSink& GetSingleton();

        // Register the sink with the global input device manager. Call
        // once at plugin load (post-load message).
        static void Install();

        // Apply a Ready Weapon press that arrived DURING dialogue. The
        // toggle is deferred to the dialogue-close edge because changing
        // weapon state mid-conversation advances the dialogue (2026-08-16);
        // called from the dialogue-close handler. No-op when nothing is
        // pending.
        static void ApplyPendingSheathe();

        // BSTEventSink<InputEvent*> implementation.
        RE::BSEventNotifyControl ProcessEvent(
            RE::InputEvent* const* a_event,
            RE::BSTEventSource<RE::InputEvent*>* a_source) override;

        // Capture mode: the next keydown will write its scancode to
        // *target instead of triggering cycle. Pass nullptr to cancel.
        void BeginCapture(std::uint32_t* target);

        bool IsCapturing() const { return _captureTarget.load() != nullptr; }

    private:
        DialogueInputSink() = default;

        // Atomic so the UI thread (BeginCapture) and the input thread
        // (ProcessEvent) don't race on the target pointer.
        std::atomic<std::uint32_t*> _captureTarget{nullptr};
    };
}
