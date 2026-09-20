# Crowd Modifier and Cinematic Views

**Current build (September 19, 2026): Cinematic Views is deferred.** Its controller
and picker are excluded from the DLL, and its menu, Quick Tune, camera, input and
SKSE serialization connections have been removed. The implementation files below
are retained for design work, not active functionality. The format-13 preset
codec retains existing definitions; they are excluded from active profile
enumeration. Crowd Modifier remains active. Pre-removal integration snapshots are
under `build/diagnostics/cinematic-views-removal-20260919/before/`.

## Combat camera research

The recommendation is **nearby enemies engaged with the player or their team**,
weighted by distance, rather than a count of enemies currently on screen.

Mihir Sheth's GDC presentation on God of War describes weighting relevant enemies
for camera assistance and deliberately maintaining the player's understanding of
off-screen threats. It also discusses allowing predictable off-screen enemy
positions instead of pushing every enemy back into view. See the enemy-position
and camera-assist sections in [Evolving Combat in God of War (2019)](https://media.gdcvault.com/gdc2019/presentations/Sheth_Mihir_EvolvingCombat.pdf).

Phil Wilkins' earlier presentation describes contextual camera selection and
framing multiple targets in the God of War series:
[Iterating on a Dynamic Camera System (2011)](https://media.gdcvault.com/gdc2011/slides/Phil_Wilkins_IteratingDynamicCamera.pdf).

These are design precedents, not evidence that either game uses this exact
enemy-count formula. The choice for Skyrim is an inference: screen membership
changes when the player turns or the camera widens, so driving FOV directly from
that membership can create repeated widening and tightening. Encounter-based
framing remains stable as the player looks around.

DDC therefore counts loaded, living hostiles fighting the player or a teammate
within 1,800 game units. Geometry rays exclude enemies separated by walls without
using the screen frustum or actor vision cone. Distance weighting is full within
600 units and falls smoothly to zero at 1,800. One enemy adds nothing. Additional
enemies keep contributing with diminishing returns; four enemies no longer hit
a fixed ceiling. A damped envelope eases into the wider view and returns more
slowly as pressure falls.

Under **Extras > Cinematic Effects > Crowd Modifier**, Zoom Intensity and FOV
Intensity are independent third-person controls, each 0-3. Zero is off and 1 is
the baseline response. Intensity increases both the available widening and the
crowd size over which it builds. Existing camera collision remains in charge of
the available third-person distance. Internally the weighted extra-enemy count
is eased first; each channel then uses `I * (1 - exp(-count / (2 + I)))`, scaled
by 60 Zoom slider units or 10 FOV degrees. The 64-actor scan limit bounds work;
it is not the previous four-enemy design threshold.

## Choosing a subject

The previous authoring path ran a single physics ray during exploration and
shared playback's weapon/combat restrictions. With no valid hit, all bind buttons
disappeared. This did not provide enough control or feedback for selecting static
scenery. The exact cause of the user's no-binding session was not reproduced.

The picker now scans loaded references on the game thread only when the editor
requests it. Scene bounds include large objects whose origin is away from the
crosshair. Native `NiPick` triangle tests distinguish visible meshes under aim;
physics supplies a separate surface-point fallback. A list includes nearby
loaded objects; clicking an entry adds its view immediately. Names
fall back to readable model names for unnamed statics (for example, a word-wall
mesh). A gold marker follows the hovered or controller-focused subject behind
the menu. This is a focus marker,
not a shader outline or automatic semantic grouping of multi-reference scenery.

This approach addresses the distinction between graphics selection and physics
selection documented by [Cobb Positioner's author](https://www.nexusmods.com/skyrim/articles/50762).
The engine's mesh picking API is exposed in
[CommonLib's NiPick](https://raw.githubusercontent.com/alandtse/CommonLibVR/ng/include/RE/N/NiPick.h).
No external object-positioning mod is required or modified.

Authoring does not require sheathed weapons. The last gameplay camera pose is
held while menus are open, and an unavailable pose produces an explanation with
a refresh action. Selection snapshots contain stable form identities and values,
not retained scene pointers. Binding logs the selected subject; scans log their
object/query counts. Playback still uses its existing exploration restrictions.
Nearby references remain individually browsable. **Aimed Scenery** also picks a
fixed point on rendered terrain, distant water and scene geometry, up to 1,048,576
units away. It requires no persistent reference or loaded target cell for playback.
The scan walks intersecting bounds in nearest-first order, with limits of 8,192
scene nodes and 512 mesh queries. Incomplete scans do not produce a scenery point;
the existing physics surface fallback and reference list remain available.
Logs record scan completion, source, query counts and focus distance.

## Authoring a view

**Extras > Cinematic Views** is immediately after Cinematic Effects.

1. Stand where the view should start, face the subject, then open Cinematic Views
   and choose **+ Add**. The
   subject picker opens automatically when there are no views.
2. Click a subject in the nearby list to bind it and open its controls. Hover
   a row to inspect its gold focus marker first; controller focus also shows
   the marker, and activation binds it. **Aimed Scenery** anchors the shot to
   a fixed point on nearby or distant scenery; **Aimed Surface** is the physics
   fallback. Close the menu to re-aim, then use **Refresh**.
3. Set **Radius** around the position where you created the view. The target may
   be far outside this area. **Set Here** moves the trigger to your current
   position in the same worldspace/cell and resets this view's encounter history.
   Older bindings retain their subject-centered radius until Set Here is used.
   Entering the trigger area for the first time automatically starts a
   **10-second** encounter. There is no first-encounter/idle mode toggle or
   duration slider. You do not have to aim at the subject to enter the area.
   After its first encounter, the view activates when you have been idle for
   its **Idle Timer**. Each view has its own timer, completely independent of
   Vanity Camera's timer and enable/disable setting.
4. Tune **Side Offset**, **Height**, **Zoom**, **FOV**, **Rotation**, and
   **Pitch Offset**, using the same controls and units as Third Person entries.
   Lower Zoom moves closer to the player; lower FOV narrows the lens. Rotation
   and Pitch Offset adjust the subject-facing angle. The normal **Transition
   Override** controls apply on entry and return. The entry clipboard can copy
   and paste complete camera profiles. **Lock-On Tightness** runs from 0 (loose)
   to 1 (tight), controlling how far the subject may drift from that angle.
   The default 0.5 preserves the previous framing. Even 0 keeps following the
   subject; 1 removes the framing dead zone while retaining normal smoothing.
5. While a view is active, open **Quick Tune**. Its own named Cinematic View box
   exposes all six camera controls, including Rotation and Pitch Offset, plus
   Lock-On Tightness and its transition companion. Radius and Idle Timer have
   a separate Activation box. Edits preview live; the encounter timer pauses
   while tuning and resumes on close.
6. Use **Update** or **Save as New** to keep edits; selecting a preset still
   restores its saved settings.

The same scenery can have multiple independent views: a lake viewed from the
Guardian Stones and from a cliff has separate trigger positions, framing, idle
timers and encounter history. Height is included in the trigger distance. A view
aimed at a distant mountain can have a small trigger area around a Falkreath
viewpoint. Being near the mountain itself does not activate that view.

Once acquired, both first encounters and later idle views keep following the
subject while you walk, sprint or jump. Radius and visibility are acquisition
conditions, so crossing the boundary or passing briefly behind scenery does
not release an existing lock. Moving the mouse/right stick releases it; combat
input and unavailable subjects also release it. Later idle views hold until
released, then restart their independent Idle Timer. There is no repeat delay.

The editor uses compact Camera and Activation sections with larger slider labels
and tracks. View and subject lists use a larger text scale, and the view list has
more width. There is no enable toggle or preview button.
Logs distinguish first encounters, idle visits, native third-person rendering and
release. A first encounter is recorded only once its profile actually renders.
Dismissing it early still leaves later visits on idle timing. **Reset Encounter**
makes its next eligible entry a first encounter again.

The subject picker and visibility refinement are unchanged: a blocking physics
hull is checked against visible triangles, with at most four exclusions per ray.
A separate real wall still blocks automatic discovery. Word effects and their
stone meshes can therefore be selected individually despite coarse collision.

Views run only in normal third person and do not change POV. Combat, dialogue,
ordinary menus, mounted/furniture states, paragliding and target lock take
priority. Quick Tune holds the current view. Locomotion/animation camera entries
do not replace an acquired view when the player starts moving. A visible nearby view reserves its idle wait ahead of the
generic vanity camera. Gameplay input is observed, never consumed.

Each view owns a normal CameraProfile. CameraController selects that profile and
adds loose subject-facing Rotation/Pitch targets to a runtime copy. The existing
six channel springs, native orbit, camera collision and FOV pipeline perform the
move and return. Cinematic Views no longer write NiCamera world transforms,
frustums, or projection matrices, avoiding a separate late-stage camera position
that can disagree with sky rendering. The reported sky behavior still needs
in-game comparison at high Zoom; an offline check cannot establish the visual fix.

Definitions use plugin/local form identities and worldspace or interior-cell
scope and optional trigger position in preset format 13. Temporary references
and distant scenery can be bound as world points. Reference bindings skip missing
or unloaded subjects; fixed-point bindings do not require a loaded target.
First-encounter history belongs
to the SKSE co-save, so another character does not inherit it. Loading a save
from before an encounter permits it again.

The earlier fixed-shot, relative-focus and selectable-trigger schemes were
unreleased prototypes. Existing definitions retain IDs, subjects, locations,
encounter history, native camera profiles and idle_delay. Lock-On Tightness is
saved independently per view; absent lock_on_tightness defaults to 0.5. Legacy
range becomes radius. Without native camera data, a view starts at normal
third-person defaults. Old first_encounter/idle_only/trigger flags, custom duration, world-space
zoom, relative FOV and repeat delays are ignored. Every entry now uses the same
first-radius-then-idle behavior. Preset files change only on an explicit save.
