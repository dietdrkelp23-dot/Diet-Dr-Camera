static void CheckCreatureRoster()
{
    using F = Reaction::Family;
    using K = Reaction::Kind;
    struct Race { const char* name; const char* skeleton; unsigned flags; K expected; };
    constexpr Race races[]{
#include "DamageReactionCreatureRoster.inc"
    };
    static_assert(std::size(races) == 144);
    for (const auto& race : races) {
        const auto family = Reaction::CreatureFamily(race.name, race.skeleton,
            race.flags & 1, race.flags & 2, race.flags & 4, race.flags & 8);
        if (Reaction::ClassifyAttack(family,K::Unarmed,"","",false) != race.expected)
            throw std::runtime_error(std::string("Creature reaction coverage mismatch: ") + race.name);
    }
    // Renamed mod variants still inherit a known physical rig. Display names
    // and broad words such as 'giant' must not turn humanoids into creatures.
    for (const auto& [rig, expected] : {
        std::pair{"ACTORS/ATRONACHFROST/Character Assets/skeleton.nif",F::FrostAtronach},
        {"Actors/DwarvenSpider/Character Assets/skeleton.nif",F::DwarvenSpider},
        {"Actors/Canine/Character Assets Dog/skeleton.nif",F::BitingCreature},
        {"Actors/DLC01/VampireBrute/Character Assets/skeleton.nif",F::HeavyClawed},
        {"Actors/DLC02/BenthicLurker/Character Assets/skeleton.nif",F::Lurker},
        {"Actors/DLC02/DwarvenBallistaCenturion/Character Assets/skeleton.nif",F::DwarvenBallista},
        {"Actors/Character/Character Assets/skeleton.nif",F::Other}})
        Require(Reaction::CreatureFamily("RenamedByMod",rig,false,false,false) == expected,
            "A renamed creature lost the fallback for its actual skeleton");
    Require(Reaction::CreatureFamily("RomanCenturionRace","Actors/Character/Character Assets/skeleton.nif",false,false,false) == F::Other &&
        Reaction::CreatureFamily("GiantSpiderBossRace","Actors/FrostbiteSpider/Character Assets/skeleton.nif",false,false,true) == F::Spider &&
        Reaction::CreatureFamily("NewConstructRace","custom.nif",false,false,false,true) == F::Construct &&
        Reaction::CreatureFamily("NewCreatureRace","custom.nif",false,false,true) == F::Creature,
        "Unknown or misleading identities bypassed conservative creature fallbacks");
    std::cout << "Creature roster: " << std::size(races) << " attacking race records covered\n";
}

