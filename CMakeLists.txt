cmake_minimum_required(VERSION 3.24)

project(
    DietDrCamera
    VERSION 1.2.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(DDC_DEPLOY_TO_MO2 "Deploy development builds to the configured MO2 mod" OFF)
option(DDC_BUILD_CHECKS "Build standalone camera timing and file-save checks" OFF)

# Keep __FILE__/source_location diagnostics portable in release binaries,
# including the bundled CommonLib sources compiled below.
if(MSVC)
    file(TO_NATIVE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" DDC_SOURCE_PATH)
    add_compile_options(
        $<$<CONFIG:Release>:/experimental:deterministic>
        "$<$<CONFIG:Release>:/pathmap:${DDC_SOURCE_PATH}=DietDrCamera>")
endif()
configure_file(cmake/Version.h.in generated/Core/Version.h @ONLY)
if(WIN32)
    enable_language(RC)
    configure_file(cmake/Version.rc.in generated/Version.rc @ONLY)
endif()

# ── vcpkg packages ──────────────────────────────────────────
find_package(tomlplusplus CONFIG REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ── CommonLibSSE-NG ─────────────────────────────────────────
set(BUILD_TESTS OFF CACHE BOOL "Disable CommonLibSSE-NG tests (we don't need catch2)" FORCE)
set(ENABLE_SKYRIM_SE ON CACHE BOOL "Build the SE runtime paths" FORCE)
set(ENABLE_SKYRIM_AE ON CACHE BOOL "Build the AE runtime paths" FORCE)
set(FETCHCONTENT_SOURCE_DIR_HDE64 "${CMAKE_CURRENT_SOURCE_DIR}/extern/minhook" CACHE PATH "Bundled instruction decoder" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "Non-VR Skyrim build; required for PLAYER_RUNTIME_DATA fields like unkBA0" FORCE)
add_subdirectory(extern/CommonLibSSE-NG CommonLibSSE-NG)
add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)

# ── Collect sources ─────────────────────────────────────────
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    src/*.cpp
)

# Cinematic Views is deferred. Keep its implementation for future work, but
# exclude its runtime and picker from the shipping DLL. The preset codec stays
# available so existing definitions survive loading and saving other settings.
list(REMOVE_ITEM SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Camera/CinematicViewController.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Camera/CinematicViewPicker.cpp"
)

file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS
    include/*.h
    include/*.hpp
)

# ── DLL target ──────────────────────────────────────────────
add_library(${PROJECT_NAME} SHARED
    ${SOURCES}
    ${HEADERS}
)
if(WIN32)
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/Version.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}/extern/minhook/src/hde
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    CommonLibSSE::CommonLibSSE
    tomlplusplus::tomlplusplus
    fmt::fmt
    spdlog::spdlog
    nlohmann_json::nlohmann_json
    version
)

target_precompile_headers(${PROJECT_NAME} PRIVATE
    include/PCH.h
)

# ── Auto-deploy to MO2 mod folder ───────────────────────────
# Override with -DMO2_MOD_DIR=... or SKYRIM_MOD_FOLDER env var per device.
if(NOT DEFINED MO2_MOD_DIR)
    if(DEFINED ENV{SKYRIM_MOD_FOLDER})
        set(MO2_MOD_DIR "$ENV{SKYRIM_MOD_FOLDER}")
    endif()
endif()
if(DDC_DEPLOY_TO_MO2 AND NOT MO2_MOD_DIR)
    message(FATAL_ERROR "Auto-deployment requires MO2_MOD_DIR or SKYRIM_MOD_FOLDER.")
endif()

# ── Symbols, even in Release ─────────────────────────────────
# A Release build shipped with no PDB, so every DDC frame in a
# CrashLoggerSSE dump read as a bare "DietDrCamera.dll+01A05B1" — which is
# how the 2026-08-23 dragon/Tween crash cost a round trip that a function
# name would have closed immediately. /Zi + /DEBUG only add a side file;
# the DLL keeps its optimisations and /OPT:REF,ICF undo the linker's
# debug-build default of keeping unreferenced code.
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(${PROJECT_NAME} PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FULL> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>
        $<$<CONFIG:Release>:/PDBALTPATH:DietDrCamera.pdb>)
endif()

if(DDC_DEPLOY_TO_MO2)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${MO2_MOD_DIR}/SKSE/Plugins"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_FILE:${PROJECT_NAME}>
        "${MO2_MOD_DIR}/SKSE/Plugins/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    # The PDB rides along. CrashLoggerSSE searches Data/SKSE/Plugins for it
    # (its own log says so: "Attempting to load pdb ... searchPath
    # Data/SKSE/Plugins"), so putting it there turns every future DDC frame
    # into a function and a line number.
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_PDB_FILE:${PROJECT_NAME}>"
        "${MO2_MOD_DIR}/SKSE/Plugins/$<TARGET_PDB_FILE_NAME:${PROJECT_NAME}>"
    COMMENT "Deploying $<TARGET_FILE_NAME:${PROJECT_NAME}> (+ PDB) -> ${MO2_MOD_DIR}/SKSE/Plugins/"
    VERBATIM
)

# Bundled loose assets (meshes, etc.) live under dist/ and mirror the Data
# layout. Deployed alongside the DLL so the packaged mod ships them too.
# Includes the empty meshes/actors/character/animations/staggercamera.hkx
# that disables the third-person camera-stagger lurch.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/dist")
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_CURRENT_SOURCE_DIR}/dist"
            "${MO2_MOD_DIR}"
        COMMENT "Deploying bundled assets (dist/) -> ${MO2_MOD_DIR}"
        VERBATIM
    )
endif()
endif()

# Component install is the release allowlist; never package a live MO2 folder.
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION SKSE/Plugins COMPONENT DDC)
if(MSVC)
    install(FILES "$<TARGET_PDB_FILE:${PROJECT_NAME}>" DESTINATION . COMPONENT DDCSymbols)
endif()
install(DIRECTORY dist/ DESTINATION . COMPONENT DDC)
# The player download contains runtime files only. Keep readable documentation
# and licenses in the source ZIP; retain binary redistribution notices in a
# passive DLL resource and debug symbols in the private release candidate.
file(GLOB DDC_THIRD_PARTY_LICENSES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/licenses/*.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/licenses/*.md")
set(DDC_LICENSE_INPUTS "${CMAKE_CURRENT_SOURCE_DIR}/LICENSING.md" "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/EXCEPTIONS.md" "${CMAKE_CURRENT_SOURCE_DIR}/THIRD-PARTY-NOTICES.txt"
    ${DDC_THIRD_PARTY_LICENSES})
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${DDC_LICENSE_INPUTS})
set(DDC_LICENSE_BUNDLE "Diet Dr Camera ${PROJECT_VERSION} - license notices\n\nSource filenames label the full notices reproduced below.\n\n")
foreach(DDC_LICENSE_INPUT IN LISTS DDC_LICENSE_INPUTS)
    file(RELATIVE_PATH DDC_LICENSE_LABEL "${CMAKE_CURRENT_SOURCE_DIR}" "${DDC_LICENSE_INPUT}")
    file(READ "${DDC_LICENSE_INPUT}" DDC_LICENSE_TEXT)
    string(APPEND DDC_LICENSE_BUNDLE "\n===== ${DDC_LICENSE_LABEL} =====\n\n${DDC_LICENSE_TEXT}\n")
endforeach()
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/LicenseNotices.txt" CONTENT "${DDC_LICENSE_BUNDLE}")

if(DDC_BUILD_CHECKS)
    enable_testing()
    add_test(NAME ReleasePackagingChecks COMMAND powershell -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/ReleasePackagingChecks.ps1"
        -PackageScript "${CMAKE_CURRENT_SOURCE_DIR}/tools/package-release.ps1")
    if(MSVC)
        add_test(NAME RuntimeBinaryChecks COMMAND powershell -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/tests/RuntimeBinaryChecks.ps1"
            -Dll "$<TARGET_FILE:DietDrCamera>" -Pdb "$<TARGET_PDB_FILE:DietDrCamera>"
            -Version "${PROJECT_VERSION}" -Verifier "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify-release-binary.ps1")
    endif()
    add_executable(TrampolineChecks tests/TrampolineChecks.cpp src/Hooks/HookTrampoline.cpp)
    target_include_directories(TrampolineChecks PRIVATE include)
    target_compile_definitions(TrampolineChecks PRIVATE ENABLE_COMMONLIBSSE_TESTING=1)
    target_link_libraries(TrampolineChecks PRIVATE CommonLibSSE::CommonLibSSE)
    target_precompile_headers(TrampolineChecks PRIVATE include/PCH.h)
    add_test(NAME TrampolineChecks COMMAND TrampolineChecks)
    add_test(NAME TrampolineLegacySKSEChecks COMMAND TrampolineChecks --no-interface)
    add_test(NAME TrampolinePoolFallbackChecks COMMAND TrampolineChecks --exhausted-pool)
    add_executable(RuntimeHookChecks tests/RuntimeHookChecks.cpp src/Hooks/RuntimePatchInspection.cpp)
    target_include_directories(RuntimeHookChecks PRIVATE include extern/minhook/src/hde)
    target_link_libraries(RuntimeHookChecks PRIVATE CommonLibSSE::CommonLibSSE nlohmann_json::nlohmann_json)
    add_test(NAME RuntimeHookChecks COMMAND RuntimeHookChecks)
    add_executable(DiagnosticsChecks tests/DiagnosticsChecks.cpp src/Core/Diagnostics.cpp)
    target_include_directories(DiagnosticsChecks PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(DiagnosticsChecks PRIVATE ENABLE_COMMONLIBSSE_TESTING=1)
    target_link_libraries(DiagnosticsChecks PRIVATE CommonLibSSE::CommonLibSSE version)
    target_precompile_headers(DiagnosticsChecks PRIVATE include/PCH.h)
    add_test(NAME DiagnosticsChecks COMMAND DiagnosticsChecks)
    add_executable(RuntimeLayoutChecks tests/RuntimeLayoutChecks.cpp src/Core/Diagnostics.cpp src/Hooks/RuntimeHooks.cpp src/Hooks/RuntimePatchInspection.cpp)
    target_include_directories(RuntimeLayoutChecks PRIVATE include extern/minhook/src/hde "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_compile_definitions(RuntimeLayoutChecks PRIVATE ENABLE_COMMONLIBSSE_TESTING=1)
    target_link_libraries(RuntimeLayoutChecks PRIVATE CommonLibSSE::CommonLibSSE version)
    target_precompile_headers(RuntimeLayoutChecks PRIVATE include/PCH.h)
    add_test(NAME RuntimeLayoutChecks COMMAND RuntimeLayoutChecks)
    # Optional before/after diagnosis using a previous release's hook sources.
    # Game executables remain private inputs to --image, never build inputs.
    set(DDC_HOOK_BASELINE_DIR "" CACHE PATH "Previous release hook sources for offline comparison")
    if(DDC_HOOK_BASELINE_DIR)
        add_executable(RuntimeBaselineChecks tests/RuntimeLayoutChecks.cpp
            "${DDC_HOOK_BASELINE_DIR}/RuntimeHooks.cpp"
            "${DDC_HOOK_BASELINE_DIR}/RuntimePatchInspection.cpp")
        target_include_directories(RuntimeBaselineChecks PRIVATE include extern/minhook/src/hde)
        target_compile_definitions(RuntimeBaselineChecks PRIVATE ENABLE_COMMONLIBSSE_TESTING=1)
        target_link_libraries(RuntimeBaselineChecks PRIVATE CommonLibSSE::CommonLibSSE)
        target_precompile_headers(RuntimeBaselineChecks PRIVATE include/PCH.h)
    endif()
    add_executable(PresetCompatibilityChecks tests/PresetCompatibilityChecks.cpp
        tests/PresetRuntimeStubs.cpp src/Settings/SettingsManager.cpp
        src/Settings/PresetManager.cpp src/Settings/MeleeWeaponOverrides.cpp
        src/Core/AtomicFile.cpp src/Shouts/ShoutRegistry.cpp)
    target_include_directories(PresetCompatibilityChecks PRIVATE include)
    target_link_libraries(PresetCompatibilityChecks PRIVATE CommonLibSSE::CommonLibSSE
        tomlplusplus::tomlplusplus spdlog::spdlog nlohmann_json::nlohmann_json)
    target_precompile_headers(PresetCompatibilityChecks PRIVATE include/PCH.h)
    if(MSVC)
        target_compile_options(PresetCompatibilityChecks PRIVATE /Gy /utf-8)
        target_link_options(PresetCompatibilityChecks PRIVATE /OPT:REF /OPT:ICF)
    endif()
    add_test(NAME PresetCompatibilityChecks COMMAND PresetCompatibilityChecks "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
    add_executable(CameraQualityChecks tests/CameraQualityChecks.cpp src/Core/AtomicFile.cpp)
    target_include_directories(CameraQualityChecks PRIVATE include)
    target_compile_definitions(CameraQualityChecks PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    if(MSVC)
        target_compile_options(CameraQualityChecks PRIVATE /utf-8)
    endif()
    add_test(NAME CameraQualityChecks COMMAND CameraQualityChecks)
    add_executable(EntryClipboardChecks tests/EntryClipboardChecks.cpp)
    target_include_directories(EntryClipboardChecks PRIVATE include)
    add_test(NAME EntryClipboardChecks COMMAND EntryClipboardChecks)
    add_executable(AnimationCatalogChecks tests/AnimationCatalogChecks.cpp src/Camera/AnimationCatalog.cpp)
    target_include_directories(AnimationCatalogChecks PRIVATE include)
    target_link_libraries(AnimationCatalogChecks PRIVATE nlohmann_json::nlohmann_json)
    if(MSVC)
        target_compile_options(AnimationCatalogChecks PRIVATE /utf-8)
    endif()
    add_test(NAME AnimationCatalogChecks COMMAND AnimationCatalogChecks)
    add_executable(DirectionalNavigationChecks tests/DirectionalNavigationChecks.cpp)
    target_include_directories(DirectionalNavigationChecks PRIVATE include)
    add_test(NAME DirectionalNavigationChecks COMMAND DirectionalNavigationChecks)
    add_executable(MenuLayoutChecks tests/MenuLayoutChecks.cpp)
    target_include_directories(MenuLayoutChecks PRIVATE include)
    add_test(NAME MenuLayoutChecks COMMAND MenuLayoutChecks)
    add_library(MenuFrameworkLoadFixture SHARED tests/MenuFrameworkLoadFixture.cpp)
    target_include_directories(MenuFrameworkLoadFixture PRIVATE include)
    set_target_properties(MenuFrameworkLoadFixture PROPERTIES
        OUTPUT_NAME SKSEMenuFramework
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-fixtures")
    add_executable(MenuFrameworkLoadChecks tests/MenuFrameworkLoadChecks.cpp)
    target_include_directories(MenuFrameworkLoadChecks PRIVATE include)
    add_dependencies(MenuFrameworkLoadChecks MenuFrameworkLoadFixture)
    add_test(NAME MenuFrameworkLoadChecks COMMAND MenuFrameworkLoadChecks
        $<TARGET_FILE:MenuFrameworkLoadFixture>)
    add_executable(SliderValueInputChecks tests/SliderValueInputChecks.cpp)
    target_include_directories(SliderValueInputChecks PRIVATE include)
    add_test(NAME SliderValueInputChecks COMMAND SliderValueInputChecks)
    add_executable(TargetLockBiasChecks tests/TargetLockBiasChecks.cpp)
    target_include_directories(TargetLockBiasChecks PRIVATE include)
    add_test(NAME TargetLockBiasChecks COMMAND TargetLockBiasChecks)
    add_executable(FleeFramingChecks tests/FleeFramingChecks.cpp)
    target_include_directories(FleeFramingChecks PRIVATE include)
    add_test(NAME FleeFramingChecks COMMAND FleeFramingChecks)
    add_executable(ParaglideDiagnosticsChecks tests/ParaglideDiagnosticsChecks.cpp)
    target_include_directories(ParaglideDiagnosticsChecks PRIVATE include)
    add_test(NAME ParaglideDiagnosticsChecks COMMAND ParaglideDiagnosticsChecks)
    add_executable(NpcNoiseChecks tests/NpcNoiseChecks.cpp)
    target_include_directories(NpcNoiseChecks PRIVATE include)
    add_test(NAME NpcNoiseChecks COMMAND NpcNoiseChecks)
    add_executable(ProjectileFlybyChecks tests/ProjectileFlybyChecks.cpp)
    target_include_directories(ProjectileFlybyChecks PRIVATE include)
    add_test(NAME ProjectileFlybyChecks COMMAND ProjectileFlybyChecks)
    add_executable(HitShakeChecks tests/HitShakeChecks.cpp)
    target_include_directories(HitShakeChecks PRIVATE include)
    add_test(NAME HitShakeChecks COMMAND HitShakeChecks)
    add_executable(DamageReactionChecks tests/DamageReactionChecks.cpp)
    target_include_directories(DamageReactionChecks PRIVATE include)
    add_test(NAME DamageReactionChecks COMMAND DamageReactionChecks)
    add_executable(ArcheryHitShakeChecks tests/ArcheryHitShakeChecks.cpp)
    target_include_directories(ArcheryHitShakeChecks PRIVATE include)
    add_test(NAME ArcheryHitShakeChecks COMMAND ArcheryHitShakeChecks)
    add_executable(MagicHitShakeChecks tests/MagicHitShakeChecks.cpp)
    target_include_directories(MagicHitShakeChecks PRIVATE include)
    add_test(NAME MagicHitShakeChecks COMMAND MagicHitShakeChecks)
    add_executable(NoiseTransitionChecks tests/NoiseTransitionChecks.cpp)
    target_include_directories(NoiseTransitionChecks PRIVATE include)
    add_test(NAME NoiseTransitionChecks COMMAND NoiseTransitionChecks)
    add_executable(FirstPersonNoiseChecks tests/FirstPersonNoiseChecks.cpp)
    target_include_directories(FirstPersonNoiseChecks PRIVATE include)
    add_test(NAME FirstPersonNoiseChecks COMMAND FirstPersonNoiseChecks)
    add_executable(CinematicViewsChecks tests/CinematicViewsChecks.cpp)
    target_include_directories(CinematicViewsChecks PRIVATE include)
    add_test(NAME CinematicViewsChecks COMMAND CinematicViewsChecks)
    add_executable(CinematicViewRenderChecks tests/CinematicViewRenderChecks.cpp)
    target_include_directories(CinematicViewRenderChecks PRIVATE include)
    target_link_libraries(CinematicViewRenderChecks PRIVATE CommonLibSSE::CommonLibSSE)
    target_precompile_headers(CinematicViewRenderChecks PRIVATE include/PCH.h)
    add_test(NAME CinematicViewRenderChecks COMMAND CinematicViewRenderChecks)
    add_executable(POVSlideRecoveryChecks tests/POVSlideRecoveryChecks.cpp)
    target_include_directories(POVSlideRecoveryChecks PRIVATE include)
    add_test(NAME POVSlideRecoveryChecks COMMAND POVSlideRecoveryChecks)
    add_executable(TargetLockPreviewChecks tests/TargetLockPreviewChecks.cpp)
    target_include_directories(TargetLockPreviewChecks PRIVATE include)
    add_test(NAME TargetLockPreviewChecks COMMAND TargetLockPreviewChecks)
    add_executable(MountedTargetLockChecks tests/MountedTargetLockChecks.cpp)
    target_include_directories(MountedTargetLockChecks PRIVATE include)
    add_test(NAME MountedTargetLockChecks COMMAND MountedTargetLockChecks)
    add_executable(CameraCollisionChecks tests/CameraCollisionChecks.cpp)
    target_include_directories(CameraCollisionChecks PRIVATE include)
    add_test(NAME CameraCollisionChecks COMMAND CameraCollisionChecks)
    add_executable(CameraCollisionSettingsChecks tests/CameraCollisionSettingsChecks.cpp)
    target_include_directories(CameraCollisionSettingsChecks PRIVATE include)
    target_link_libraries(CameraCollisionSettingsChecks PRIVATE tomlplusplus::tomlplusplus)
    add_test(NAME CameraCollisionSettingsChecks COMMAND CameraCollisionSettingsChecks)
    add_executable(LocationOverrideChecks tests/LocationOverrideChecks.cpp)
    target_include_directories(LocationOverrideChecks PRIVATE include)
    add_test(NAME LocationOverrideChecks COMMAND LocationOverrideChecks)
    add_executable(BleedoutFreeLookChecks tests/BleedoutFreeLookChecks.cpp)
    target_include_directories(BleedoutFreeLookChecks PRIVATE include)
    add_test(NAME BleedoutFreeLookChecks COMMAND BleedoutFreeLookChecks)
    add_executable(SpellTrajectoryChecks tests/SpellTrajectoryChecks.cpp)
    target_include_directories(SpellTrajectoryChecks PRIVATE include)
    add_test(NAME SpellTrajectoryChecks COMMAND SpellTrajectoryChecks)
    add_executable(MagicCastChecks tests/MagicCastChecks.cpp)
    target_include_directories(MagicCastChecks PRIVATE include)
    add_test(NAME MagicCastChecks COMMAND MagicCastChecks)
    add_executable(VanityCameraChecks tests/VanityCameraChecks.cpp)
    target_include_directories(VanityCameraChecks PRIVATE include)
    add_test(NAME VanityCameraChecks COMMAND VanityCameraChecks)
    add_executable(ItemBindingChecks tests/ItemBindingChecks.cpp)
    target_include_directories(ItemBindingChecks PRIVATE include)
    target_link_libraries(ItemBindingChecks PRIVATE tomlplusplus::tomlplusplus)
    add_test(NAME ItemBindingChecks COMMAND ItemBindingChecks)
    add_executable(OverrideInheritanceChecks tests/OverrideInheritanceChecks.cpp)
    target_include_directories(OverrideInheritanceChecks PRIVATE include)
    target_link_libraries(OverrideInheritanceChecks PRIVATE tomlplusplus::tomlplusplus)
    add_test(NAME OverrideInheritanceChecks COMMAND OverrideInheritanceChecks)
endif()
