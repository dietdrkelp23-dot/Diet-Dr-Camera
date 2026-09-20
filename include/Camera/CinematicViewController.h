#pragma once
#include "Core/CinematicViews.h"
#include "Camera/CinematicViewPicker.h"
#include <string_view>
#include <optional>

namespace RE { class NiCamera; class InputEvent; }
namespace DietDrCamera::CinematicViewController
{
    struct EditorState {
        std::vector<CinematicViewPicker::Subject> subjects;
        bool hasCamera = false, active = false, scanned = false, scanPending = false;
        std::string pickerStatus;
        std::string status;
        std::string playbackStatus;
    };
    void InstallSerialization();
    void Reset();
    void Tick();
    void OnInput(RE::InputEvent* events);
    bool IsActive();
    bool WantsCamera();
    void CaptureBase(RE::NiCamera* camera);
    struct ActiveEntry {
        std::string id;
        CinematicViews::Point toSubject;
    };
    std::optional<ActiveEntry> GetActiveEntry();
    void ProfileApplied(std::string_view id);
    EditorState GetEditorState();
    void RefreshSubjects();
    std::string AddBinding(std::string_view subjectKey);
    void SetTriggerHere(std::string_view id);
    void Preview(std::string_view id);
    bool HasSeen(std::string_view id);
    void Forget(std::string_view id);
}
