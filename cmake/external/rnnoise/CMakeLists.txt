# This file is part of Desktop App Toolkit,
# a set of libraries for developing nice desktop applications.
#
# For license and copyright information please follow this link:
# https://github.com/desktop-app/legal/blob/master/LEGAL

if (DESKTOP_APP_USE_PACKAGED)
    add_library(external_rnnoise INTERFACE IMPORTED GLOBAL)
    add_library(desktop-app::external_rnnoise ALIAS external_rnnoise)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(RNNOISE REQUIRED IMPORTED_TARGET rnnoise)

    target_link_libraries(external_rnnoise INTERFACE PkgConfig::RNNOISE)
else()
    add_library(external_rnnoise STATIC IMPORTED GLOBAL)
    add_library(desktop-app::external_rnnoise ALIAS external_rnnoise)

    set(rnnoise_lib_loc ${libs_loc}/rnnoise/out)
    if (WIN32)
        target_include_directories(external_rnnoise SYSTEM
        INTERFACE
            ${libs_loc}/rnnoise/include
        )
        set_target_properties(external_rnnoise PROPERTIES
            IMPORTED_LOCATION "${rnnoise_lib_loc}/Release/rnnoise.lib"
            IMPORTED_LOCATION_DEBUG "${rnnoise_lib_loc}/Debug/rnnoise.lib"
        )
    elseif (APPLE)
        target_include_directories(external_rnnoise SYSTEM
        INTERFACE
            ${libs_loc}/rnnoise/include
        )
        set_target_properties(external_rnnoise PROPERTIES
            IMPORTED_LOCATION "${rnnoise_lib_loc}/Release/librnnoise.a"
            IMPORTED_LOCATION_DEBUG "${rnnoise_lib_loc}/Debug/librnnoise.a"
        )
    else()
        target_include_directories(external_rnnoise SYSTEM
        INTERFACE
            /usr/local/include
        )
        find_library(RNNOISE_LIBRARY librnnoise.a)
        set_target_properties(external_rnnoise PROPERTIES
            IMPORTED_LOCATION "${RNNOISE_LIBRARY}"
        )
    endif()
endif()