static void CheckAttackProfiles()
{
    using F = Reaction::Family;
    using K = Reaction::Kind;
    Require(Reaction::CreatureFamily("DwarvenCenturionRace","",false,false,true) == F::Centurion &&
        Reaction::CreatureFamily("CustomGuardian","Actors/DwarvenSteamCenturion/Character Assets/skeleton.nif",false,false,true) == F::Centurion,
        "Centurion identity misses the vanilla race or a renamed skeleton variant");
    Require(Reaction::CreatureFamily("RomanCenturionRace","",false,false,false) == F::Other &&
        Reaction::CreatureFamily("FrostbiteSpiderRaceGiant","",false,false,true) == F::LargeSpider,
        "An unrelated centurion or giant spider was treated as a heavy automaton or giant");
    Require(Reaction::CreatureFamily("DLC2GhostFrostGiantRace","",false,false,true) == F::Giant &&
        Reaction::CreatureFamily("ModdedGiant","",true,false,true) == F::Giant,
        "Giant variants lost their attack family");
    Require(Reaction::CreatureFamily("DLC1TrollRaceArmored","",false,false,true) == F::HeavyClawed &&
        Reaction::CreatureFamily("WolfRace","",false,false,true) == F::BitingCreature,
        "Heavy creatures and biting animals use the same fallback");

    // ATKE values from Skyrim.esm and node names from steambehavior.hkx.
    struct Case { F family; K weapon; const char* event; const char* clip; bool bash; K expected; };
    for (const auto& c : {
        Case{F::Giant,K::HeavyBlunt,"attackStart_ClubAttack1","",false,K::GiantClub},
        {F::Giant,K::Unarmed,"attackStart_ClubAttack2","",false,K::GiantClub},
        {F::Giant,K::Unarmed,"attackPowerStart_ForwardPowerAttack","",false,K::GiantClub},
        {F::Giant,K::HeavyBlunt,"attackPowerStart_Stomp","",false,K::GiantStomp},
        {F::Giant,K::HeavyBlunt,"attackStart_HandSwipeAttack","",false,K::GiantSwipe},
        {F::Giant,K::HeavyBlunt,"bashStart","",true,K::GiantSwipe},
        {F::Centurion,K::Unarmed,"attackStartChop","H2HAttackLeftChop",false,K::CenturionAxe},
        {F::Centurion,K::Unarmed,"attackStartChop","H2HAttackRightChop",false,K::CenturionHammer},
        {F::Centurion,K::Unarmed,"attackStartSlash","H2HAttackLeftSlash",false,K::CenturionAxe},
        {F::Centurion,K::Unarmed,"attackStartSlash","H2HAttackRightSlash",false,K::CenturionHammer},
        {F::Centurion,K::Unarmed,"attackStartStab","H2HAttackRightStab",false,K::CenturionHammer},
        {F::Centurion,K::Unarmed,"attackStartForwardPowerRushLeft","",false,K::CenturionAxe},
        {F::Centurion,K::Unarmed,"attackStartForwardPowerRight","",false,K::CenturionHammer},
        {F::Centurion,K::Unarmed,"attackStartForwardPowerRight","moddedAttack",false,K::CenturionHammer},
        {F::Centurion,K::Unarmed,"attackStartChop","",false,K::Centurion},
        {F::Centurion,K::Unarmed,"moddedAttack","newUnknownAttack",false,K::Centurion},
        {F::FrostAtronach,K::Unarmed,"attackStart_Attack_R1","",false,K::FrostAtronachSpike},
        {F::FrostAtronach,K::Unarmed,"attackStart_Attack_L1","",false,K::FrostAtronachBlunt},
        {F::FrostAtronach,K::Unarmed,"attackPowerStart_ForwardPowerAttack_R1","",false,K::FrostAtronachSpike},
        {F::FrostAtronach,K::Unarmed,"attackPowerStart_PowerAttack_L1","",false,K::FrostAtronachBlunt},
        {F::FrostAtronach,K::Unarmed,"customAttack","PowerAttack_R1",false,K::FrostAtronachSpike},
        {F::FrostAtronach,K::Unarmed,"attackStart_Attack_L1","CustomAttack",false,K::FrostAtronachBlunt},
        {F::FrostAtronach,K::Unarmed,"bashPowerStart","PowerBash",true,K::FrostAtronachBlunt},
        {F::FrostAtronach,K::Unarmed,"customAttack","unrecognizedAttack",false,K::ConstructBlunt},
        {F::Lurker,K::HeavyBlade,"attackPowerStart_Stomp","",false,K::CreatureStomp},
        {F::Lurker,K::HeavyBlade,"attackStart_ClubAttack1","",false,K::CreatureHeavy},
        {F::Lurker,K::HeavyBlade,"attackStart_HandSwipeAttack","",false,K::CreatureHeavy},
        {F::BoneColossus,K::HeavyBlade,"attackPowerStart_Stomp","",false,K::CreatureStomp},
        {F::BoneColossus,K::HeavyBlade,"attackStart_ClubAttack1","",false,K::CreatureHeavy},
        {F::Dragon,K::Unarmed,"attackStartTailRight","",false,K::DragonTail},
        {F::Dragon,K::Unarmed,"attackStartWingLeft","",false,K::DragonWing},
        {F::Dragon,K::Unarmed,"attackStartBite","",false,K::DragonBite},
        {F::HeavyCreature,K::Unarmed,"attackStartPowerComboA","",false,K::CreatureHeavy},
        {F::HeavyCreature,K::HeavyBlade,"attackStart_AttackLtoR","",false,K::CreatureHeavy},
        {F::HeavyClawed,K::Unarmed,"attackStartPowerComboA","",false,K::HeavyClaw},
        {F::HeavyClawed,K::Unarmed,"AttackStartDualRunning","",false,K::HeavyClaw},
        {F::HeavyClawed,K::Unarmed,"AttackStartBite","",false,K::HeavyBite},
        {F::HeavyClawed,K::Unarmed,"bashStart","",true,K::CreatureHeavy},
        {F::HeavyClawed,K::Blunt,"customWeaponAttack","",false,K::Blunt},
        {F::StoneConstruct,K::Unarmed,"attackStart_Attack_Swipe","",false,K::ConstructBlunt},
        {F::DwarvenSpider,K::Unarmed,"AttackStart_LeftStab","",false,K::MechanicalPierce},
        {F::DwarvenSpider,K::Unarmed,"AttackStartLungeBite","",false,K::MechanicalPierce},
        {F::DwarvenSphere,K::Unarmed,"attackStartSlash","",false,K::MechanicalBlade},
        {F::DwarvenSphere,K::Arrow,"attackStartSlash","",false,K::Arrow},
        {F::DwarvenBallista,K::Arrow,"attackStartStab","",false,K::HeavyProjectile},
        {F::Mammoth,K::Unarmed,"attackStart_AttackLeft1","",false,K::HeavyTusk},
        {F::Mammoth,K::Unarmed,"attackStartStomp","",false,K::CreatureStomp},
        {F::Tusked,K::Unarmed,"attackStart_HorkerSwim","",false,K::Tusk},
        {F::Tusked,K::HeavyBlade,"attackStart_ForwardPowerLarge","",false,K::Tusk},
        {F::Hoofed,K::Unarmed,"attackStart_Attack1","",false,K::Hoof},
        {F::Horned,K::Unarmed,"attackStart_ForwardPower","",false,K::Ram},
        {F::Netch,K::Unarmed,"attackStartRight","",false,K::Tentacle},
        {F::SmallBiter,K::Unarmed,"attackStart_Attack1","",false,K::SmallBite},
        {F::Chaurus,K::Unarmed,"AttackStart_RHeadBash","",false,K::Blunt},
        {F::Chaurus,K::Unarmed,"AttackStart_RBite3","",false,K::CreatureBite},
        {F::Chaurus,K::Unarmed,"AttackStart_LeftChop","",false,K::CreatureClaw},
        {F::Chaurus,K::Unarmed,"attackStart_AttackRtoL","",false,K::CreatureBite},
        {F::Chaurus,K::Unarmed,"attackPowerStart_PowerAttack","",false,K::CreatureBite},
        {F::LargeSpider,K::Unarmed,"AttackStartLungeBite","",false,K::HeavyBite},
        {F::LargeSpider,K::Unarmed,"AttackStart_RightChop","",false,K::HeavyClaw},
        {F::Spider,K::Unarmed,"AttackStart_RightChop","",false,K::CreatureClaw},
        {F::Spider,K::Unarmed,"AttackStartLungeBite","",false,K::CreatureBite},
        {F::Mudcrab,K::Unarmed,"AttackStart_RightChop","",false,K::CreatureClaw},
        {F::IceWraith,K::Unarmed,"attackPowerStart_GroundDive","",false,K::CreatureBite},
        {F::Spirit,K::Unarmed,"attackStart_TouchPush","",false,K::SpiritTouch},
        {F::Clawed,K::Unarmed,"attackStart_L1","",false,K::CreatureClaw},
        {F::SpearUser,K::HeavyBlade,"attackStart_Spear_Attack1","",false,K::Spear},
        {F::SpearUser,K::Blunt,"attackStart_MT_Attack1","",false,K::Blunt},
        {F::SpearUser,K::Arrow,"attackStart_Spear_Attack1","",false,K::Arrow},
        {F::Construct,K::Unarmed,"customAttack","",false,K::Blunt},
        {F::BitingCreature,K::Unarmed,"attackStart_Attack1","",false,K::CreatureBite},
        {F::Creature,K::Unarmed,"AttackStartLungeBite","",false,K::CreatureBite},
        {F::Creature,K::Unarmed,"AttackStart_RightChop","",false,K::CreatureClaw},
        {F::Creature,K::Blade,"attackStart","",false,K::Blade},
        {F::Other,K::Unarmed,"bashStart","",true,K::Blunt},
        {F::Other,K::Arrow,"bashStart","",true,K::Blunt},
        {F::DwarvenSphere,K::Arrow,"bashStart","",true,K::Blunt},
        {F::Other,K::Unarmed,"moddedAttack","",false,K::Unarmed},
        {F::Centurion,K::Arrow,"attackStartChop","H2HAttackLeftChop",false,K::Arrow}})
        Require(Reaction::ClassifyAttack(c.family,c.weapon,c.event,c.clip,c.bash) == c.expected,
            "Attack classification disagrees with the weapon, creature or actual striking hand");

    Require(Reaction::Shape(K::GiantClub).weight > Reaction::Shape(K::HeavyBlunt).weight &&
        Reaction::Shape(K::CenturionHammer).rise > 1.4f*Reaction::Shape(K::CenturionAxe).rise &&
        Reaction::Shape(K::CenturionHammer).weight > Reaction::Shape(K::CenturionAxe).weight,
        "Giant club and centurion hands lost their different impact weight/timing");
    Require(Reaction::Shape(K::FrostAtronachBlunt).rise == Reaction::Shape(K::CenturionHammer).rise &&
        Reaction::Shape(K::FrostAtronachSpike).rise == Reaction::Shape(K::CenturionAxe).rise &&
        Reaction::Shape(K::FrostAtronachBlunt).weight > Reaction::Shape(K::FrostAtronachSpike).weight &&
        Reaction::Shape(K::HeavyProjectile).weight > Reaction::Shape(K::Arrow).weight &&
        Reaction::Shape(K::SmallBite).weight < Reaction::Shape(K::CreatureBite).weight,
        "Creature contacts lost their sharp/blunt distinction or scale ordering");
    Require(Reaction::NeedsAttackClip(F::FrostAtronach) && Reaction::NeedsAttackClip(F::Centurion) &&
        !Reaction::NeedsAttackClip(F::Dragon) && !Reaction::NeedsAttackClip(F::Other),
        "Animation tracking escaped the bounded asymmetric-arm families");
    const Reaction::Basis attacker{{-1,0,0},{0,-1,0},{0,0,1}};
    auto axe = Hit(K::CenturionAxe), hammer = Hit(K::CenturionHammer);
    axe.towardSource = hammer.towardSource = {0,1,0};
    axe.strikeDirection = attacker.ToWorld(Reaction::LocalStrike(axe.kind,"attackStartSlash","H2HAttackLeftSlash"));
    hammer.strikeDirection = attacker.ToWorld(Reaction::LocalStrike(hammer.kind,"attackStartSlash","H2HAttackRightSlash"));
    const auto axeAxes = Reaction::ContactDirection(axe,basis,true), hammerAxes = Reaction::ContactDirection(hammer,basis,true);
    Require(axeAxes.yaw > .8f && hammerAxes.yaw < -.8f && axeAxes.pitch < 0 && hammerAxes.pitch < 0,
        "Mirrored centurion sweeps do not deflect along their actual incoming direction");
    hammer.strikeDirection = attacker.ToWorld(Reaction::LocalStrike(hammer.kind,"attackStartChop","H2HAttackRightChop"));
    const auto chop = Reaction::ContactDirection(hammer,basis,true);
    Require(chop.pitch < -.85f && std::abs(chop.yaw) < .2f, "A hammer chop does not compress the view downward");

    Reaction::AttackClips clips;
    std::array<Reaction::AttackClips::Scope,16> scopes{};
    scopes[0] = {100,7};
    clips.Observe(100,101,"H2HAttackLeftChop",true,false,10);
    Require(clips.At(7,10).Name().empty(), "Inactive damage reaction captures enemy clips");
    clips.SetScopes(scopes,1);
    clips.Observe(200,101,"H2HAttackLeftChop",true,false,10);
    Require(clips.At(7,10).Name().empty(), "A different actor supplied a striking hand");
    clips.Observe(100,101,"H2HAttackLeftChop",true,false,10);
    clips.Observe(100,102,"H2HAttackRightSlash",true,false,10.02);
    clips.Observe(100,101,"H2HAttackLeftChop",true,true,10.03);
    Require(clips.At(7,10.03).Name() == "H2HAttackRightSlash", "An outgoing clip's update replaced the new attack");
    clips.Observe(100,101,"",false,false,10.04);
    clips.SetScopes(scopes,1);
    Require(clips.At(7,10.04).Name() == "H2HAttackRightSlash", "Scope refresh or an older clip's exit lost the live hand");
    Require(clips.At(7,10.3).Name().empty(), "A stale animation supplied the current hit's hand");
    scopes[0] = {100,8}; clips.SetScopes(scopes,1);
    Require(clips.At(8,10.04).Name().empty(), "Reusing a graph address retained another actor's attack");
    clips.Observe(100,103,"H2HAttackLeftStab",true,true,10.05);
    Require(clips.At(8,10.05).Name() == "H2HAttackLeftStab", "Enabling mid-attack did not acquire the active clip");
    clips.Clear(); clips.Observe(100,103,"H2HAttackLeftStab",true,true,10.06);
    Require(!clips.Enabled() && clips.At(8,10.06).Name().empty(), "A reset retained enemy animation context");
}

