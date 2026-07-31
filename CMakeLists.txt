cmake_minimum_required(VERSION 3.21)

project(D4AssetBrowser
    VERSION 2.1.0
    DESCRIPTION "Native C++/Qt6 Diablo IV asset browser (d4analyzer code stack)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Dependencies (resolved by the vcpkg toolchain — see CMakePresets.json) ──
# UI / rendering: Qt 6 Widgets + OpenGL (same surface as d4analyzer).
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Svg)

# Game-archive reading: native CASC reader (ported from the proven pure-Python
# reader). D4 stores assets in a nested TVFS that CascLib (as built here) did not
# resolve, so we parse the .build config + .idx index + TVFS root ourselves and
# BLTE-decode with zlib + lz4 (+ Salsa20 for encrypted frames).
find_package(ZLIB REQUIRED)
find_package(lz4 CONFIG REQUIRED)

# glTF: fastgltf (writer/parser) + tinygltf (image/loader).
# These back the Models tab / .glb export. (Draco mesh-compression is deferred to the
# Models .glb-export milestone — it's omitted for now because its long internal file
# paths trip the Windows 260-char path limit under deep build trees. Re-add it with
# long-path support enabled when we wire export.)
find_package(fastgltf CONFIG REQUIRED)
# tinygltf is header-only in vcpkg; it exposes an include dir (and, on some ports,
# a config target). We locate the header and add it to the include path.
find_path(TINYGLTF_INCLUDE_DIR "tiny_gltf.h")

qt_standard_project_setup()

# AUTORCC is NOT enabled by qt_standard_project_setup() — that sets AUTOMOC and AUTOUIC only.
# Without this, res/app.qrc is listed as a source, configures without complaint, and is simply
# never compiled: the window icon silently stays whatever the fallback is. Verified by searching
# the built exe for the PNG's bytes and not finding them.
set(CMAKE_AUTORCC ON)

