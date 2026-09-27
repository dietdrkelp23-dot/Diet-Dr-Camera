#pragma once
#include "PCH.h"
#include "Hooks/RuntimeHooks.h"
#include "Hooks/CameraStateEntry.h"
#include <Windows.h>
#include <fstream>
#include "RuntimeImageScenario.h"

// Optional offline verification against a locally supplied, unpacked executable.
// No engine instructions, entry point, imports or game initialization are executed.
inline void CheckRuntimeImage(const std::filesystem::path& executable, const std::filesystem::path& library,
    std::string_view scenario = "clean")
{
    std::ifstream stream(executable, std::ios::binary);
    std::vector<char> file{std::istreambuf_iterator<char>{stream}, {}};
    const auto require=[](bool condition) { if (!condition) throw std::runtime_error("invalid runtime image"); };
    const auto read=[&]<class T>(std::size_t offset) {
        require(offset<=file.size() && sizeof(T)<=file.size()-offset);
        T value{};
        std::memcpy(&value,file.data()+offset,sizeof(value));
        return value;
    };
    const auto dos=read.operator()<IMAGE_DOS_HEADER>(0);
    require(dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>0);
    const auto nt=read.operator()<IMAGE_NT_HEADERS64>(dos.e_lfanew);
    require(nt.Signature==IMAGE_NT_SIGNATURE && nt.FileHeader.Machine==IMAGE_FILE_MACHINE_AMD64 &&
        nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR64_MAGIC && nt.OptionalHeader.SizeOfImage<0x20000000);
    const auto imageSize=nt.OptionalHeader.SizeOfImage;
    auto* image=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,imageSize,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
    require(image!=nullptr);
    struct ImageOwner { void* image; ~ImageOwner() { VirtualFree(image,0,MEM_RELEASE); } } owner{image};
    require(nt.OptionalHeader.SizeOfHeaders<=file.size() && nt.OptionalHeader.SizeOfHeaders<=imageSize);
    std::memcpy(image,file.data(),nt.OptionalHeader.SizeOfHeaders);
    const auto sections=dos.e_lfanew+24+nt.FileHeader.SizeOfOptionalHeader;
    for (std::size_t i=0;i<nt.FileHeader.NumberOfSections;++i) {
        const auto section=read.operator()<IMAGE_SECTION_HEADER>(sections+i*sizeof(IMAGE_SECTION_HEADER));
        require(section.PointerToRawData<=file.size() && section.SizeOfRawData<=file.size()-section.PointerToRawData &&
            section.VirtualAddress<=imageSize && section.SizeOfRawData<=imageSize-section.VirtualAddress);
        std::memcpy(image+section.VirtualAddress,file.data()+section.PointerToRawData,section.SizeOfRawData);
        if (section.Characteristics&IMAGE_SCN_MEM_EXECUTE) {
            DWORD oldProtection{};
            require(VirtualProtect(image+section.VirtualAddress,section.SizeOfRawData,PAGE_EXECUTE_READ,&oldProtection)!=0);
        }
    }
    const auto exception=nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    require(exception.VirtualAddress<=imageSize && exception.Size<=imageSize-exception.VirtualAddress &&
        exception.Size && exception.Size%sizeof(RUNTIME_FUNCTION)==0);
    auto* functions=reinterpret_cast<RUNTIME_FUNCTION*>(image+exception.VirtualAddress);
    require(RtlAddFunctionTable(functions,exception.Size/sizeof(RUNTIME_FUNCTION),reinterpret_cast<DWORD64>(image))!=0);
    struct TableOwner { RUNTIME_FUNCTION* table; ~TableOwner() { RtlDeleteFunctionTable(table); } } tableOwner{functions};

    // Use the production version reader. SE 1.5.97's fixed numeric version is
    // 1.0.0.0; its actual runtime version is in the ProductVersion string.
    const auto detectedVersion=REL::GetFileVersion(executable.wstring());
    require(detectedVersion.has_value());
    const auto version=*detectedVersion;
    require(REL::Module::mock(version,REL::Module::Runtime::Unknown,L"SkyrimSE.exe",reinterpret_cast<std::uintptr_t>(image)));
    require(REL::IDDB::inject(library.wstring(),version));
    const auto cameraSetState = REL::RelocationID(32290, 33026).address();
    require(DietDrCamera::CameraStateEntry::Gateway(
        {reinterpret_cast<const std::uint8_t*>(cameraSetState), 16}, cameraSetState + 5).has_value());
    RuntimeImageScenario mutation(image, imageSize, version, scenario);
    if (!mutation.Prepare()) return;
    const auto& sites=DietDrCamera::RuntimeHooks::Get();
    require(sites.mainUpdate!=0 && sites.dialogueTimer!=0 && sites.cameraUpdate!=0 && sites.enterFurniture!=0);
    // Check the native targets of every vtable slot DDC installs, including
    // conditional dragon hooks. This validates addresses, not C++ signatures
    // or gameplay behavior. No target is ever invoked.
    std::size_t checkedSlots = 0;
    const auto checkSlot = [&](std::string_view name, auto id, std::size_t slot) {
        const REL::Relocation<std::uintptr_t> table{id};
        const auto tableRva = table.address() - reinterpret_cast<std::uintptr_t>(image);
        require(tableRva < imageSize && (slot + 1) * sizeof(std::uintptr_t) <= imageSize - tableRva);
        std::uintptr_t target{};
        std::memcpy(&target, image + tableRva + slot * sizeof(target), sizeof(target));
        require(target >= nt.OptionalHeader.ImageBase && target - nt.OptionalHeader.ImageBase < imageSize);
        const auto rva = target - nt.OptionalHeader.ImageBase;
        bool executableTarget = false;
        for (std::size_t i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            const auto section = read.operator()<IMAGE_SECTION_HEADER>(sections + i * sizeof(IMAGE_SECTION_HEADER));
            if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) && rva >= section.VirtualAddress &&
                rva - section.VirtualAddress < section.Misc.VirtualSize) executableTarget = true;
        }
        if (!executableTarget) throw std::runtime_error(std::string(name) + " slot " +
            std::to_string(slot) + " does not target executable image code");
        ++checkedSlots;
        return reinterpret_cast<std::uintptr_t>(image) + rva;
    };
