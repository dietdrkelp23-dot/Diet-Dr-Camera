# Damage Reaction creature coverage

Reviewed September 19, 2026 against UESP and the installed Bethesda game data.
This is an attack and event-path audit, not a claim that every encounter has
been tested in a running game. Camera response values are design choices based
on the contact type; UESP does not prescribe camera recoil values.

## Evidence and scope

The [UESP creature index](https://en.uesp.net/wiki/Skyrim:Creatures) supplied the
roster. A read-only scan of the five base/expansion masters and 74 installed
Creation Club plugins found 240 race records including overrides, representing
195 distinct race identities. All 144 final non-NPC races with authored attack
events are in `tests/DamageReactionCreatureRoster.inc`. This includes some
unused/test races and normally peaceful animals, which may attack under magic.
NPC races use their actual weapon and damaging magic-effect classifications.
The follow-up audit also follows NPC templates and attack spells into effects,
projectiles, explosions, hazards, enchantments and script properties. It includes
all 152 final non-NPC race records, including the eight without attack events;
normal NPC races continue to use their equipped weapons, spells and shouts.
Leveled templates retain the relationship between a chosen race and its spells.

`tools/audit_creature_attacks.py` can reproduce that scan. The detailed race/event/
effect matrix is `build/diagnostics/creature-attacks-20260919/creature-roster.md`,
with resolved records, script references and input hashes alongside it. The
record layouts come from [xEdit's Skyrim definitions](https://github.com/TES5Edit/TES5Edit/blob/dev-4.1.6/Core/wbDefinitionsTES5.pas).
Kykvendi was inspected separately from the Bethesda content. A read-only scan
of the active Finale profile resolved 1,293 installed plugins to check whether
the reported spider spit had been replaced: its poison spell/effect records
still resolve to the original Skyrim records.

Local evidence is under `build/diagnostics/damage-enemies-20260919/`: plugin
hashes, extracted race/weapon/attack records, UESP reference pages, frost
atronach/centurion attack names, test/build logs and deployment records.
The record scanner does not load or modify the game, saves or installed plugins.

## Projectile, effect and nearby-noise audit

Incoming reactions require contact with the player. Nearby attack noise belongs
to the attacker and can play when an attack misses. They remain independent
settings. **Dragon breath and centurion steam/Forgemaster breath keep their
dedicated Cinematic Effects noise in both views.** The cinematic scanner and
generic cast, shout, melee, archery and projectile-noise paths now use the same
race/rig ownership rule. Setting a dedicated source to zero never enables a
generic fallback. Continuous breath damage remains a smooth incoming stream,
not repeated projectile jolts.

The [frostbite spider reference](https://en.uesp.net/wiki/Skyrim:Frostbite_Spider)
and `crSpider01/02/03PoisonSpit` identify the ordinary ranged attack as poison
spit. Its three-second damage effect previously supplied only the softer poison
stream. A real projectile contact now adds a short venom impact. Its effect has
no magic school, which previously excluded its nearby release noise. Hostile
school-less attacks now reuse the appropriate Destruction or Alteration tuning.
Actual webs/paralysis, including Arachnia's web trap, have a contact path even
without health damage. Utility movement penalties and self-applied effects do
not gain reactions.

| Enemy type / variants | Attacks checked beyond the physical table | Reaction and nearby-noise handling |
| --- | --- | --- |
| Wolves, dogs, foxes, huskies, sabre cats, bears, trolls | Bites, swipes, lunges/power attacks; disease payloads where present | Existing NPC melee noise retained. Body contacts use their physical family; diseases do not create extra impacts. |
| Skeevers, slaughterfish, ash hoppers | Small bites, lunges; poison/disease variants | Small physical contact; damaging poison remains a stream. |
| Mammoths, horkers, bristlebacks | Tusks, body/head blows, charges/stomps | Physical family and power/stomp classification; NPC melee noise. |
| Horses/reindeer, deer/elk, goats/cows | Hooves and rams when provoked or commanded | Physical contacts only when an attack actually occurs; movement itself does not generate attack noise. |
| Hares, rabbits, chickens and non-attacking props/test races | No authored combat attack | No invented hit or attack noise. |
| Frostbite spiders, snow/large/giant variants, Nimhe | Spit, poisoned bites, lunges, leg chops | Venom projectile impact plus poison stream; school-less spit release uses NPC magic noise. Bite poison does not manufacture a ranged release. |
| Web Mother, Arachnia and replacement spiders | Stronger poison, paralysis and web traps | Same poison path; hostile web/paralysis contacts can react without a health tick. Own traps do not count as incoming attacks. |
| Chaurus/reapers/hunters | Poison spit/bites, head bash, flying sweeps/dives | Venom impact and poison stream; ranged release gets magic noise, body attacks get melee noise. |
| Mudcrabs, including Solstheim/Fishing variants | Pincers and special enchanted/fire attacks | Physical contacts plus actual damaging effects; harmless visual effects stay quiet. |
| Spriggans, matrons, earth mothers, burnt/corrupted variants | Poison claws, insect streams, fire stream/cloak, healing and animal calls | Poison/fire damage streams; nearby concentration noise follows casting; claws retain melee noise. Healing/calls do not become incoming hits. |
| Hagravens and Falmer variants | Claws, weapons, fire/frost/shock, wards/healing | Actual weapon or elemental path and the corresponding NPC melee/magic source. |
| Ice wraiths, Karstaag's wraiths, spectral dragon/fire wyrms | Cold bites and elemental breath/contact effects | Physical bite plus actual frost/fire damage. Borrowing a dragon name does not invent a grounded dragon body attack. |
| Wisps, wispmothers, shades, anomalies | Frost volley/streams, health/magicka drain, slowing contact, summoned shades | Elemental projectile/stream or drain contact; actual harmful magic keywords supplement resistance-based identification. |
| Flame atronachs | Firebolts, flames, fiery melee, cloak, death explosion | Discrete bolt impact, fire stream/contact effects and damaging blast/enchantment paths; caster noise stays separate from victim contact. |
| Frost atronachs | Spike/club melee, frost damage/slow, frost cloak | Existing asymmetric arms plus frost damage. A secondary slow does not add a second magical impact to the same damaging melee spell. |
| Storm atronachs and ash guardians | Blunt melee, lightning/chain lightning or ash/fire attacks, area blasts | Construct contacts, actual element, continuous/discrete spell handling and blast/stagger contacts. |
| Dwarven spiders | Mechanical melee, shock projectile, death discharge | Mechanical contact and shock damage; cast/launch noise for the ranged attack. |
| Dwarven spheres/ballistae | Blade/bolts and heavy ballista shots | Mechanical blade, arrow/bolt or heavy projectile response; NPC melee/archery controls. They are not centurions for cinematic ownership. |
| Centurions, Forgemaster and centurion variants | Axe, hammer, steam/fire/frost breath | Existing physical distinction and incoming damage stream; dedicated cinematic noise exclusively owns their nearby activity. |
| Giants, frost giants, Karstaag | Club/slam, swipe, stomp/area stagger; Karstaag's cold effects | Existing physical family, area force contact where there is no health tick, and actual frost streams. |
| Lurkers/vindicators | Hand blows, stomp explosion/stagger, tentacle spit/spray and slowing residue | Heavy physical contact; poison-like spit impact/stream, smooth spray and hostile slowing/force contacts. |
| Seekers/high seekers | Knowledge-drain projectile, multi-resource Seeker Drain, duplicated summons | Projectile impact plus one health-drain stream; health/magicka/stamina components do not create three launch beats. |
| Netch/calves | Tentacle/head contact with shock payload | Physical strike plus real shock; the contact spell does not pretend to be a separate ranged cast. |
| Rieklings/mounted rieklings, goblins | Spears/thrown ammunition, bristleback charge; shaman weapons/spells | Actual spear/projectile/tusk path, melee/archery/magic noise as appropriate. |
| Werewolves/werebears, vampire lords | Claws, power/knockback attacks, howls; lord drain/poison/bats | Physical or harmful spell reactions; existing transformation noise controls retain ownership. |
| Gargoyles and sentinel/brute variants | Absorbing claws and stagger | Heavy claws with drain/force applications; no ranged release noise for the attached contact spell. |
| Death hounds | Bite with frost damage and slow | Bite plus frost; secondary slowing does not duplicate the hit. |
| Draugr/deathlords, skeletons, bonemen/mistmen/wrathmen, keepers | Weapons/bows, elemental spells, summons, disarm/Unrelenting Force | Weapon/arrow/elemental paths plus actual control/force hits; NPC melee, archery, magic and shout noise. |
| Dragon priests, acolytes, Ayleid liches | Staves, elemental spells/cloaks, drain, summons/wards | Actual spell/enchantment semantics; utility/summoning visuals do not become incoming hits. |
| Ash spawn | Formed weapons, firebolts and higher-tier flame cloak | Real weapon and fire contact/stream handling. |
| Bone colossi, wights, zombies, tortured shades, wailing wraiths | Heavy body/stomp and shout, weapons or spectral frost/drain depending on type | Physical family or actual spell; colossus stomp is traced through its carrier effect into the explosion's stagger enchantment. |
| Dragons, Alduin, Durnehviir, revered/legendary/serpentine variants | Bite, wing, tail, fire/frost/shock breath, projectiles, Drain Vitality and special summons | Body contacts, discrete projectile contacts and continuous damage/drain; dedicated dragon cinematic noise excludes every generic NPC noise path. Summoning is not a victim hit. |
| Dragonborn exploding/jumping/cloaked spiders | Fire/frost/shock/poison blasts, cloaks, spit and residue | Real damage/enchantment/hazard paths. Camera typing follows the effective records, including any patched elemental changes. |
| Bone wolves, nix-hounds, elytra and other CC pets/variants | Bites or known rig attacks, poison/elemental variants where authored | Body family plus actual spell data. Companion status does not create an extra attack or suppress a real incoming hit. |
| Humanoids: bandits, Forsworn, mages, vampires, cultists, afflicted, bosses | Equipped weapons, bows/staves, elemental/drain spells, shouts and afflicted bile | Shared weapon, projectile, spell and continuous-damage paths; no exhaustive name table is required. |

Specific references supplementing the creature indexes include
[Seeker](https://en.uesp.net/wiki/Skyrim:Seeker),
[Wispmother](https://en.uesp.net/wiki/Skyrim:Wispmother),
[Spider Scrolls](https://en.uesp.net/wiki/Skyrim:Spider_Scrolls),
[Drain Vitality](https://en.uesp.net/wiki/Skyrim:Drain_Vitality),
[Karstaag](https://en.uesp.net/wiki/Skyrim:Karstaag), and
[Dwarven Automatons](https://en.uesp.net/wiki/Skyrim:Dwarven_Automatons).
The recorded forms, rather than translated names or a wiki's approximate damage
numbers, determine the runtime classification.

## Physical contacts

| Enemies | Classification |
| --- | --- |
| Giants, Forgotten Vale frost giants and Karstaag | Existing separate club, hand swipe and stomp responses. Their club weapon proxies are not interpreted as ordinary swords. |
| Centurions and the Forgemaster | Existing left axe/right hammer distinction, resolved through active animation clips where shared attack events omit the hand. |
| Frost atronachs | Right ice spike uses the centurion axe's sharp-impact timing/weight; left club and bashes use the hammer's blunt-impact timing/weight. Unknown replacement motions use a general heavy construct contact. |
| Storm atronachs and ash guardians | Heavy blunt stone contact. Actual shock/fire applications retain their separate elemental reactions. |
| Dwarven spiders, spheres and ballistae | Piercing mechanical legs, mechanical blades, and heavier ballista projectiles respectively. Sphere crossbow bolts retain the ordinary projectile response. |
| Bears, sabre cats, trolls, werewolves, werebears, gargoyles and vampire lords | Heavy claws, with heavy bites when the attack identifies a bite; bashes retain a blunt contact. |
| Wolves, dogs, foxes, huskies, death hounds, bone wolves and nix-hounds | Bites. Skeevers, slaughterfish and ash hoppers use a smaller bite response. |
| Mammoths, horkers and bristlebacks | Heavy tusk/body blows for mammoths, lighter tusk blows for horkers and bristlebacks. Mounted riekling charge proxies follow the boar's contact. Explicit stomp events retain a vertical impulse. |
| Horses/reindeer, goats, cows, deer and elk | Hoof or head/antler ram contacts instead of claws. |
| Chaurus and hunters | Bites, claw contacts and blunt head bashes distinguished by the authored event. Hunter sweeps/dives/power contacts use bites. |
| Frostbite spiders, Dragonborn spiders, elytra and Web Mother | Bite or claw contacts; the large/giant spider races have heavier contact profiles. |
| Mudcrabs, spriggans, hagravens and unarmed Falmer | Claws/pincers, with bite events distinguished where present. Equipped weapons take precedence. |
| Netch | Tentacle/body contact; actual shock damage remains an elemental effect. |
| Lurkers and bone colossi | Heavy body/hand blows and distinct stomps. Giant-derived event names and sword-typed proxies do not invent a carried club or sword. |
| Rieklings and goblins | Spear-style contact for the spear proxy; ordinary maces, blades, arrows and other real weapons retain their own classification. |
| Ice wraiths and the spectral dragon's wraith rig | Bite contacts, alongside actual spell effects. |
| Wisps, shades, swarms and magic anomalies | Softer spirit contact rather than an animal claw. Known spectral/anomaly identities take precedence over borrowed skeletons. |
| Dragons, including undead variants | Existing bite, wing and tail responses. |
| Draugr, skeletons, keepers, ash spawn, zombies, wights and humanoids | Actual weapons and spell effects. An empty-hand fallback does not manufacture claws solely from a creature keyword. |

UESP describes the frost giants' shared giant animations in
[Frost Giant](https://en.uesp.net/wiki/Skyrim:Frost_Giant), the different atronach
forms in [Daedra](https://en.uesp.net/wiki/Skyrim:Daedra), and the centurion's two
weapons and other machines' attacks in
[Dwarven Automatons](https://en.uesp.net/wiki/Skyrim:Dwarven_Automatons).
The [frost atronach reference image](https://en.uesp.net/wiki/File:SR-creature-Frost_Atronach.jpg)
shows the right spike and left broad club; `AtronachFrostRace` and
`atronachfrostbehavior.hkx` supply the corresponding `R1`/`L1` attack names.

Animal and monster groupings use [Animals](https://en.uesp.net/wiki/Skyrim:Animals),
[Monsters](https://en.uesp.net/wiki/Skyrim:Monsters) and
[Undead](https://en.uesp.net/wiki/Skyrim:Undead). Specific DLC contact details
come from [Lurker](https://en.uesp.net/wiki/Skyrim:Lurker),
[Netch](https://en.uesp.net/wiki/Skyrim:Netch),
[Bristleback](https://en.uesp.net/wiki/Skyrim:Bristleback),
[Chaurus Hunter](https://en.uesp.net/wiki/Skyrim:Chaurus_Hunter),
[Gargoyle](https://en.uesp.net/wiki/Skyrim:Gargoyle),
[Ash Guardian](https://en.uesp.net/wiki/Skyrim:Ash_Guardian),
[Riekling](https://en.uesp.net/wiki/Skyrim:Riekling),
[Vampire Lord](https://en.uesp.net/wiki/Skyrim:Vampire_Lord) and
[Werebear](https://en.uesp.net/wiki/Skyrim:Werebear).

## Compatibility and limits

- Race editor IDs, skeleton identities and type keywords determine physical
  families. Localized actor display names, enemy health, level and damage amount
  do not determine reaction strength.
- Existing intensity settings, weapon/elemental/giant/centurion profiles, motion
  mixing, directional mapping and recovery limits are retained. New contact
  profiles run through that same system in first and third person.
- Real projectile contacts retain their incoming velocity cue. Bashes, blocked
  hits and power attacks retain their existing modifiers. Non-hostile utility
  debuffs still produce no reaction; hostile external control attacks can react
  on contact. Bow/crossbow bashes are classified as blunt
  melee contacts, rather than inheriting the equipped weapon's projectile type.
- Animation observation stays bounded to 16 nearby graphs for the asymmetric
  centurion/frost-atronach families. No new engine hooks, native offsets or
  relocation IDs are required.
- Replacement creatures using known rigs inherit those families. Completely
  unknown creatures/constructs use conservative generic contacts. Opaque custom
  animations cannot guarantee a precise striking limb; their source-direction
  cue remains available. No invented swing vector is assigned to an ambiguous
  animation. An explicit bite can refine a clawed animal, but unnamed animations
  retain the family's default contact.
- The race roster and named-attack tests check classification; frame-rate,
  direction, overlap, lifecycle and recovery tests check the motion. These do not
  substitute for subjective in-game checks with animation/creature replacers.
- Thirty frozen spell fixtures plus the colossus explosion's actual stagger
  effect check the new routing, including all three spider-spit levels, chaurus,
  seekers, lurker spray/spit, web traps, dragon breath and centurion/Forgemaster
  breath. Separate checks cover duplicate notifications across camera frames,
  simultaneous attackers/projectiles, poison impact plus stream, utility
  filtering, school-less noise and cinematic ownership.
- The research includes conditional/unused records; a linked script property
  does not prove that a particular quest executes it. Opaque custom scripts
  which change health without a supported hit/effect notification remain a
  runtime limitation. No live encounter, GOG/runtime-version matrix or arbitrary
  third-party animation pack is claimed verified by these checks.
