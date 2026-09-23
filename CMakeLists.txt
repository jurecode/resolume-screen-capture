cmake_minimum_required(VERSION 3.20)

# macOS: one "universal" bundle for Apple Silicon and Intel. Must be set before project().
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Arquitecturas de macOS")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS minimo")

project(ResolumePlugins VERSION 1.0 LANGUAGES CXX)

if(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Resolume Arena 7 es de 64 bits: configura con -A x64.")
    endif()
elseif(APPLE)
    enable_language(OBJCXX)
else()
    message(FATAL_ERROR "Estos plugins se compilan para Windows o macOS.")
endif()

set(FFGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffgl/lib)
set(GLEW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew-2.1.0)

# Version of this build. The release workflow sets it from the git tag (v1.2.0 -> 1.2.0).
set(PLUGIN_VERSION "1.0.0" CACHE STRING "Version de los plugins (x.y.z)")
# Where installed plugins look for updates. Empty = updates disabled (local test builds).
set(UPDATE_MANIFEST_URL "" CACHE STRING "URL https del latest.json de actualizaciones")

if(NOT PLUGIN_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "PLUGIN_VERSION debe ser x.y.z, no '${PLUGIN_VERSION}'")
endif()
set(PLUGIN_VERSION_MAJOR ${CMAKE_MATCH_1})
set(PLUGIN_VERSION_MINOR ${CMAKE_MATCH_2})

# Optional: copy the plugins here after every build, e.g.
#   -DRESOLUME_EXTRA_EFFECTS_DIR="C:/Users/<you>/Documents/Resolume Arena/Extra Effects"
set(RESOLUME_EXTRA_EFFECTS_DIR "" CACHE PATH "Carpeta Extra Effects de Resolume donde copiar los plugins")

set(SHARED_SOURCES
    src/Updater.cpp src/Updater.h src/UpdateDialog.h
    src/Platform.h src/PluginIdentity.h
    src/FileLog.cpp src/FileLog.h
)
set(WIN_SHARED_SOURCES src/win/PlatformWin.cpp src/win/UpdateDialogWin.cpp src/win/ScreenCapture.manifest)
set(MAC_SHARED_SOURCES src/mac/PlatformMac.mm src/mac/UpdateDialogMac.mm)

# One Resolume Arena 7 (FFGL 2) plugin: a .dll on Windows, a universal .bundle on macOS.
#
#   add_ffgl_plugin(<target>
#       DISPLAY_NAME    "Shown in update windows and notifications"
#       SETTINGS_PREFIX "Prefix for its saved update choices"
#       LOG_FILE        "Log file name in Documents"
#       UPDATE_FIELD    "latest.json field suffix: url<suffix> (Windows), url<suffix>_mac (macOS)"
#       BUNDLE_ID       "macOS bundle identifier"
#       SOURCES / WIN_SOURCES / MAC_SOURCES ...
#       WIN_LIBRARIES / MAC_FRAMEWORKS ...)
function(add_ffgl_plugin target)
    cmake_parse_arguments(PLUGIN ""
        "DISPLAY_NAME;SETTINGS_PREFIX;LOG_FILE;UPDATE_FIELD;BUNDLE_ID"
        "SOURCES;WIN_SOURCES;MAC_SOURCES;WIN_LIBRARIES;MAC_FRAMEWORKS" ${ARGN})

    add_library(${target} MODULE ${PLUGIN_SOURCES} ${SHARED_SOURCES} ${FFGL_DIR}/FFGLSDK.cpp)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_include_directories(${target} PRIVATE ${FFGL_DIR})
    target_compile_definitions(${target} PRIVATE
        PLUGIN_VERSION_STRING="${PLUGIN_VERSION}"
        PLUGIN_VERSION_MAJOR=${PLUGIN_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PLUGIN_VERSION_MINOR}
        UPDATE_MANIFEST_URL="${UPDATE_MANIFEST_URL}"
        PLUGIN_DISPLAY_NAME="${PLUGIN_DISPLAY_NAME}"
        PLUGIN_SETTINGS_PREFIX="${PLUGIN_SETTINGS_PREFIX}"
        LOG_FILE_NAME="${PLUGIN_LOG_FILE}"
    )

    if(WIN32)
        target_sources(${target} PRIVATE ${WIN_SHARED_SOURCES} ${PLUGIN_WIN_SOURCES})
        target_include_directories(${target} PRIVATE ${GLEW_DIR}/include)
        target_compile_definitions(${target} PRIVATE
            GLEW_STATIC NOMINMAX _CRT_SECURE_NO_WARNINGS
            UPDATE_URL_FIELD="url${PLUGIN_UPDATE_FIELD}"
            UPDATE_SHA256_FIELD="sha256${PLUGIN_UPDATE_FIELD}"
        )
        target_compile_options(${target} PRIVATE /EHsc /utf-8 /bigobj /MP)
        # The bundled glew32s.lib was built against the static CRT; we use the dll CRT like FFGL.
        target_link_options(${target} PRIVATE /NODEFAULTLIB:LIBCMT)
        target_link_libraries(${target} PRIVATE
            opengl32 winhttp bcrypt shell32 user32 gdi32 ${PLUGIN_WIN_LIBRARIES}
            ${GLEW_DIR}/lib/Release/x64/glew32s.lib
        )
        if(RESOLUME_EXTRA_EFFECTS_DIR)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E make_directory "${RESOLUME_EXTRA_EFFECTS_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:${target}> "${RESOLUME_EXTRA_EFFECTS_DIR}"
                COMMENT "Copiando ${target}.dll a ${RESOLUME_EXTRA_EFFECTS_DIR}"
            )
        endif()
    else()
        target_sources(${target} PRIVATE ${MAC_SHARED_SOURCES} ${PLUGIN_MAC_SOURCES})
        target_compile_definitions(${target} PRIVATE
            GL_SILENCE_DEPRECATION
            UPDATE_URL_FIELD="url${PLUGIN_UPDATE_FIELD}_mac"
            UPDATE_SHA256_FIELD="sha256${PLUGIN_UPDATE_FIELD}_mac"
        )
        target_compile_options(${target} PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
        set_target_properties(${target} PROPERTIES
            BUNDLE TRUE
            BUNDLE_EXTENSION bundle
            MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/mac/Info.plist.in
            MACOSX_BUNDLE_GUI_IDENTIFIER ${PLUGIN_BUNDLE_ID}
            MACOSX_BUNDLE_BUNDLE_NAME "${PLUGIN_DISPLAY_NAME}"
            MACOSX_BUNDLE_BUNDLE_VERSION ${PLUGIN_VERSION}
            MACOSX_BUNDLE_SHORT_VERSION_STRING ${PLUGIN_VERSION}
        )
        set(frameworks)
        foreach(framework OpenGL AppKit CoreGraphics Foundation ${PLUGIN_MAC_FRAMEWORKS})
            list(APPEND frameworks "-framework ${framework}")
        endforeach()
        target_link_libraries(${target} PRIVATE ${frameworks})
        if(RESOLUME_EXTRA_EFFECTS_DIR)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:${target}> "${RESOLUME_EXTRA_EFFECTS_DIR}/${target}.bundle"
                COMMENT "Copiando ${target}.bundle a ${RESOLUME_EXTRA_EFFECTS_DIR}"
            )
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------------------------
# Captura Pantalla: live capture of a monitor or a window.
add_ffgl_plugin(ScreenCapture
    DISPLAY_NAME    "Captura Pantalla"
    LOG_FILE        "CapturaPantalla-log.txt"
    BUNDLE_ID       com.jurecode.resolume-screen-capture
    SOURCES
        src/ScreenCapture.cpp src/ScreenCapture.h src/Thumbnail.cpp
        src/CaptureEngine.h src/CaptureTargets.cpp src/CaptureTargets.h
        src/ContentFinder.cpp src/ContentFinder.h
    WIN_SOURCES
        src/win/CaptureEngineWin.cpp src/win/CaptureTargetsWin.cpp
        src/win/ContentDetectorWin.cpp src/win/ContentDetectorWin.h
    MAC_SOURCES
        src/mac/CaptureEngineMac.mm src/mac/CaptureTargetsMac.mm
    WIN_LIBRARIES d3d11 dxgi dwmapi windowsapp
    MAC_FRAMEWORKS CoreMedia CoreVideo
)
if(APPLE)
    # Weak so the plugin still loads (without capture) on macOS older than 12.3.
    target_link_libraries(ScreenCapture PRIVATE "-weak_framework ScreenCaptureKit")
