cmake_minimum_required(VERSION 3.20)

project(obs-vjlink VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# --- OBS SDK ---
# Set OBS_DIR to your OBS Studio install or build directory
# e.g. cmake -DOBS_DIR="C:/Program Files/obs-studio" ..
if(NOT DEFINED OBS_DIR)
    set(OBS_DIR "C:/Program Files/obs-studio" CACHE PATH "OBS Studio install directory")
endif()

# Find OBS headers and libraries
set(OBS_INCLUDE_DIRS
    "${OBS_DIR}/include"
    "${OBS_DIR}/include/obs"
)

# w32-pthreads headers (OBS dependency on Windows)
if(WIN32)
    set(W32_PTHREADS_INCLUDE "${OBS_DIR}/deps/w32-pthreads")
    if(EXISTS "${W32_PTHREADS_INCLUDE}/pthread.h")
        list(APPEND OBS_INCLUDE_DIRS "${W32_PTHREADS_INCLUDE}")
    endif()
endif()

# Try multiple lib paths (install vs build layout)
find_library(OBS_LIB obs PATHS
    "${OBS_DIR}/lib"
    "${OBS_DIR}/bin/64bit"
    "${OBS_DIR}/build/libobs/Release"
    "${OBS_DIR}/build/libobs/Debug"
)

if(NOT OBS_LIB)
    message(WARNING "Could not find libobs. Set OBS_DIR to your OBS install path.")
endif()

# Find w32-pthreads lib
if(WIN32)
    find_library(W32_PTHREADS_LIB w32-pthreads PATHS
        "${OBS_DIR}/lib"
        "${OBS_DIR}/bin/64bit"
    )
endif()

# --- Sources ---
set(VJLINK_SOURCES
    src/plugin-main.c
    src/vjlink_context.c
    # Audio
    src/audio/audio_engine.c
    src/audio/audio_texture.c
    src/audio/bpm_detector.c
    src/audio/kissfft/kiss_fft.c
    # Rendering
    src/rendering/effect_system.c
    src/rendering/compositor.c
    src/rendering/engine3d.c
    src/rendering/particles.c
    src/rendering/band_effects.c
    src/rendering/media_layer.c
    # Sources
    src/sources/audio_filter.c
    src/sources/compositor_source.c
    src/sources/effect_filter.c
    src/sources/videowall_source.c
    # Controls
    src/controls/hotkey_manager.c
    src/controls/websocket_handler.c
    src/controls/source_trigger.c
    src/controls/http_server.c
    src/controls/tools_menu.c
    # Presets
    src/presets/preset_manager.c
    src/presets/param_animator.c
    src/presets/cjson/cJSON.c
    # UI
    src/ui/properties_builder.c
)

set(VJLINK_HEADERS
    src/vjlink_context.h
    src/audio/audio_engine.h
    src/audio/audio_texture.h
    src/audio/bpm_detector.h
    src/audio/kissfft/kiss_fft.h
    src/rendering/effect_system.h
    src/rendering/compositor.h
    src/rendering/engine3d.h
    src/rendering/particles.h
    src/rendering/band_effects.h
    src/rendering/media_layer.h
    src/sources/audio_filter.h
    src/sources/compositor_source.h
    src/sources/effect_filter.h
    src/sources/videowall_source.h
    src/controls/hotkey_manager.h
    src/controls/websocket_handler.h
    src/controls/source_trigger.h
    src/controls/http_server.h
    src/controls/tools_menu.h
    src/presets/preset_manager.h
    src/presets/param_animator.h
    src/presets/cjson/cJSON.h
    src/ui/properties_builder.h
)

# --- Plugin target ---
add_library(obs-vjlink MODULE ${VJLINK_SOURCES} ${VJLINK_HEADERS})

target_include_directories(obs-vjlink PRIVATE
    ${OBS_INCLUDE_DIRS}
    "${CMAKE_SOURCE_DIR}/src"
    "${CMAKE_SOURCE_DIR}/src/audio/kissfft"
)

if(OBS_LIB)
    target_link_libraries(obs-vjlink PRIVATE ${OBS_LIB})
endif()

if(W32_PTHREADS_LIB)
    target_link_libraries(obs-vjlink PRIVATE ${W32_PTHREADS_LIB})
endif()

# HTTP server needs Winsock
if(WIN32)
    target_link_libraries(obs-vjlink PRIVATE ws2_32)
endif()

# Windows-specific
if(WIN32)
    target_compile_definitions(obs-vjlink PRIVATE
        _CRT_SECURE_NO_WARNINGS
        WIN32_LEAN_AND_MEAN
    )
    # Remove "lib" prefix on Windows
    set_target_properties(obs-vjlink PROPERTIES PREFIX "")
endif()

# Use math defines
target_compile_definitions(obs-vjlink PRIVATE _USE_MATH_DEFINES)

# Hide cJSON symbols to avoid DLL export conflicts
target_compile_definitions(obs-vjlink PRIVATE CJSON_HIDE_SYMBOLS)

# --- Post-build: copy to OBS plugins ---
set(OBS_PLUGIN_DEST "${OBS_DIR}/obs-plugins/64bit" CACHE PATH "OBS plugin install directory")

add_custom_command(TARGET obs-vjlink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_PLUGIN_DEST}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:obs-vjlink> "${OBS_PLUGIN_DEST}/"
    COMMENT "Copying obs-vjlink.dll to OBS plugins directory"
)

