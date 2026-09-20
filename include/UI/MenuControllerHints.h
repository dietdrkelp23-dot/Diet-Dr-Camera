#pragma once

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace DietDrCamera::MenuControllerHints
{
    enum class Mode { Sidebar, Page, Popup, Slider, Binding, TextInput };
    enum class Slot { Navigation, Confirm, Back, Alternate, Scroll, Copy, Paste, QuickTune, Count };
    inline constexpr auto SlotCount = static_cast<std::size_t>(Slot::Count);
    // Font Awesome Solid arrows from the framework's own icon font.
    inline constexpr auto ArrowSymbols = "\xef\x81\xa0 \xef\x81\xa2 \xef\x81\xa3 \xef\x81\xa1";

    // Shared with the hover targets, so the footer's fitting budget includes
    // every label without depending on the item under the cursor this frame.
    namespace ClipboardLabels
    {
        inline constexpr auto Entry = "Entry";
        inline constexpr auto Transition = "Transition Override";
        inline constexpr auto Location = "Location Override";
        inline constexpr auto Weapon = "Weapon Type Overrides";
        inline constexpr auto Enemy = "Enemy Override";
        inline constexpr auto Shout = "Shout Settings";
        inline constexpr auto ShoutNoise = "Shout Noise";
        inline constexpr auto Dialogue = "Dialogue Look";
        inline constexpr auto Transformation = "Transformation Shake";
        inline constexpr auto Environment = "Environment Settings";
        inline constexpr auto Tab = "Entire Tab";
        inline constexpr auto Indoor = "Indoor Settings";
        inline constexpr auto Outdoor = "Outdoor Settings";
        inline constexpr auto Noise = "Noise Settings";
        inline constexpr std::array All{Entry, Transition, Location, Weapon, Enemy,
            Shout, ShoutNoise, Dialogue, Transformation, Environment, Tab, Indoor, Outdoor, Noise};
    }

    struct Hint
    {
        Slot slot;
        std::string keys;
        std::string action;
        bool symbolKeys = false;
        bool operator==(const Hint&) const = default;
    };

    struct Context
    {
        Mode mode = Mode::Page;
        bool tabs = false;
        bool playStation = false;
        bool keyboard = false;
        // Empty when the current controller item cannot be copied/pasted.
        std::string_view clipboardSubject;
        // A Paste binding alone does not mean the menu clipboard has a payload.
        bool clipboardHasContents = false;
        // Empty means unbound. The caller resolves these from live settings,
        // using the same key-name function as the binding buttons.
        std::string copy, paste, quickTune;
    };

    inline std::string PlayStationKeyName(std::string name)
    {
        if (name == "A") return "Cross";
        if (name == "B") return "Circle";
        if (name == "X") return "Square";
        if (name == "Y") return "Triangle";
        if (name == "LB") return "L1";
        if (name == "RB") return "R1";
        if (name == "L Stick") return "L3";
        if (name == "R Stick") return "R3";
        if (name == "Start") return "Options";
        if (name == "Back") return "Share";
        return name;
    }

    inline std::string BindingName(std::string name, bool gamepad, bool playStation)
    {
        if (gamepad) {
            if (playStation) return PlayStationKeyName(std::move(name));
            if (name == "L Stick") return "L-stick click";
            if (name == "R Stick") return "R-stick click";
            return name;
        }
        // A keyboard X must not look like a controller X in the pad footer.
        return name.starts_with("Key ") ? name : "Key " + name;
    }

    inline std::vector<Hint> Build(const Context& context)
    {
        if (context.keyboard) {
            if (context.mode == Mode::Binding || context.mode == Mode::TextInput) return {};
            std::vector<Hint> hints{{Slot::Navigation, ArrowSymbols,
                context.mode == Mode::Slider ? "Adjust value" : "Move cursor", true},
                {Slot::Confirm, "Enter", context.mode == Mode::Slider ? "Keep changes" : "Select / edit"}};
            if (!context.clipboardSubject.empty()) {
                if (!context.copy.empty()) hints.push_back({Slot::Copy, context.copy, "Copy " + std::string(context.clipboardSubject)});
                if (context.clipboardHasContents && !context.paste.empty())
                    hints.push_back({Slot::Paste, context.paste, "Paste " + std::string(context.clipboardSubject)});
            }
            if (!context.quickTune.empty() && (context.mode == Mode::Page || context.mode == Mode::Sidebar))
                hints.push_back({Slot::QuickTune, context.quickTune, "Open Quick Tune"});
            return hints;
        }
        const char* confirm = context.playStation ? "Cross" : "A";
        const char* cancel = context.playStation ? "Circle" : "B";
        const char* bumpers = context.playStation ? "L1 / R1" : "LB / RB";
        if (context.mode == Mode::Binding)
            return {{Slot::Confirm, "Key / Button", "Set binding"}, {Slot::Back, "Esc", "Clear binding"}};
        if (context.mode == Mode::TextInput)
            return {{Slot::Confirm, "Keyboard", "Enter text"}, {Slot::Back, "Esc", "Cancel editing"}};
        if (context.mode == Mode::Sidebar)
            return {{Slot::Navigation, "D-pad Up / Down", "Browse sections"}, {Slot::Confirm, confirm, "Open section"},
                    {Slot::Alternate, "D-pad Right", "Enter right pane"}};
        if (context.mode == Mode::Slider)
            return {{Slot::Navigation, "D-pad Left / Right", "Adjust value"}, {Slot::Confirm, confirm, "Keep changes"},
                    {Slot::Back, cancel, "Cancel changes"}, {Slot::Alternate, bumpers, "Larger steps (10x)"}};

        std::vector<Hint> hints{{Slot::Navigation, "D-pad", "Move cursor"}, {Slot::Confirm, confirm, "Select / edit"},
            {Slot::Back, cancel, context.mode == Mode::Popup ? "Close popup" : "Back to sections"}};
        if (context.tabs) hints.push_back({Slot::Alternate, bumpers, "Switch tabs"});
        hints.push_back({Slot::Scroll, context.playStation ? "L2 / R2 / Right stick" : "LT / RT / Right stick", "Scroll up / down"});
        if (!context.clipboardSubject.empty()) {
            if (!context.copy.empty()) hints.push_back({Slot::Copy, context.copy, "Copy " + std::string(context.clipboardSubject)});
            if (context.clipboardHasContents && !context.paste.empty())
                hints.push_back({Slot::Paste, context.paste, "Paste " + std::string(context.clipboardSubject)});
        }
        if (!context.quickTune.empty() && context.mode == Mode::Page)
            hints.push_back({Slot::QuickTune, context.quickTune, "Open Quick Tune"});
        return hints;
    }

    inline std::vector<Hint> LayoutCandidates(Context context)
    {
        // Only binding/device settings affect these measurements. Hover, popup,
        // slider, tab and clipboard state keep the same font size and rows.
        context.tabs = true;
        context.clipboardSubject = {};
        if (context.copy.empty()) context.copy = "X";
        if (context.paste.empty()) context.paste = "Y";
        if (context.quickTune.empty()) context.quickTune = "Back";
        std::vector<Hint> candidates;
        for (const auto mode : {Mode::Sidebar, Mode::Page, Mode::Popup, Mode::Slider, Mode::Binding, Mode::TextInput}) {
            context.mode = mode;
            for (auto& hint : Build(context)) candidates.push_back(std::move(hint));
        }
        for (const auto* subject : ClipboardLabels::All) {
            candidates.push_back({Slot::Copy, context.copy, "Copy " + std::string(subject)});
            candidates.push_back({Slot::Paste, context.paste, "Paste " + std::string(subject)});
        }
        return candidates;
    }
}
