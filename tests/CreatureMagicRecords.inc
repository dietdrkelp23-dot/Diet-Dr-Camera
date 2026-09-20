// Frozen Bethesda SPEL/MGEF/PROJ descriptors from the 2026-09-19 record audit.
// Each expected reaction is chosen for the actual attack, independently of the classifier.
static void CheckCreatureSpells()
{
    using namespace CreatureMagic;
    {
        // ccvsvsse003-necroarts.esl:000849 (explosion enchantment) -> :00083D
        Spell info;
        info.Include({"ccVSVSSE003_MGEF_BoneColossus_stompStagger", 33, -1, -1, -1, 1, 1, 2, true});
        Require(info.Impact(1, 6) == Kind::Force, "Bone colossus explosion stagger has no reaction");
    }
    {
        // Skyrim.esm:0434DD crSpider01PoisonSpit
        Spell info;
        info.Include({"crSpiderSpitFFAimed", 5, 24, 26, 40, 1, 2, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::VenomSpit}, "crSpider01PoisonSpit: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "crSpider01PoisonSpit: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:04CCF9 crSpider02PoisonSpit
        Spell info;
        info.Include({"crSpiderSpitFFAimed", 5, 24, 26, 40, 1, 2, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::VenomSpit}, "crSpider02PoisonSpit: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "crSpider02PoisonSpit: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:04CCFA crSpider03PoisonSpit
        Spell info;
        info.Include({"crSpiderSpitFFAimed", 5, 24, 26, 40, 1, 2, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::VenomSpit}, "crSpider03PoisonSpit: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "crSpider03PoisonSpit: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:0638B2 crSpider01PoisonBite
        Spell info;
        info.Include({"crSpiderBiteFFContact", 5, 24, 26, 40, 1, 1, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 5) == std::nullopt, "crSpider01PoisonBite: wrong contact path");
        Require(CastNoiseAllowed(1, 5, 1, info) == false, "crSpider01PoisonBite: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:06F6FE crChaurusPoisonSpit01
        Spell info;
        info.Include({"crChaurusSpitFFAimed", 0, 24, -1, 40, 1, 2, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::VenomSpit}, "crChaurusPoisonSpit01: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "crChaurusPoisonSpit01: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:1093B2 crChaurusPoisonSpit02
        Spell info;
        info.Include({"crChaurusSpitFFAimed", 0, 24, -1, 40, 1, 2, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::VenomSpit}, "crChaurusPoisonSpit02: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "crChaurusPoisonSpit02: wrong release/continuous/payload routing");
    }
    {
        // Dawnguard.esm:009D3C DLC1crChaurusPoisonSpit03
        Spell info;
        info.Include({"crChaurusSpitFFAimed", 0, 24, -1, 40, 1, 2, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::VenomSpit}, "DLC1crChaurusPoisonSpit03: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "DLC1crChaurusPoisonSpit03: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:0A041F crSprigganConcentration
        Spell info;
        info.Include({"SprigganBugDamageConcAimed", 0, 24, -1, 40, 2, 2, 2, true, true, false, true, false, false, false});
        Require(info.Impact(2, 0) == std::nullopt, "crSprigganConcentration: wrong contact path");
        Require(CastNoiseAllowed(2, 0, 2, info) == false, "crSprigganConcentration: wrong release/continuous/payload routing");
    }
    {
        // Dragonborn.esm:028E80 DLC2Seeker01MouthSpell
        Spell info;
        info.Include({"DLC2SeekerHealthEffect", 4, 24, -1, -1, 1, 2, 2, true, true, true, false, false, false, false});
        info.Include({"DLC2SeekerMagickaEffect", 4, 25, -1, -1, 1, 2, 2, true, true, true, false, false, false, false});
        info.Include({"DLC2SeekerStaminaEffect", 4, 26, -1, -1, 1, 2, 2, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Drain}, "DLC2Seeker01MouthSpell: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "DLC2Seeker01MouthSpell: wrong release/continuous/payload routing");
    }
    {
        // Dragonborn.esm:028E81 DLC2Seeker01RightHandSpell
        Spell info;
        info.Include({"DLC2SeekerRightHandEffect", 5, 24, 25, -1, 1, 2, 2, true, true, true, false, false, false, false});
        info.Include({"PerkImpactStaggerPushFFAimed", 33, -1, -1, -1, 1, 2, 4, false, false, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Magic}, "DLC2Seeker01RightHandSpell: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "DLC2Seeker01RightHandSpell: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:090355 crDwarvenCenturionSteamBreath
        Spell info;
        info.Include({"crDwarvenSteamBreath", 0, 24, -1, 41, 2, 2, 2, true, true, false, true, false, false, false});
        Require(info.Impact(2, 11) == std::nullopt, "crDwarvenCenturionSteamBreath: wrong contact path");
        Require(CastNoiseAllowed(2, 11, 2, info) == false, "crDwarvenCenturionSteamBreath: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:10E754 crDwarvenCenturionSteamBreathMal
        Spell info;
        info.Include({"crDwarvenSteamBreath", 0, 24, -1, 41, 2, 2, 2, true, true, false, true, false, false, false});
        Require(info.Impact(2, 11) == std::nullopt, "crDwarvenCenturionSteamBreathMal: wrong contact path");
        Require(CastNoiseAllowed(2, 11, 2, info) == false, "crDwarvenCenturionSteamBreathMal: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:0252C2 VoiceDragonFire01
        Spell info;
        info.Include({"VoiceDragonFireBreathEffect1", 0, 24, -1, 41, 2, 2, -1, true, true, false, true, true, false, false});
        Require(info.Impact(2, 11) == std::nullopt, "VoiceDragonFire01: wrong contact path");
        Require(CastNoiseAllowed(2, 11, 2, info) == false, "VoiceDragonFire01: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:0F80F4 VoiceDragonFrost01
        Spell info;
        info.Include({"VoiceDragonFrostBreathEffect1", 5, 24, 26, 43, 2, 2, -1, true, true, false, true, false, true, false});
        info.Include({"FrostSlowConcAimed", 34, 30, -1, 43, 2, 2, 2, true, true, false, false, false, false, false});
        Require(info.Impact(2, 11) == std::nullopt, "VoiceDragonFrost01: wrong contact path");
        Require(CastNoiseAllowed(2, 11, 2, info) == false, "VoiceDragonFrost01: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:0742CE VoiceDragonFireBall01
        Spell info;
        info.Include({"VoiceDragonFireballEffect1", 0, 24, -1, 41, 1, 2, 2, true, true, true, false, true, false, false});
        Require(info.Impact(1, 11) == std::optional{Kind::Fire}, "VoiceDragonFireBall01: wrong contact path");
        Require(CastNoiseAllowed(1, 11, 2, info) == true, "VoiceDragonFireBall01: wrong release/continuous/payload routing");
    }
    {
        // Dragonborn.esm:02AA10 DLC2LurkerSpray
        Spell info;
        info.Include({"DLC2crLurkerSprayConcAimed", 5, 24, 26, 40, 2, 2, -1, true, true, true, false, false, false, false});
        Require(info.Impact(2, 0) == std::nullopt, "DLC2LurkerSpray: wrong contact path");
        Require(CastNoiseAllowed(2, 0, 2, info) == false, "DLC2LurkerSpray: wrong release/continuous/payload routing");
    }
    {
        // Dragonborn.esm:028E8A DLC2crLurkerSpit01
        Spell info;
        info.Include({"DLC2crBenthicLurkerSpitFFAimed", 0, 24, -1, 40, 1, 2, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::VenomSpit}, "DLC2crLurkerSpit01: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "DLC2crLurkerSpit01: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:0EFC5F WispVolley
        Spell info;
        info.Include({"WispIceVolleyEffect", 5, 24, 26, 43, 1, 2, 2, true, true, true, false, false, true, false});
        info.Include({"PerkImpactStaggerPushFFAimed", 33, -1, -1, -1, 1, 2, 4, false, false, false, false, false, false, false});
        info.Include({"FrostSlowFFAimed", 34, 30, -1, 43, 1, 2, 2, true, true, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Frost}, "WispVolley: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 2, info) == true, "WispVolley: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:091F78 WispDrainAttack
        Spell info;
        info.Include({"FrostSlowFFContact", 34, 30, -1, 43, 1, 1, 2, true, true, false, false, false, false, false});
        info.Include({"EnchMagickaDamageFFContact", 0, 25, -1, 44, 1, 1, 2, true, true, false, false, false, false, false});
        info.Include({"WispDrainAttackEffect", 1, -1, -1, 44, 1, 1, 3, true, true, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Drain}, "WispDrainAttack: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "WispDrainAttack: wrong release/continuous/payload routing");
    }
    {
        // Dragonborn.esm:0271FB DLC2NetchShockTouch
        Spell info;
        info.Include({"DLC2NetchShockDamageFFContact", 5, 24, 25, 42, 1, 1, -1, true, true, false, false, false, false, true});
        Require(info.Impact(1, 0) == std::nullopt, "DLC2NetchShockTouch: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "DLC2NetchShockTouch: wrong release/continuous/payload routing");
    }
    {
        // ccvsvsse002-pets.esl:00081A ccVSVSSE002_SPELL_WebTrap
        Spell info;
        info.Include({"ccVSVSSE002_MGEF_WebTrap", 21, 53, -1, -1, 1, 1, -1, true, false, true, false, false, false, false});
        info.Include({"ccVSVSSE002_MGEF_StaggerCont", 33, -1, -1, -1, 1, 1, -1, false, false, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Web}, "ccVSVSSE002_SPELL_WebTrap: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "ccVSVSSE002_SPELL_WebTrap: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:02FFD2 crGiantStomp
        Spell info;
        info.Include({"crStaggerAttackAreaEffectGiantStomp", 33, -1, -1, -1, 1, 1, 2, true, false, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Force}, "crGiantStomp: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "crGiantStomp: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:104D43 crGiantClubSlam
        Spell info;
        info.Include({"crStaggerAttackAreaEffectGiantSlam", 33, -1, -1, -1, 1, 1, 2, true, false, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Force}, "crGiantClubSlam: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "crGiantClubSlam: wrong release/continuous/payload routing");
    }
    {
        // Dragonborn.esm:03CEF5 DLC2LurkerStomp
        Spell info;
        info.Include({"DLC2LurkerStompEffect", 33, -1, -1, -1, 1, 1, 2, true, false, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Force}, "DLC2LurkerStomp: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "DLC2LurkerStomp: wrong release/continuous/payload routing");
    }
    {
        // ccvsvsse003-necroarts.esl:000862 ccVSVSSE003_SPELL_BoneColossus_stomp
        Spell info;
        info.Include({"ccVSVSSE003_MGEF_BoneColossus_stompEff", 1, -1, -1, -1, 1, 1, 2, true, false, false, false, false, false, false});
        // The carrier spawns an explosion. Its stagger enchantment below
        // supplies the contact, not this script-archetype application.
        Require(info.Impact(1, 0) == std::nullopt, "ccVSVSSE003_SPELL_BoneColossus_stomp: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "ccVSVSSE003_SPELL_BoneColossus_stomp: wrong release/continuous/payload routing");
    }
    {
        // Dawnguard.esm:00C5F4 DLC1crDeathHoundMeleeAttack
        Spell info;
        info.Include({"crAtronachFrostDamageFFContact", 5, 24, 26, 43, 1, 1, 2, true, true, true, false, false, true, false});
        info.Include({"FrostSlowFFContact", 34, 30, -1, 43, 1, 1, 2, true, true, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::nullopt, "DLC1crDeathHoundMeleeAttack: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "DLC1crDeathHoundMeleeAttack: wrong release/continuous/payload routing");
    }
    {
        // Skyrim.esm:0A26F4 crIceWraithBite
        Spell info;
        info.Include({"crIceWraithBiteFFContact", 5, 24, 26, 43, 1, 1, -1, true, true, true, false, false, false, false});
        Require(info.Impact(1, 0) == std::nullopt, "crIceWraithBite: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "crIceWraithBite: wrong release/continuous/payload routing");
    }
    {
        // Dawnguard.esm:011A1A DLC1crGargoyleAbsorbHealth
        Spell info;
        info.Include({"DLC1AbsorbHealthFFContact", 4, 24, -1, 44, 1, 1, -1, true, true, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::nullopt, "DLC1crGargoyleAbsorbHealth: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "DLC1crGargoyleAbsorbHealth: wrong release/continuous/payload routing");
    }
    {
        // Dawnguard.esm:010F68 DLC1crGargoyleAbsorbHealthStagger
        Spell info;
        info.Include({"DLC1AbsorbHealthFFContact", 4, 24, -1, 44, 1, 1, -1, true, true, false, false, false, false, false});
        info.Include({"StaggerAttackFFContact", 33, -1, -1, -1, 1, 1, 2, true, false, false, false, false, false, false});
        Require(info.Impact(1, 0) == std::optional{Kind::Force}, "DLC1crGargoyleAbsorbHealthStagger: wrong contact path");
        Require(CastNoiseAllowed(1, 0, 1, info) == false, "DLC1crGargoyleAbsorbHealthStagger: wrong release/continuous/payload routing");
    }
    {
        // Dawnguard.esm:015C40 DLC1LD_ForgemasterFireBreath01
        Spell info;
        info.Include({"DLC1LD_ForgemasterFireBreathEffect", 0, 24, -1, 41, 2, 2, -1, true, true, false, true, true, false, false});
        Require(info.Impact(2, 11) == std::nullopt, "DLC1LD_ForgemasterFireBreath01: wrong contact path");
        Require(CastNoiseAllowed(2, 11, 2, info) == false, "DLC1LD_ForgemasterFireBreath01: wrong release/continuous/payload routing");
    }
}
