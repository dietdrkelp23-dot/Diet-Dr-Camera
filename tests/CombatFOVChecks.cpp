#include "Core/CombatFOV.h"
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace DietDrCamera;
static void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
static float Enter(float fps, float speed)
{
    CombatFOV::Envelope envelope;
    CombatFOV::Tuning tuning{true,110,speed};
    for (int i=0;i<static_cast<int>(fps);++i) envelope.Step(tuning,80,true,1/fps);
    return envelope.Hold(80);
}
static void CheckMenuPreview()
{
    CombatFOV::Envelope envelope;
    CombatFOV::Tuning tuning{true,110,1};
    CombatFOV::Frame frame;
    frame.paused = true;
    frame.editorOpen = true;
    frame.preview = true;
    frame.dt = 1.0f/60;
    for (int i=0;i<180;++i) envelope.Update(tuning,80,frame);
    Require(envelope.Hold(80)==110, "Paused editor did not preview Combat FOV outside combat");

    tuning.fov = 95;
    float previous = 110;
    for (int i=0;i<180;++i) {
        const float value = envelope.Update(tuning,80,frame);
        Require(value<=previous && value>=95, "Live FOV edit snapped or moved away from the slider");
        previous = value;
    }
    Require(previous==95, "FOV slider changes remained frozen in the menu");
    auto slow = envelope, fast = envelope;
    const float slowValue = slow.Update({true,110,.1f},80,frame);
    const float fastValue = fast.Update({true,110,5},80,frame);
    Require(fastValue>slowValue && slowValue>95 && fastValue<110,
        "Transition Speed edits do not affect the live preview");

    frame.editorOpen = false; // native pause; stale preview cannot activate
    tuning.fov = 120;
    for (int i=0;i<180;++i) Require(envelope.Update(tuning,80,frame)==95,
        "A normal paused menu advanced the effect");
    frame.editorOpen = true;
    frame.preview = false; // select a different effect, tab or mod page
    for (int i=0;i<180;++i) envelope.Update(tuning,83,frame);
    Require(envelope.Hold(83)==83, "Leaving Combat FOV settings retained the preview");

    frame.inCombat = true; // real combat still edits live on other pages / Quick Tune
    tuning.fov = 100;
    for (int i=0;i<180;++i) envelope.Update(tuning,83,frame);
    Require(envelope.Hold(83)==100, "Combat FOV stopped updating on another editor page");
    tuning.enabled = false;
    frame.preview = true;
    for (int i=0;i<180;++i) envelope.Update(tuning,83,frame);
    Require(envelope.Hold(83)==83, "Disabling Combat FOV in a paused editor left its FOV applied");

    tuning.enabled = true;
    frame.inCombat = false;
    for (int i=0;i<180;++i) envelope.Update(tuning,83,frame);
    frame.editorOpen = false;
    frame.paused = false;
    frame.dt = 30; // native pause / stalled camera writer
    Require(envelope.Update(tuning,90,frame)==100, "A long camera gap jumped the preview on resume");
    frame.dt = 1.0f/60;
    for (int i=0;i<180;++i) envelope.Update(tuning,90,frame);
    Require(envelope.Hold(90)==90, "Closing the editor retained a stale preview flag");
    envelope.Reset();
    Require(envelope.Update({false,110,1},90,{false,true,true,true,1.0f/60})==90,
        "Selecting disabled Combat FOV changed the camera");
}
int main()
{
    CheckMenuPreview();
    const CombatFOV::Tuning defaults;
    CombatFOV::Envelope envelope;
    Require(envelope.Step(defaults,92,true,1.0f/60)==92,
        "Old presets acquired combat FOV");
    CombatFOV::Tuning tuning{true,110,1};
    Require(envelope.Step(tuning,80,true,0)==80, "Combat entry snapped to its target");
    float previous=80;
    for (int i=0;i<180;++i) {
        const float value=envelope.Step(tuning,80,true,1.0f/60);
        Require(value>=previous && value<=110, "Combat entry overshot or restarted");
        previous=value;
    }
    Require(previous==110, "Combat FOV did not hold its target");
    for (int i=0;i<60;++i) Require(envelope.Step(tuning,70,true,1.0f/60)==110,
        "Normal state changes overrode the active combat FOV");
    const float held=envelope.Hold(70);
    Require(envelope.Step(tuning,70,false,0)==held &&
        envelope.Step(tuning,70,false,std::numeric_limits<float>::quiet_NaN())==held,
        "A paused or invalid tick advanced the transition");
    for (int i=0;i<240;++i) {
        const float value=envelope.Step(tuning,95,false,1.0f/60);
        Require(value<=previous && value>=95, "Combat exit overshot or jumped");
        previous=value;
    }
    Require(previous==95 && envelope.Step(tuning,87,false,1.0f/60)==87,
        "Combat exit restored a stale FOV instead of the current normal profile");

    envelope.Reset();
    const float entering=envelope.Step(tuning,80,true,.05f);
    const float leaving=envelope.Step(tuning,80,false,.05f);
    const float returning=envelope.Step(tuning,80,true,.05f);
    Require(entering>80 && leaving<entering && leaving>80 && returning>leaving,
        "Rapid combat toggles restart or snap the transition");
    tuning.enabled=false;
    for(int i=0;i<240;++i)envelope.Step(tuning,80,true,1.0f/60);
    Require(envelope.Hold(80)==80, "Disabling the effect left a combat FOV behind");

    for(float fps:{30.0f,60.0f,144.0f,240.0f})
        Require(std::abs(Enter(fps,1)-Enter(60,1))<.001f, "Transition depends on frame rate");
    Require(Enter(60,2)>Enter(60,.25f), "Transition Speed does not control the blend");
    const float nan=std::numeric_limits<float>::quiet_NaN();
    Require(CombatFOV::Sanitize({true,nan,nan})==CombatFOV::Tuning{true,80,1},
        "Invalid tuning did not receive safe defaults");
    const auto limits=CombatFOV::Sanitize({true,300,-3});
    Require(limits.fov==CombatFOV::kMaxFOV && limits.transitionSpeed==CombatFOV::kMinSpeed,
        "Combat FOV bounds are not enforced");
    envelope.Reset();
    Require(envelope.Hold(80)==80, "Camera/preset reset retained a combat override");
    std::cout<<"Combat FOV checks passed (entry, hold, exit, pause, live menu preview, disabling, bounds and frame rates).\n";
}