static void CheckFirstPersonTravel()
{
    for (int kind = 0; kind < static_cast<int>(Reaction::Kind::Count); ++kind) for (int fps : {30,60,144,240}) {
        Reaction::Mixer mixer;
        auto hit = Hit(static_cast<Reaction::Kind>(kind)); hit.towardSource = {0,1,0};
        mixer.Add(hit,{3},basis,true,10);
        float peak = 0, peakTime = 0, atQuarter = 0;
        Reaction::Rotation previous{};
        for (int frame = 1; frame <= 2*fps; ++frame) {
            const auto r = mixer.Advance(1.0f/fps,true);
            if (r.pitch > peak) { peak = r.pitch; peakTime = static_cast<float>(frame)/fps; }
            if (frame == static_cast<int>(std::lround(.25f*fps))) atQuarter = r.pitch;
            if (frame > fps/2) Require(Magnitude(r) == 0, "First-person impact acquired a prolonged recovery");
            previous = r;
        }
        Require(peakTime > 0 && peakTime <= .115f && std::abs(atQuarter) < .18f*peak,
            "First-person travel was increased by stretching out the motion instead of moving the view farther");
        if (hit.kind == Reaction::Kind::HeavyBlade)
            Require(peakTime <= .05f && peak > .190f && peak < .200f,
                "Greatsword lost its quick response or roughly doubled directional travel");
        Require(Magnitude(previous) == 0, "Impact did not settle completely");
    }
    Reaction::Mixer delayed, timely;
    delayed.Add(Hit(Reaction::Kind::HeavyBlade),{3},basis,true,10.12);
    timely.Add(Hit(Reaction::Kind::HeavyBlade),{3},basis,true,10);
    Require(Magnitude(delayed.Advance(0,true)) == 0,
        "A delayed contact appeared halfway through the recoil");
    for (int frame = 0; frame < 60; ++frame)
        Require(Difference(delayed.Advance(1.0f/60,true),timely.Advance(1.0f/60,true)) < 1e-7f,
            "Callback delay shortened the visible impact travel");

    Reaction::Inbox inbox;
    inbox.SetView(1);
    auto magic = Hit(Reaction::Kind::Magic);
    for (int i = 0; i < 20; ++i) inbox.Push(magic,inbox.Token());
    std::array<Reaction::Contact,64> contacts{};
    Require(inbox.Drain(10,contacts) == 1, "Multi-effect spell notifications multiply one impact");
    inbox.Push(magic,inbox.Token());
    magic.time += .02; inbox.Push(magic,inbox.Token());
    magic.attacker = 8; inbox.Push(magic,inbox.Token());
    Require(inbox.Drain(10.02,contacts) == 3, "Contact coalescing discarded a new hit or another attacker");
}
