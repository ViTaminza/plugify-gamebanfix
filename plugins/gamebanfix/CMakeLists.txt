# gamebanfix — Plugify C++ port of the Metamod:Source plugin "GameBanFix":
# hooks GameSystem_Think_CheckSteamBan (polyhook) and keeps the engine's GC-ban kick pass from
# acting on stale competitive-cooldown entries of CCSGameRules::sm_mapGcBanInformation.
#
#   plugify-gamebanfix       the plugin shared library (entry "bin/gamebanfix" -> bin/libgamebanfix.so)
#   gamebanfix_tests         the container unit tests (GcBanTree), run by ../test.sh
#
# The plugin links:
#   * s2sdk (gamedata: signatures/addresses, convar access, logging),
#   * polyhook (the official hooking plugin; HookDetour + Pre/Post callbacks).

set(GAMEBANFIX_VERSION "1.0.1")

add_library(plugify-gamebanfix SHARED
        src/plugin.cpp
        src/gcban_map.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:polyhook_bindings>
)
target_include_directories(plugify-gamebanfix PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: the generated s2sdk/polyhook/plg headers are not ours to fix; their warnings must not
# fail -Werror on our sources.
target_include_directories(plugify-gamebanfix SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-gamebanfix PRIVATE -static-libstdc++ -static-libgcc)
target_compile_options(plugify-gamebanfix PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-gamebanfix PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="gamebanfix"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-gamebanfix PROPERTIES OUTPUT_NAME gamebanfix)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-gamebanfix PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/gamebanfix.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/gamebanfix.pplugin"
)

# The container unit tests: no engine, no plugify runtime - GcBanTree is exercised against an
# in-test RB tree builder that fills the same 0x38-byte nodes (see tests/gamebanfix_tests.cpp).
add_executable(gamebanfix_tests
        tests/gamebanfix_tests.cpp
        src/gcban_map.cpp
)
target_include_directories(gamebanfix_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_compile_options(gamebanfix_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(gamebanfix_tests PRIVATE _GLIBCXX_USE_CXX11_ABI=1)
set_target_properties(gamebanfix_tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
