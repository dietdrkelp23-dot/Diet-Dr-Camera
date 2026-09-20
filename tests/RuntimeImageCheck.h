#pragma once
#include "PCH.h"
#include "Hooks/RuntimeHooks.h"
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
    };
#define CHECK_SLOT(Type, table, slot) checkSlot(#Type, RE::VTABLE_##Type[table], slot)
    CHECK_SLOT(ValueModifierEffect, 0, 0x20);
    CHECK_SLOT(DualValueModifierEffect, 0, 0x20);
    CHECK_SLOT(PeakValueModifierEffect, 0, 0x20);
    CHECK_SLOT(AbsorbEffect, 0, 0x20);
    CHECK_SLOT(AccumulatingValueModifierEffect, 0, 0x20);
    CHECK_SLOT(TargetValueModifierEffect, 0, 0x20);
    CHECK_SLOT(ValueAndConditionsEffect, 0, 0x20);
    for (const auto slot : {0x3, 0x4, 0x5, 0xB, 0xE}) {
        CHECK_SLOT(ThirdPersonState, 0, slot);
        CHECK_SLOT(HorseCameraState, 0, slot);
    }
    for (const auto slot : {0x3, 0x4}) CHECK_SLOT(DragonCameraState, 0, slot);
    for (const auto slot : {0x1, 0x2, 0x3}) CHECK_SLOT(BleedoutCameraState, 0, slot);
    CHECK_SLOT(TweenMenuCameraState, 0, 0x3);
    CHECK_SLOT(FirstPersonState, 0, 0x3);
    CHECK_SLOT(PlayerCameraTransitionState, 0, 0x3);
    CHECK_SLOT(MenuControls, 0, 0x1);
    CHECK_SLOT(NiCamera, 0, 0x30);
    CHECK_SLOT(DialogueMenu, 0, 0x5);
    CHECK_SLOT(ThirdPersonState, 1, DietDrCamera::RuntimeHooks::InputSlot(4));
    CHECK_SLOT(HorseCameraState, 1, DietDrCamera::RuntimeHooks::InputSlot(4));
    CHECK_SLOT(TogglePOVHandler, 0, DietDrCamera::RuntimeHooks::InputSlot(4));
    CHECK_SLOT(TogglePOVHandler, 0, DietDrCamera::RuntimeHooks::InputSlot(5));
    CHECK_SLOT(ActivateHandler, 0, 1);
    CHECK_SLOT(AttackBlockHandler, 0, 1);
    CHECK_SLOT(AutoMoveHandler, 0, 1);
    CHECK_SLOT(JumpHandler, 0, 1);
    CHECK_SLOT(LookHandler, 0, 1);
    CHECK_SLOT(MovementHandler, 0, 1);
    CHECK_SLOT(ReadyWeaponHandler, 0, 1);
    CHECK_SLOT(RunHandler, 0, 1);
    CHECK_SLOT(ShoutHandler, 0, 1);
    CHECK_SLOT(SneakHandler, 0, 1);
    CHECK_SLOT(SprintHandler, 0, 1);
    CHECK_SLOT(TogglePOVHandler, 0, 1);
    CHECK_SLOT(ToggleRunHandler, 0, 1);
    CHECK_SLOT(FavoritesHandler, 0, 1);
    CHECK_SLOT(MenuOpenHandler, 0, 1);
#undef CHECK_SLOT
    std::cout << checkedSlots << " hooked vtable targets passed executable-image validation\n";
    std::cout << "Complete hook preflight passed against mapped Skyrim " << version.string() << "; no game code executed\n";
}