endif()

# ---------------------------------------------------------------------------------------------
# Zocalo: animated lower third with a photo, a name and a subtitle.
add_ffgl_plugin(Zocalo
    DISPLAY_NAME    "Zocalo"
    SETTINGS_PREFIX "Zocalo"
    LOG_FILE        "Zocalo-log.txt"
    UPDATE_FIELD    "_zocalo"
    BUNDLE_ID       com.jurecode.resolume-zocalo
    SOURCES
        src/zocalo/Zocalo.cpp src/zocalo/Zocalo.h src/zocalo/ZocaloThumbnail.cpp
        src/Drawing.h
    WIN_SOURCES src/win/DrawingWin.cpp
    MAC_SOURCES src/mac/DrawingMac.mm
    WIN_LIBRARIES windowscodecs windowsapp
    MAC_FRAMEWORKS CoreText ImageIO
)

# ---------------------------------------------------------------------------------------------
# macOS only: Captura Pantalla for Resolume Arena 6 (FFGL 1.6). It shares the capture engine,
# the source list and the updater with the Arena 7 plugin.
if(APPLE)
    set(FFGL16_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffgl-1.6/ffgl)
    set(FFGL16_SOURCES
        ${FFGL16_DIR}/FFGL.cpp
        ${FFGL16_DIR}/FFGLPluginInfo.cpp
        ${FFGL16_DIR}/FFGLPluginInfoData.cpp
        ${FFGL16_DIR}/FFGLPluginManager.cpp
        ${FFGL16_DIR}/FFGLPluginSDK.cpp
    )
    add_library(ScreenCaptureArena6 MODULE
        src/arena6/ScreenCaptureArena6.cpp
        src/CaptureTargets.cpp
        ${SHARED_SOURCES}
        ${MAC_SHARED_SOURCES}
        src/mac/CaptureEngineMac.mm
        src/mac/CaptureTargetsMac.mm
        ${FFGL16_SOURCES}
    )
    # The 2018 SDK predates today's compilers' warnings; it's not our code.
    set_source_files_properties(${FFGL16_SOURCES} PROPERTIES COMPILE_OPTIONS "-w;-includeTargetConditionals.h")
    target_compile_features(ScreenCaptureArena6 PRIVATE cxx_std_17)
    target_include_directories(ScreenCaptureArena6 PRIVATE ${FFGL16_DIR})
    target_compile_definitions(ScreenCaptureArena6 PRIVATE
        GL_SILENCE_DEPRECATION
        PLUGIN_VERSION_STRING="${PLUGIN_VERSION}"
        PLUGIN_VERSION_MAJOR=${PLUGIN_VERSION_MAJOR}
        PLUGIN_VERSION_MINOR=${PLUGIN_VERSION_MINOR}
        UPDATE_MANIFEST_URL="${UPDATE_MANIFEST_URL}"
        PLUGIN_DISPLAY_NAME="Captura Pantalla"
        PLUGIN_SETTINGS_PREFIX="Arena6"
        LOG_FILE_NAME="CapturaPantalla-log.txt"
        UPDATE_URL_FIELD="url_mac_arena6"
        UPDATE_SHA256_FIELD="sha256_mac_arena6"
    )
    target_compile_options(ScreenCaptureArena6 PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    set_target_properties(ScreenCaptureArena6 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION bundle
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/mac/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.jurecode.resolume-screen-capture-arena6
        MACOSX_BUNDLE_BUNDLE_NAME "Captura Pantalla (Arena 6)"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PLUGIN_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PLUGIN_VERSION}
    )
    target_link_libraries(ScreenCaptureArena6 PRIVATE
        "-framework OpenGL" "-framework AppKit" "-framework CoreGraphics" "-framework CoreMedia"
        "-framework CoreVideo" "-framework Foundation" "-weak_framework ScreenCaptureKit"
    )
endif()
