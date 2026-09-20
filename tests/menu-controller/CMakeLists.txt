cmake_minimum_required(VERSION 3.24)
project(MenuControllerChecks LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
enable_testing()
if(NOT SMF_SOURCE_DIR)
    message(FATAL_ERROR "Run tools/check-menu-controller.ps1 to obtain the pinned framework sources.")
endif()
file(TO_CMAKE_PATH "${SMF_SOURCE_DIR}" SMF_SOURCE_DIR)
get_filename_component(DDC_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
foreach(layout IN ITEMS LegacyIO NamedKeysOnly)
    add_executable(MenuControllerCapture${layout}
        MenuControllerCaptureChecks.cpp
        "${SMF_SOURCE_DIR}/src/GamepadNavigation.cpp"
        "${SMF_SOURCE_DIR}/imgui/imgui.cpp"
        "${SMF_SOURCE_DIR}/imgui/imgui_draw.cpp"
        "${SMF_SOURCE_DIR}/imgui/imgui_tables.cpp"
        "${SMF_SOURCE_DIR}/imgui/imgui_widgets.cpp")
    add_executable(MenuControllerLayout${layout} MenuControllerLayoutChecks.cpp)
    add_executable(MenuControllerNavigation${layout}
        MenuNavigationChecks.cpp
        "${SMF_SOURCE_DIR}/imgui/imgui.cpp"
        "${SMF_SOURCE_DIR}/imgui/imgui_draw.cpp"
        "${SMF_SOURCE_DIR}/imgui/imgui_tables.cpp"
        "${SMF_SOURCE_DIR}/imgui/imgui_widgets.cpp")
    foreach(kind IN ITEMS Capture Layout Navigation)
        set(check MenuController${kind}${layout})
        target_include_directories(${check} PRIVATE stubs "${DDC_ROOT}/include"
            "${SMF_SOURCE_DIR}/include" "${SMF_SOURCE_DIR}/imgui")
        target_compile_definitions(${check} PRIVATE IMGUI_DEFINE_MATH_OPERATORS
            WIN32_LEAN_AND_MEAN NOMINMAX _SILENCE_CXX17_CODECVT_HEADER_DEPRECATION_WARNING
            SMF_SYMBOL_FONT="${SMF_SOURCE_DIR}/public/SKSE/plugins/fonts/fa-solid-900.ttf")
        if(layout STREQUAL "NamedKeysOnly")
            target_compile_definitions(${check} PRIVATE IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
        endif()
        if(MSVC)
            target_compile_options(${check} PRIVATE /utf-8)
        endif()
        add_test(NAME ${check} COMMAND ${check})
    endforeach()
endforeach()

add_executable(KeyboardScanCodeChecks KeyboardScanCodeChecks.cpp)
target_include_directories(KeyboardScanCodeChecks PRIVATE "${DDC_ROOT}/include")
target_compile_definitions(KeyboardScanCodeChecks PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
add_test(NAME KeyboardScanCodeChecks COMMAND KeyboardScanCodeChecks)
