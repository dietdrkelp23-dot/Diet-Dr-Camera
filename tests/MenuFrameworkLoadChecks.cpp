#include "UI/MenuFrameworkBinding.h"
#include <iostream>
#include <stdexcept>

int wmain(int argc, wchar_t** argv)
{
    try {
        const auto check = [](bool ok, const char* message) {
            if (!ok) throw std::runtime_error(message);
        };
        check(argc == 2, "Expected the private framework fixture DLL path");
        check(static_cast<HMODULE>(menuFramework) == nullptr,
              "The fixture must not be present when the consumer starts");
        check(!DietDrCamera::MenuFrameworkBinding::HasContext(),
              "An absent framework must not expose an ImGui context");
        check(!DietDrCamera::MenuFrameworkBinding::MainWindowOpen().has_value(),
              "Absent framework exposed a main window");

        const auto fixture = LoadLibraryW(argv[1]);
        check(fixture != nullptr, "Fixture DLL could not be loaded");
        check(static_cast<HMODULE>(menuFramework) == fixture,
              "The handle must recover after the dependency loads later");
        check(!DietDrCamera::MenuFrameworkBinding::HasContext(),
              "A loaded DLL is not sufficient before ImGui initializes");
        const auto initialize = reinterpret_cast<void (*)()>(
            GetProcAddress(menuFramework, "SetFixtureContextReady"));
        check(initialize != nullptr, "Deferred handle must work with SDK export lookups");
        initialize();
        check(DietDrCamera::MenuFrameworkBinding::HasContext(),
              "The UI should become available when its context is ready");
        const auto setOpen = reinterpret_cast<void (*)(bool)>(GetProcAddress(fixture,"SetFixtureWindowOpen"));
        check(setOpen != nullptr,"Fixture window export missing");
        check(DietDrCamera::MenuFrameworkBinding::MainWindowOpen() == false,"Initial window state was not closed");
        setOpen(true); // Risa's direct IsOpen path sends no framework event.
        check(DietDrCamera::MenuFrameworkBinding::MainWindowOpen() == true,"Direct launcher open was missed");
        setOpen(false);
        check(DietDrCamera::MenuFrameworkBinding::MainWindowOpen() == false,"Direct launcher close was missed");
        FreeLibrary(fixture);
        std::cout << "Late framework load, context readiness and direct launcher window checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