# Copy data files (effects, presets, locale)
add_custom_command(TARGET obs-vjlink POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${OBS_DIR}/data/obs-plugins/obs-vjlink"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/effects"
        "${OBS_DIR}/data/obs-plugins/obs-vjlink/effects"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/data"
        "${OBS_DIR}/data/obs-plugins/obs-vjlink"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/presets"
        "${OBS_DIR}/data/obs-plugins/obs-vjlink/presets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/effects_meta"
        "${OBS_DIR}/data/obs-plugins/obs-vjlink/effects_meta"
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${OBS_DIR}/data/obs-plugins/obs-vjlink/web-ui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/web-ui"
        "${OBS_DIR}/data/obs-plugins/obs-vjlink/web-ui"
    COMMENT "Copying data files to OBS plugin data directory"
)

# --- CPack / NSIS Installer ---
set(CPACK_PACKAGE_NAME "OBS-VJLink")
set(CPACK_PACKAGE_VENDOR "VJLink")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "VJ visuals and audio-reactive effects for OBS Studio")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "OBS-VJLink")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")

if(WIN32)
    set(CPACK_GENERATOR "NSIS;ZIP")

    # NSIS settings
    set(CPACK_NSIS_DISPLAY_NAME "OBS-VJLink ${PROJECT_VERSION}")
    set(CPACK_NSIS_PACKAGE_NAME "OBS-VJLink")
    set(CPACK_NSIS_HELP_LINK "https://github.com/vjlink/obs-vjlink")
    set(CPACK_NSIS_URL_INFO_ABOUT "https://github.com/vjlink/obs-vjlink")
    set(CPACK_NSIS_MODIFY_PATH OFF)
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)

    # Let user choose OBS install path
    set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS "
        ; Copy plugin DLL
        CreateDirectory '$INSTDIR\\\\obs-plugins\\\\64bit'
        CopyFiles '$INSTDIR\\\\bin\\\\obs-vjlink.dll' '$INSTDIR\\\\obs-plugins\\\\64bit\\\\obs-vjlink.dll'
        ; Copy data files
        CreateDirectory '$INSTDIR\\\\data\\\\obs-plugins\\\\obs-vjlink'
        CopyFiles /SILENT '$INSTDIR\\\\data\\\\*.*' '$INSTDIR\\\\data\\\\obs-plugins\\\\obs-vjlink\\\\'
    ")
else()
    set(CPACK_GENERATOR "TGZ;ZIP")
endif()

# Install rules for packaging
install(TARGETS obs-vjlink
    LIBRARY DESTINATION "obs-plugins/64bit"
    RUNTIME DESTINATION "obs-plugins/64bit"
)

install(DIRECTORY effects/
    DESTINATION "data/obs-plugins/obs-vjlink/effects"
)

install(DIRECTORY effects_meta/
    DESTINATION "data/obs-plugins/obs-vjlink/effects_meta"
)

install(DIRECTORY presets/
    DESTINATION "data/obs-plugins/obs-vjlink/presets"
)

install(DIRECTORY data/
    DESTINATION "data/obs-plugins/obs-vjlink"
)

include(CPack)
