#pragma once

#include <filesystem>
#include <cstdint>
#include <future>
#include <string>
#include <vector>

namespace DietDrCamera
{
    // Shared by the file browser and runtime OAR matching. Preserve the actor
    // directory so identically named replacements in different projects differ.
    std::string AnimationPathKey(std::string path);
    std::string LegacyAnimationPathKey(std::string path);
    std::string AnimationVariantKey(std::string path, const std::string& variant);
    bool AnimationSearchMatches(const std::string& text, const std::string& query);
    bool IsPlayerAnimationPath(const std::string& path);

    struct AnimationFormIdentity
    {
        std::string plugin;
        std::uint32_t localID = 0;
        std::string editorID;
    };
    struct AnimationPlayerContext
    {
        // Copied on the menu thread; the scanner never reads game objects.
        std::vector<AnimationFormIdentity> races;
        AnimationFormIdentity actorBase{"Skyrim.esm", 7, "Player"};
        AnimationFormIdentity reference{"Skyrim.esm", 0x14, "PlayerRef"};
    };

    struct CatalogAnimation
    {
        std::string key, filename, mod, submod, modKey, submodKey, search;
        bool firstPerson = false;
        bool variant = false;
    };
    struct AnimationCatalogResult
    {
        std::vector<CatalogAnimation> animations;
        std::size_t skipped = 0;
        std::string error;
    };

    // Reads the mod manager's merged Data view when called inside Skyrim.
    // Pure filesystem code: no game objects or menu calls on the scan thread.
    AnimationCatalogResult ScanAnimationCatalog(const std::filesystem::path& meshes,
        const AnimationPlayerContext& player = {});

    class AnimationCatalog
    {
    public:
        void Refresh(const std::filesystem::path& meshes, AnimationPlayerContext player = {});
        void Poll();
        bool Started() const { return _started; }
        bool Busy() const { return _job.valid(); }
        const AnimationCatalogResult& Result() const { return _result; }
        std::size_t Revision() const { return _revision; }
    private:
        bool _started = false;
        std::size_t _revision = 0;
        std::future<AnimationCatalogResult> _job;
        AnimationCatalogResult _result;
    };
}