// Native method identities reviewed against SE 1.5.97, AE 1.6.1170 and
    // 1.7.104 images, then checked across the historical executable matrix.
    // An adjacent slot targeting a different valid function must fail.
#define CHECK_SLOT(Type, table, slot, se, ae) require(checkSlot(#Type, RE::VTABLE_##Type[table], slot) == REL::RelocationID(se, ae).address())
    CHECK_SLOT(ValueModifierEffect, 0, 0x20, 34286, 35086);
    CHECK_SLOT(DualValueModifierEffect, 0, 0x20, 33545, 34324);
    CHECK_SLOT(PeakValueModifierEffect, 0, 0x20, 34286, 35086);
    CHECK_SLOT(AbsorbEffect, 0, 0x20, 34286, 35086);
    CHECK_SLOT(AccumulatingValueModifierEffect, 0, 0x20, 34286, 35086);
    CHECK_SLOT(TargetValueModifierEffect, 0, 0x20, 34286, 35086);
    CHECK_SLOT(ValueAndConditionsEffect, 0, 0x20, 34267, 35067);
    CHECK_SLOT(BeamProjectile, 0, 0xBD, 42594, 43757);
    CHECK_SLOT(ArrowProjectile, 0, 0xAB, 42541, 43704);
    CHECK_SLOT(ArrowProjectile, 0, 0xBD, 42547, 43710);
    CHECK_SLOT(MissileProjectile, 0, 0xAB, 42852, 44027);
    CHECK_SLOT(MissileProjectile, 0, 0xBD, 42866, 44041);
    CHECK_SLOT(ConeProjectile, 0, 0xAB, 42624, 43789);
    CHECK_SLOT(ConeProjectile, 0, 0xBD, 42633, 43801);
    CHECK_SLOT(hkbClipGenerator, 0, 0x4, 58602, 59252);
    CHECK_SLOT(hkbClipGenerator, 0, 0x5, 58603, 59253);
    CHECK_SLOT(hkbClipGenerator, 0, 0x7, 58604, 59254);
    CHECK_SLOT(hkpSimpleShapePhantom, 0, 0xF, 60678, 61538);
    CHECK_SLOT(hkpSimpleShapePhantom, 0, 0x10, 60679, 61540);
    CHECK_SLOT(hkpSimpleShapePhantom, 0, 0x11, 60680, 61541);
    CHECK_SLOT(hkpCachingShapePhantom, 0, 0xF, 60799, 61661);
    CHECK_SLOT(hkpCachingShapePhantom, 0, 0x10, 60800, 61662);
    CHECK_SLOT(hkpCachingShapePhantom, 0, 0x11, 60802, 61664);
    CHECK_SLOT(hkpWorldLinearCaster, 0, 0x1, 63653, 64642);
    CHECK_SLOT(ThirdPersonState, 0, 0x3, 49960, 50896);
    CHECK_SLOT(ThirdPersonState, 0, 0x4, 49961, 50897);
    CHECK_SLOT(ThirdPersonState, 0, 0x5, 49962, 50898);
    CHECK_SLOT(ThirdPersonState, 0, 0xB, 49965, 50901);
    CHECK_SLOT(ThirdPersonState, 0, 0xE, 49976, 50912);
    CHECK_SLOT(HorseCameraState, 0, 0x3, 49960, 50896);
    CHECK_SLOT(HorseCameraState, 0, 0x4, 49961, 50897);
    CHECK_SLOT(HorseCameraState, 0, 0x5, 49962, 50898);
    CHECK_SLOT(HorseCameraState, 0, 0xB, 49836, 50767);
    CHECK_SLOT(HorseCameraState, 0, 0xE, 49840, 50771);
    CHECK_SLOT(DragonCameraState, 0, 0x3, 49960, 50896);
    CHECK_SLOT(DragonCameraState, 0, 0x4, 49961, 50897);
    CHECK_SLOT(BleedoutCameraState, 0, 0x1, 49786, 50714);
    CHECK_SLOT(BleedoutCameraState, 0, 0x2, 49787, 50715);
    CHECK_SLOT(BleedoutCameraState, 0, 0x3, 49788, 50716);
    CHECK_SLOT(TweenMenuCameraState, 0, 0x3, 49985, 50925);
    CHECK_SLOT(FirstPersonState, 0, 0x3, 49795, 50724);
    CHECK_SLOT(FirstPersonState, 0, 0x4, 49796, 50725);
    CHECK_SLOT(FirstPersonState, 1, DietDrCamera::RuntimeHooks::InputSlot(0x4), 49800, 50730);
    CHECK_SLOT(PlayerCameraTransitionState, 0, 0x3, 49955, 50891);
    // Additional primary Update slots wrapped by the bleedout transition guard.
    CHECK_SLOT(AutoVanityState, 0, 0x3, 49781, 50709);
    CHECK_SLOT(VATSCameraState, 0, 0x3, 49992, 50935);
    CHECK_SLOT(FreeCameraState, 0, 0x3, 49813, 50743);
    CHECK_SLOT(IronSightsState, 0, 0x3, 49846, 50778);
    CHECK_SLOT(FurnitureCameraState, 0, 0x3, 49822, 50752);
    CHECK_SLOT(MenuControls, 0, 0x1, 51356, 52200);
    CHECK_SLOT(NiCamera, 0, 0x30, 69272, 70642);
    CHECK_SLOT(DialogueMenu, 0, 0x5, 80284, 82307);
    CHECK_SLOT(ThirdPersonState, 1, DietDrCamera::RuntimeHooks::InputSlot(0x4), 49970, 50906);
    CHECK_SLOT(HorseCameraState, 1, DietDrCamera::RuntimeHooks::InputSlot(0x4), 49832, 50763);
    CHECK_SLOT(TogglePOVHandler, 0, DietDrCamera::RuntimeHooks::InputSlot(0x4), 41359, 42433);
    CHECK_SLOT(TogglePOVHandler, 0, DietDrCamera::RuntimeHooks::InputSlot(0x5), 41256, 42335);
    CHECK_SLOT(ActivateHandler, 0, 0x1, 41380, 42453);
    CHECK_SLOT(AttackBlockHandler, 0, 0x1, 41381, 42454);
    CHECK_SLOT(AutoMoveHandler, 0, 0x1, 41382, 42455);
    CHECK_SLOT(JumpHandler, 0, 0x1, 41383, 42456);
    CHECK_SLOT(LookHandler, 0, 0x1, 41384, 42457);
    // 1.7.99 replaced the thumbstick method while adding motion-input slots.
    const auto thumbstickMethod = REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99) ? 523968 : 42425;
    CHECK_SLOT(LookHandler, 0, DietDrCamera::RuntimeHooks::InputSlot(0x2), 41351, thumbstickMethod);
    CHECK_SLOT(LookHandler, 0, DietDrCamera::RuntimeHooks::InputSlot(0x3), 41350, 42424);
    CHECK_SLOT(PlayerControls, 0, 0x1, 41259, 42338);
    CHECK_SLOT(PlayerCharacter, 6, 0x8, 37001, 38029);
    CHECK_SLOT(MovementHandler, 0, 0x1, 41385, 42458);
    CHECK_SLOT(ReadyWeaponHandler, 0, 0x1, 41386, 42459);
    CHECK_SLOT(RunHandler, 0, 0x1, 41387, 42460);
    CHECK_SLOT(ShoutHandler, 0, 0x1, 41388, 42461);
    CHECK_SLOT(SneakHandler, 0, 0x1, 41389, 42462);
    CHECK_SLOT(SprintHandler, 0, 0x1, 41390, 42463);
    CHECK_SLOT(TogglePOVHandler, 0, 0x1, 41391, 42464);
    CHECK_SLOT(ToggleRunHandler, 0, 0x1, 41392, 42465);
    CHECK_SLOT(FavoritesHandler, 0, 0x1, 51409, 52258);
    CHECK_SLOT(MenuOpenHandler, 0, 0x1, 51410, 52259);
    // Production locates these three tables by RTTI name. An independent
    // replay of that walk matched these tables in all 21 Steam images.
    CHECK_SLOT(AnimatedCameraStartHandler, 0, 0x1, 41814, 42895);
    CHECK_SLOT(AnimatedCameraDeltaStartHandler, 0, 0x1, 41815, 42896);
    CHECK_SLOT(AnimatedCameraEndHandler, 0, 0x1, 41816, 42897);
