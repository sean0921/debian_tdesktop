# This file is part of Desktop App Toolkit,
# a set of libraries for developing nice desktop applications.
#
# For license and copyright information please follow this link:
# https://github.com/desktop-app/legal/blob/master/LEGAL

if (DESKTOP_APP_USE_PACKAGED AND NOT DESKTOP_APP_USE_PACKAGED_LAZY)
    add_library(external_fcitx5_qt INTERFACE IMPORTED GLOBAL)
    add_library(desktop-app::external_fcitx5_qt ALIAS external_fcitx5_qt)
else()
    add_library(external_fcitx5_qt STATIC)
    add_library(desktop-app::external_fcitx5_qt ALIAS external_fcitx5_qt)
    init_target(external_fcitx5_qt "(external)")

    set(fcitx5_qt_loc ${third_party_loc}/fcitx5-qt)

    if (DESKTOP_APP_QT6)
        set(fcitx5_qt_src ${fcitx5_qt_loc}/qt6/platforminputcontext)
    else()
        set(fcitx5_qt_src ${fcitx5_qt_loc}/qt5/platforminputcontext)
    endif()

    set_target_properties(external_fcitx5_qt PROPERTIES AUTOMOC ON)
    set(FCITX5_QT_EXTRA_PLUGIN_NAME "")

    configure_file("${fcitx5_qt_src}/fcitx5.json.in" "${CMAKE_CURRENT_BINARY_DIR}/fcitx5.json")

    nice_target_sources(external_fcitx5_qt ${fcitx5_qt_src}
    PRIVATE
        qfcitxplatforminputcontext.cpp
        qfcitxplatforminputcontext.h
        fcitxcandidatewindow.cpp
        fcitxcandidatewindow.h
        fcitxtheme.cpp
        fcitxtheme.h
        font.cpp
        font.h
        qtkey.cpp
        main.cpp
        main.h
    )

    target_include_directories(external_fcitx5_qt
    PRIVATE
        ${fcitx5_qt_src}
        ${fcitx5_qt_loc}/common
    )

    target_compile_definitions(external_fcitx5_qt
    PRIVATE
        QT_STATICPLUGIN
        FCITX_PLUGIN_DATA_FILE_PATH="${CMAKE_CURRENT_BINARY_DIR}/fcitx5.json"
    )

    add_subdirectory(fcitx5_qt_dbusaddons)
    target_link_libraries(external_fcitx5_qt
    PRIVATE
        desktop-app::external_fcitx5_qt_dbusaddons
        desktop-app::external_qt
        desktop-app::external_xcb
    )

    if (DESKTOP_APP_USE_PACKAGED)
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)

        target_link_libraries(external_fcitx5_qt
        PRIVATE
            PkgConfig::XKBCOMMON
        )
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(XKBCOMMON REQUIRED xkbcommon)

        target_include_directories(external_fcitx5_qt SYSTEM
        PUBLIC
            ${XKBCOMMON_INCLUDE_DIRS}
        )

        target_link_static_libraries(external_fcitx5_qt
        PRIVATE
            xkbcommon
        )
    endif()
endif()
