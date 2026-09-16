#pragma once

// Open Animation Replacer — Animations API (vendored 2026-08-28 from
// https://github.com/ersh1/OpenAnimationReplacer, src/API/, upstream files
// OpenAnimationReplacerAPI-Animations.h/.cpp collapsed into one header-only
// unit, the same shape as TrueDirectionalMovementAPI.h next door).
//
// The one call DDC uses: GetCurrentReplacementAnimationInfo(clipGenerator)
// answers "which replacement file did OAR actually pick for this clip",
// including the sub-mod and mod names a user recognizes from their load
// order. Requested at kPostLoad; nullptr simply means OAR is not installed
// and the animation-camera feature falls back to raw clip paths.

namespace OAR_API::Animations
{
    // Available Open Animation Replacer interface versions
    enum class InterfaceVersion : uint8_t
    {
        V1,

        Latest = V1
    };

    struct ReplacementAnimationInfo
    {
        RE::BSString animationPath{};
        RE::BSString projectName{};
        RE::BSString variantFilename{};
        RE::BSString subModName{};
        RE::BSString modName{};
    };

    class IAnimationsInterface1
    {
    public:
        /// <summary>
        ///	Gets information about the current replacement animation for a clip generator.
        ///	</summary>
        ///	<param name="a_clipGenerator">The clip generator to get the information for.</param>
        ///	<returns>The information about the current replacement animation.</returns>
        [[nodiscard]] virtual ReplacementAnimationInfo GetCurrentReplacementAnimationInfo(RE::hkbClipGenerator* a_clipGenerator) noexcept = 0;

        /// <summary>
        ///	Clears all condition state data related to a clip generator.
        ///	</summary>
        /// <param name="a_clipGenerator">The clip generator to clear related condition state data for.</param>
        virtual void ClearConditionStateData(RE::hkbClipGenerator* a_clipGenerator) noexcept = 0;

        /// <summary>
        ///	Clears all condition state data related to a refr.
        ///	</summary>
        /// <param name="a_refr">The refr to clear related condition state data for.</param>
        virtual void ClearConditionStateData(RE::TESObjectREFR* a_refr) noexcept = 0;
    };

    using IAnimationsInterface = IAnimationsInterface1;

    using _RequestPluginAPI_Animations = IAnimationsInterface* (*)(InterfaceVersion a_interfaceVersion, const char* a_pluginName, REL::Version a_pluginVersion);

    /// <summary>
    /// Request the Open Animation Replacer Animations API interface.
    /// Recommended: send the request during or after SKSE's kPostLoad message.
    /// </summary>
    /// <param name="a_interfaceVersion">The interface version to request</param>
    /// <returns>The pointer to the API singleton, or nullptr if request failed</returns>
    [[nodiscard]] inline IAnimationsInterface* GetAPI(const InterfaceVersion a_interfaceVersion = InterfaceVersion::Latest)
    {
        const auto pluginHandle = GetModuleHandleA("OpenAnimationReplacer.dll");
        if (!pluginHandle) {
            return nullptr;
        }
        const auto requestAPIFunction = reinterpret_cast<_RequestPluginAPI_Animations>(
            GetProcAddress(pluginHandle, "RequestPluginAPI_Animations"));
        if (!requestAPIFunction) {
            return nullptr;
        }
        const auto* plugin = SKSE::PluginDeclaration::GetSingleton();
        return requestAPIFunction(a_interfaceVersion, plugin->GetName().data(), plugin->GetVersion());
    }
}