#undef CHECK_SLOT
    // The fallback calls the same normalizer that native PlayerControls calls.
    // SE routes it through a helper; AE inlines that helper into ProcessEvent.
    const auto nativeLookCaller = REL::RelocationID(41292, 42338).address();
    const auto nativeLookNormalizer = REL::RelocationID(41275, 42354).address();
    bool normalizerReferenced = false;
    const std::size_t lookCallerSize = REL::Module::IsAE() ? 0x800 : 0x30;
    for (std::size_t i = 0; i + 5 <= lookCallerSize; ++i) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(nativeLookCaller + i);
        if (bytes[0] != 0xE8) continue;
        std::int32_t displacement{};
        std::memcpy(&displacement, bytes + 1, sizeof(displacement));
        if (nativeLookCaller + i + 5 + displacement == nativeLookNormalizer) normalizerReferenced = true;
    }
    require(normalizerReferenced);
    // Verify the real-time input delta used by both native normalization and
    // third-person look. The scaled simulation delta is the adjacent float.
    require(REL::RelocationID(523661, 410200).address() == REL::RelocationID(523660, 410199).address() + sizeof(float));
    std::cout << checkedSlots << " hooked vtable targets passed method-identity and executable-image validation\n";
    std::cout << "Complete hook preflight passed against mapped Skyrim " << version.string() << "; no game code executed\n";
}
