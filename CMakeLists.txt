cmake_minimum_required(VERSION 3.10)

# ==========================================
# Plugin Version Configuration
set(PLUGIN_VERSION "1.0.5")
# ==========================================

project(adjustment-layer VERSION ${PLUGIN_VERSION} LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Pass version to C code
add_compile_definitions(PLUGIN_VERSION="${PLUGIN_VERSION}")

set(LOCALE_FILES
    data/locale/en-US.ini
    data/locale/ja-JP.ini
    data/locale/zh-TW.ini
    data/locale/zh-CN.ini
    data/locale/ko-KR.ini
)

add_library(adjustment-layer MODULE
    src/adjustment-layer.c
    ${LOCALE_FILES}
)

# ------------------------------------------
# In-tree / Out-of-tree detection
# ------------------------------------------
set(IS_IN_TREE FALSE)
set(LIBOBS_TARGET "")

# In OBS in-tree builds, OBS::libobs is commonly provided.
if(TARGET OBS::libobs)
    set(IS_IN_TREE TRUE)
    set(LIBOBS_TARGET OBS::libobs)
    message(STATUS "adjustment-layer: In-tree build detected (OBS::libobs)")
elseif(TARGET libobs)
    set(IS_IN_TREE TRUE)
    set(LIBOBS_TARGET libobs)
    message(STATUS "adjustment-layer: In-tree build detected (libobs)")
else()
    message(STATUS "adjustment-layer: Stand-alone build detected")
endif()

# ------------------------------------------
# libobs include/link (stand-alone)
# ------------------------------------------
if(NOT IS_IN_TREE)
    # Prefer user-supplied paths (build.sh / build.ps1 style)
    if(DEFINED LIBOBS_INCLUDE_DIR)
        message(STATUS "Using manual LibObs include dir: ${LIBOBS_INCLUDE_DIR}")
        target_include_directories(adjustment-layer PRIVATE ${LIBOBS_INCLUDE_DIR})

        if(DEFINED LIBOBS_CONFIG_DIR)
            message(STATUS "Using manual LibObs config dir: ${LIBOBS_CONFIG_DIR}")
            target_include_directories(adjustment-layer PRIVATE ${LIBOBS_CONFIG_DIR})
        endif()

        if(NOT DEFINED LIBOBS_LIB)
            message(FATAL_ERROR "Stand-alone build requires LIBOBS_LIB when LIBOBS_INCLUDE_DIR is set.")
        endif()

        set(LIBOBS_TARGET "${LIBOBS_LIB}")
    else()
        # Best-effort fallback (often not available)
        find_package(LibObs QUIET)
        if(LibObs_FOUND)
            message(STATUS "LibObs package found via find_package(LibObs)")
            if(TARGET OBS::libobs)
                set(LIBOBS_TARGET OBS::libobs)
            elseif(TARGET libobs)
                set(LIBOBS_TARGET libobs)
            else()
                message(FATAL_ERROR "LibObs package found but no known libobs target was exported.")
            endif()
        else()
            message(FATAL_ERROR
                "Stand-alone build: LibObs not found.\n"
                "Please pass -DLIBOBS_INCLUDE_DIR=<path-to-obs/libobs> and -DLIBOBS_LIB=<path-to-libobs>.\n"
                "Example (macOS): -DLIBOBS_INCLUDE_DIR=.../obs-studio/libobs -DLIBOBS_LIB=.../libobs.dylib"
            )
        endif()
    endif()
endif()

# Link libobs
target_link_libraries(adjustment-layer PRIVATE ${LIBOBS_TARGET})

# ------------------------------------------
# OBS in-tree: use OBS helper for properties (bundle extension, folders, prefix, etc.)
# ------------------------------------------
if(IS_IN_TREE)
    if(COMMAND set_target_properties_obs)
        set_target_properties_obs(adjustment-layer PROPERTIES FOLDER plugins PREFIX "")
    else()
        message(WARNING "set_target_properties_obs not available; in-tree build may miss OBS-standard properties.")
    endif()
endif()

# ------------------------------------------
# Install / packaging
# ------------------------------------------
if(IS_IN_TREE)
    # In-tree: install locale to OBS data destination if OBS defines it.
    # Many OBS builds define OBS_DATA_DESTINATION. If not defined, we skip to avoid guessing.
    if(DEFINED OBS_DATA_DESTINATION)
        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/locale
            DESTINATION "${OBS_DATA_DESTINATION}/obs-plugins/adjustment-layer"
        )
    else()
        message(STATUS "OBS_DATA_DESTINATION not defined; skipping in-tree locale install (OBS packaging/rundir may handle it).")
    endif()

else()
    # Stand-alone: your original "dist" layout
    if(APPLE)
        # Put locale files into bundle Resources/locale at build time (nice for IDE browsing)
        set_source_files_properties(${LOCALE_FILES} PROPERTIES
            MACOSX_PACKAGE_LOCATION Resources/locale
        )

        set_target_properties(adjustment-layer PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION "plugin"
            MACOSX_BUNDLE_GUI_IDENTIFIER "jp.horaiken.adjustment-layer"
            MACOSX_BUNDLE_BUNDLE_NAME "adjustment-layer"
            MACOSX_BUNDLE_INFO_STRING "adjustment-layer plugin"
            MACOSX_BUNDLE_SHORT_VERSION_STRING "${PLUGIN_VERSION}"
            MACOSX_BUNDLE_BUNDLE_VERSION "1"
            MACOSX_BUNDLE_COPYRIGHT "Copyright 2026"
            MACOSX_BUNDLE_PACKAGE_TYPE "BNDL"
            BUILD_WITH_INSTALL_RPATH TRUE
            INSTALL_RPATH "@executable_path/../Frameworks;@loader_path/../Frameworks"
        )

        install(TARGETS adjustment-layer
            BUNDLE DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/dist
            LIBRARY DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/dist
        )

        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/locale
            DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/dist/adjustment-layer.plugin/Contents/Resources
        )

    elseif(WIN32)
        install(TARGETS adjustment-layer
            RUNTIME DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/dist/obs-plugins/64bit
            LIBRARY DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/dist/obs-plugins/64bit
        )

        install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/data/locale
            DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/dist/data/obs-plugins/adjustment-layer
        )
    endif()
endif()