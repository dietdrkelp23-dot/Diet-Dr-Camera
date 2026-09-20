void CheckCinematicViewPresets(SettingsManager& s)
{
    s.ResetAllToVanilla();
    Require(s.cinematicViews.empty() && s.combatFraming == CombatFraming::Tuning{},"New cinematic features do not default off");
    CinematicViews::View item;
    item.id = "chillrend-test"; item.name = "Chillrend";
    item.target = {"Skyrim.esm",0x123456}; item.space = {"Skyrim.esm",0x65432}; item.exterior = false;
    item.point = {18,-290,470}; item.targetOffset = {3,4,5}; 
    item.idleDelay = 12.5f;
    item.lockOnTightness = .2f;
    item.profile.fov = 64; item.profile.rotation = 9;
    CinematicViews::View landmark = item;
    landmark.id = "entrance-test"; landmark.name = "High Hrothgar entrance";
    landmark.target = {}; landmark.exterior = true; landmark.radius = 8000;
    landmark.lockOnTightness = .85f;
    landmark.profile = CameraProfile::Default3p();
    landmark.profile.sideOffset = -45; landmark.profile.height = 110;
    landmark.profile.zoom = 2.5f; landmark.profile.fov = 55; landmark.profile.rotation = -23;
    landmark.profile.pitchOffset = 18;
    landmark.profile.transitionSetZoom = landmark.profile.transitionSetPitch = true;
    landmark.profile.transitionSetWeight = landmark.profile.transitionSetLooseness = true;
    landmark.profile.transitionZoom = .2f; landmark.profile.transitionPitch = .3f;
    landmark.profile.transitionWeight = .6f; landmark.profile.transitionLooseness = .75f;
    landmark.profile.SyncTransitionOverride();
    s.cinematicViews = {item,landmark}; s.combatFraming = {1.25f,0};
    const auto profiles = s.GetAllProfiles();
    Require(std::find(profiles.begin(),profiles.end(),&s.cinematicViews[0].profile) == profiles.end() &&
        std::find(profiles.begin(),profiles.end(),&s.cinematicViews[1].profile) == profiles.end(),
        "Deferred cinematic views must not participate in active profile enumeration");
    const auto expected = s.cinematicViews;
    auto table = s.BuildSaveTable();
    Require(table["cinematic_views"].as_array() && table["cinematic_views"].as_array()->size() == 2,"Views not written as independent entries");
    const auto firstRow = table["cinematic_views"][0];
    Require(firstRow["lock_on_tightness"].value_or(-1.0f) == item.lockOnTightness,
        "Per-view lock tightness was not saved");
    Require(!firstRow["style"] && !firstRow["hold"] && !firstRow["fov"] && !firstRow["heading"] &&
        !firstRow["duration"] && !firstRow["trigger"] && firstRow["radius"] && firstRow["idle_delay"] && firstRow["camera"].is_table() &&
        !firstRow["repeat_delay"] && !firstRow["zoom_in"] && !firstRow["fov_reduction"] && !firstRow["focus_strength"] &&
        !firstRow["idle_only"] && !firstRow["first_encounter"],"Obsolete shot controls are still written instead of camera entries");
    RoundTrip(s);
    Require(s.cinematicViews == expected && s.combatFraming == CombatFraming::Tuning{1.25f,0},"Object / landmark views or independent framing sliders changed after round trip");
    auto previousNative = table;
    (*previousNative["cinematic_views"].as_array())[0].as_table()->erase("lock_on_tightness");
    Require(s.ApplyTable(previousNative) && s.cinematicViews.front().lockOnTightness == CinematicViews::kDefaultLockOnTightness &&
        s.cinematicViews.front().profile == item.profile && s.cinematicViews.back() == landmark,
        "An existing native entry lost its camera or default lock behavior");
    Require(s.ApplyTable(table) && s.cinematicViews == expected,"Reload did not restore each view's saved lock tightness");
    s.combatFraming = {0,2.15f}; RoundTrip(s);
    Require(s.combatFraming == CombatFraming::Tuning{0,2.15f},"FOV-only combat framing did not persist");
    s.combatFraming = {.35f,2.7f}; RoundTrip(s);
    Require(s.combatFraming == CombatFraming::Tuning{.35f,2.7f},"Independent framing intensities changed after round trip");
    Require(!s.BuildSaveTable()["cinematic"]["combat_framing"]["zoom"] &&
        !s.BuildSaveTable()["cinematic"]["combat_framing"]["fov"],"Retired development maximum sliders are still being written");
    const auto before = s.BuildSaveTable();
    auto invalid = table;
    invalid.insert_or_assign("cinematic_views",toml::table{});
    Require(!s.ApplyTable(invalid) && before == s.BuildSaveTable(),"Invalid view table changed current settings");
    invalid = table;
    auto* rows = invalid["cinematic_views"].as_array();
    rows->push_back(*rows->get(0));
    Require(!s.ApplyTable(invalid) && before == s.BuildSaveTable(),"Duplicate view identities were accepted");
    invalid = table;
    (*invalid["cinematic_views"].as_array())[0].as_table()->insert_or_assign("point",toml::array{1,2});
    Require(!s.ApplyTable(invalid) && before == s.BuildSaveTable(),"Malformed coordinates partially applied a preset");
    invalid = table;
    (*invalid["cinematic_views"].as_array())[0].as_table()->insert_or_assign("camera",toml::array{1,2});
    Require(!s.ApplyTable(invalid) && before == s.BuildSaveTable(),"Malformed camera entry partially applied a preset");
    auto prototype = table;
    auto* old = (*prototype["cinematic_views"].as_array())[0].as_table();
    for (const char* key : {"camera","radius","idle_delay","lock_on_tightness"}) old->erase(key);
    old->insert("hold",15.0f); old->insert("blend_in",1.0f); old->insert("blend_out",1.0f);
    old->insert("style","saved_shot"); old->insert("distance",2000.0f); old->insert("fov",20.0f);
    old->insert("range",900.0f); old->insert("idle_only",true); old->insert("first_encounter",false); old->insert("idle_delay",1.5f);
    old->insert("repeat_delay",90.0f); old->insert("zoom_in",400.0f); old->insert("fov_reduction",60.0f);
    Require(s.ApplyTable(prototype),"Prototype view bindings could not be loaded as third-person entries");
    const auto converted = s.cinematicViews.front();
    Require(converted.id == item.id && converted.target == item.target && converted.point == item.point &&
        converted.radius == 900 && converted.idleDelay == 1.5f && converted.profile == CameraProfile::Default3p() &&
        converted.lockOnTightness == CinematicViews::kDefaultLockOnTightness,
        "Prototype binding/radius/timer was lost, or retired shot controls became camera settings");
    old->insert_or_assign("first_encounter",true);
    old->insert_or_assign("trigger","first_encounter");
    old->insert_or_assign("duration",1.0f);
    Require(s.ApplyTable(prototype) && s.cinematicViews.front() == converted,
        "Retired trigger/duration controls changed automatic radius behavior");
    const auto kept = s.cinematicViews;
    s.disableVanityCamera = true; s.vanityIdleSeconds = 600;
    RoundTrip(s);
    Require(s.cinematicViews == kept && CinematicViews::Eligible(s.cinematicViews.front(),true,100,true,1.5),
        "Disabling or retiming Vanity changes a view's independent idle trigger");
    s.disableVanityCamera = false; s.vanityIdleSeconds = 120;
    RoundTrip(s);
    Require(s.cinematicViews == kept,"Re-enabling Vanity changes view tuning");
    auto stones = landmark;
    stones.id = "lake-stones"; stones.name = "Lake from the stones";
    stones.point = {150000,90000,5000}; stones.triggerPoint = CinematicViews::Point{-5000,2000,300};
    auto cliff = stones;
    cliff.id = "lake-cliff"; cliff.name = "Lake from the cliff";
    cliff.triggerPoint = CinematicViews::Point{6000,-4000,2300}; cliff.profile.fov = 65;
    s.cinematicViews = {stones,cliff};
    const auto positionedViews = s.cinematicViews;
    RoundTrip(s);
    Require(s.cinematicViews == positionedViews && kCurrentPresetFormat >= 13,
        "Independent trigger positions and framing for the same scenery do not survive a preset round trip");
    const auto positionedTable = s.BuildSaveTable();
    Require(positionedTable["cinematic_views"][0]["trigger_point"].is_array(),"Trigger position was not written");
    for (const auto& badPoint : {toml::array{1,2},toml::array{1e30,0,0},toml::array{"wrong",2,3}}) {
        auto broken = positionedTable;
        (*broken["cinematic_views"].as_array())[0].as_table()->insert_or_assign("trigger_point",badPoint);
        Require(!s.ApplyTable(broken) && s.BuildSaveTable() == positionedTable,
            "Malformed trigger position partially applied a preset");
    }
    auto legacyTable = positionedTable;
    legacyTable["meta"].as_table()->insert_or_assign("format",12);
    for (auto& row : *legacyTable["cinematic_views"].as_array()) row.as_table()->erase("trigger_point");
    Require(s.ApplyTable(legacyTable) && !s.cinematicViews[0].triggerPoint && !s.cinematicViews[1].triggerPoint &&
        s.cinematicViews[0].point == stones.point && s.cinematicViews[1].profile == cliff.profile,
        "Format-12 views acquired invented trigger positions or lost their targets and framing");
    s.ResetAllToVanilla();
    Require(s.ApplyTable(toml::parse("[meta]\nformat = 11")) && s.cinematicViews.empty() && s.combatFraming == CombatFraming::Tuning{},
        "Older presets enabled new camera effects");
}