# rc.exe reads app.ico THROUGH app.rc, and CMake does not know that. Replacing the .ico leaves the
# .rc byte-identical, so ninja sees nothing to do and the exe keeps the OLD icon while every
# timestamp says the build is current — indistinguishable from "the icon change did not work".
set_property(SOURCE res/app.rc APPEND PROPERTY
             OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/res/app.ico")

qt_add_executable(D4AssetBrowser WIN32
    res/app.rc
    res/app.qrc     # window icon, embedded (AUTORCC compiles it in)
    src/main.cpp

    src/app/Config.h            src/app/Config.cpp
    src/app/MainWindow.h        src/app/MainWindow.cpp
    src/app/SettingsDialog.h    src/app/SettingsDialog.cpp
    src/app/ExportCapture.h     src/app/ExportCapture.cpp
    src/app/ExportNotifier.h
    src/app/LogConsole.h        src/app/LogConsole.cpp
    src/app/SehGuard.h          src/app/SehGuard.cpp

    src/casc/CascReader.h       src/casc/CascReader.cpp

    src/index/CoreToc.h         src/index/CoreToc.cpp
    src/index/SnoIndex.h        src/index/SnoIndex.cpp
    src/index/SnoListModel.h    src/index/SnoListModel.cpp
    src/index/AppearanceMeta.h  src/index/AppearanceMeta.cpp
    src/index/MatSnoSweep.h     src/index/MatSnoSweep.cpp
    src/model/AppearanceMatBin.h src/model/AppearanceMatBin.cpp
    src/tex/TextureDefTable.h   src/tex/TextureDefTable.cpp
    src/index/IconIndex.h       src/index/IconIndex.cpp
    src/index/AssetLinks.h      src/index/AssetLinks.cpp
    src/index/ItemDef.h         src/index/ItemDef.cpp
    src/index/DadOverride.h     src/index/DadOverride.cpp
    src/index/IconAudit.h       src/index/IconAudit.cpp
    src/index/ItemHoverIndex.h   src/index/ItemHoverIndex.cpp
    src/index/BackTrophyIndex.h  src/index/BackTrophyIndex.cpp
    src/index/WardrobeAnimIndex.h  src/index/WardrobeAnimIndex.cpp

    src/tex/TexFormat.h         src/tex/TexFormat.cpp
    src/tex/TexMeta.h           src/tex/TexMeta.cpp
    src/tex/BcDecode.h          src/tex/BcDecode.cpp
    src/tex/FrameTable.h        src/tex/FrameTable.cpp

    src/gl/GLTextureWidget.h    src/gl/GLTextureWidget.cpp
    src/gl/GLModelWidget.h      src/gl/GLModelWidget.cpp
    src/gl/GifEncoder.h         src/gl/GifEncoder.cpp

    src/text/StringTable.h      src/text/StringTable.cpp

    src/model/Appearance.h      src/model/Appearance.cpp
    src/model/Material.h        src/model/Material.cpp
    src/model/ModelGeometry.h
    src/model/ModelParser.h     src/model/ModelParser.cpp
    src/model/AnimParser.h      src/model/AnimParser.cpp
    src/model/ModelExporter.h   src/model/ModelExporter.cpp
    src/model/Retarget.h        src/model/Retarget.cpp
    src/model/Hardpoints.h      src/model/Hardpoints.cpp
    src/model/RigMath.h
    src/model/Attachments.h     src/model/Attachments.cpp
    src/model/FormatProbe.h     src/model/FormatProbe.cpp
    src/model/MaterialDecode.h  src/model/MaterialDecode.cpp

    src/deps/D4DataDownloader.h src/deps/D4DataDownloader.cpp
    src/deps/DependencyDialog.h src/deps/DependencyDialog.cpp
    src/deps/UpdateCheck.h      src/deps/UpdateCheck.cpp

    src/util/CsvCopy.h          src/util/CsvCopy.cpp

    src/tabs/BrowserTab.h
    src/tabs/PanelBox.h
    src/tabs/ViewGlyphs.h
    src/tabs/HintBar.h
    src/tabs/BatchSink.h
    src/tabs/TexturesTab.h      src/tabs/TexturesTab.cpp
    src/tabs/ModelsTab.h        src/tabs/ModelsTab.cpp
    src/tabs/ModelsTab_Panels.cpp
    src/tabs/ModelsTab_Export.cpp
    src/tabs/ModelOutliner.h    src/tabs/ModelOutliner.cpp
    src/tabs/WardrobeTab2.h     src/tabs/WardrobeTab2.cpp
    src/tabs/WardrobeTab2_Panels.cpp
    src/tabs/MarkingCompose.h   src/tabs/MarkingCompose.cpp
    src/tabs/StableTab2.h       src/tabs/StableTab2.cpp
    src/tabs/BulkExtractorTab.h src/tabs/BulkExtractorTab.cpp
)

target_include_directories(D4AssetBrowser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${TINYGLTF_INCLUDE_DIR}
)

# Crash resilience: /EHa lets C++ try/catch intercept Windows structured
# exceptions (access violations from CPU OOB *or* GPU-driver faults) so a bad
# model can be quarantined in-session instead of terminating the process.
# (/EHa supersedes CMake's default /EHsc since it appears later on the cmdline.)
if(MSVC)
    target_compile_options(D4AssetBrowser PRIVATE /EHa)
    # Multi-processor compilation: let each cl.exe invocation use all cores (helps full/clean builds;
    # Ninja already parallelises across files, this parallelises within).
    target_compile_options(D4AssetBrowser PRIVATE /MP)
    # The PCH force-includes QtGui/QtWidgets (→ windows.h) into every TU; NOMINMAX stops windows.h's
    # min/max macros from clobbering std::min/std::max anywhere.
    target_compile_definitions(D4AssetBrowser PRIVATE NOMINMAX)
endif()

# ── Precompiled header ────────────────────────────────────────────────────────
# Front-load the heavy, ubiquitous Qt/std headers so each incremental recompile — especially the
# large ModelsTab.cpp / GLModelWidget.cpp we iterate on — skips re-parsing them from scratch.
# Establishing the PCH triggers ONE full rebuild; every edit-compile after that is noticeably faster.
# Kept to stable, widely-included framework headers only (no app headers), to avoid coupling.
target_precompile_headers(D4AssetBrowser PRIVATE
    <QtCore/QString>
    <QtCore/QByteArray>
    <QtCore/QStringList>
    <QtCore/QHash>
    <QtCore/QMap>
    <QtCore/QSet>
    <QtCore/QList>
    <QtCore/QVector>
    <QtCore/QFile>
    <QtCore/QDir>
    <QtCore/QFileInfo>
    <QtCore/QDebug>
    <QtCore/QObject>
    <QtCore/QRegularExpression>
    <QtGui/QImage>
    <QtGui/QColor>
    <QtWidgets/QWidget>
    <vector>
    <memory>
    <algorithm>
    <cstdint>
)

target_link_libraries(D4AssetBrowser PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    Qt6::Svg
    ZLIB::ZLIB
    lz4::lz4
    fastgltf::fastgltf
)

# ── Packaging ────────────────────────────────────────────────────────────────
# Detect whether Qt was built statically (vcpkg x64-windows-static triplet). A static Qt lets us ship
# ONE self-contained .exe with the plugins baked in — no Qt DLLs, no plugins/ folder, nothing to deploy.
# A dynamic Qt falls back to copying the runtime DLLs + plugins next to the exe (windeployqt).
get_target_property(_qt_core_type Qt6::Core TYPE)
set(D4_STATIC_QT OFF)
if(_qt_core_type STREQUAL "STATIC_LIBRARY")
    set(D4_STATIC_QT ON)
endif()

if(D4_STATIC_QT)
    message(STATUS "Qt is STATIC → building a single self-contained D4AssetBrowser.exe (plugins baked in).")
    # Bake the plugins a single-exe build needs (there is no plugins/ folder to load them from):
    # the Windows platform + native style, the image formats the tool reads/writes, and SVG for icons.
    # Only import the plugin targets that actually exist in this static Qt, so a Qt build that omits
    # one (e.g. a slimmer imageformats set) can't fail configure.
    set(_d4_wanted_plugins
        Qt6::QWindowsIntegrationPlugin
        Qt6::QWindowsVistaStylePlugin
        Qt6::QJpegPlugin
        Qt6::QGifPlugin
        Qt6::QICOPlugin
        Qt6::QSvgPlugin
        Qt6::QSvgIconPlugin)
    set(_d4_have_plugins)
    foreach(_p IN LISTS _d4_wanted_plugins)
        if(TARGET ${_p})
            list(APPEND _d4_have_plugins ${_p})
        endif()
    endforeach()
    if(_d4_have_plugins)
        qt_import_plugins(D4AssetBrowser INCLUDE ${_d4_have_plugins})
    endif()
    # Static Qt is built against the static CRT; match it so the single exe has zero external deps
    # (no VC++ redistributable required on the target machine).
    if(MSVC)
        set_property(TARGET D4AssetBrowser PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
elseif(WIN32)
    # Dynamic Qt: copy the runtime DLLs + plugins next to the .exe so it launches stand-alone.
    #
    # NOT qt_generate_deploy_app_script. That generates a call to
    # qt6_deploy_runtime_dependencies, which needs windeployqt.exe — and THIS VCPKG QT DOES NOT
    # SHIP IT. vcpkg_installed/x64-windows contains only share/Qt6/mkspecs/features/win32/
    # windeployqt.prf, the qmake feature file, not the tool. So the install step always failed with
    # "No Qt deploy tool available for this target platform", and NO_UNSUPPORTED_PLATFORM_ERROR only
    # downgrades the configure-time check, not the run-time failure.
    #
    # run.bat has been deploying correctly this whole time by copying the same files by hand, so
    # this does what run.bat does. Same source, same plugin set, no external tool.
    set(_qt_bin "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/bin")
    set(_qt_plugins "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows/Qt6/plugins")
    install(DIRECTORY "${_qt_bin}/" DESTINATION . FILES_MATCHING PATTERN "*.dll")
    # Only the plugin families a Widgets/OpenGL app actually loads. platforms is mandatory —
    # without qwindows.dll the exe dies at startup with "no Qt platform plugin could be initialized".
    foreach(_p platforms imageformats styles iconengines)
        if(EXISTS "${_qt_plugins}/${_p}")
            install(DIRECTORY "${_qt_plugins}/${_p}" DESTINATION .
                    FILES_MATCHING PATTERN "*.dll")
        endif()
    endforeach()
endif()

install(TARGETS D4AssetBrowser
    RUNTIME DESTINATION .
    BUNDLE  DESTINATION .)
