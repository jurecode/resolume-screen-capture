cmake_minimum_required(VERSION 3.20)

# macOS: one "universal" bundle for Apple Silicon and Intel. Must be set before project().
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "Arquitecturas de macOS")
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "macOS minimo")

project(ResolumeScreenCapture VERSION 1.0 LANGUAGES CXX)

if(WIN32)
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "Resolume Arena 7 es de 64 bits: configura con -A x64.")
    endif()
elseif(APPLE)
    enable_language(OBJCXX)
else()
    message(FATAL_ERROR "Este plugin se compila para Windows o macOS.")
endif()

set(FFGL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffgl/lib)
set(GLEW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/glew-2.1.0)

# Version of this build. The release workflow sets it from the git tag (v1.2.0 -> 1.2.0).
set(PLUGIN_VERSION "1.0.0" CACHE STRING "Version del plugin (x.y.z)")
# Where installed plugins look for updates. Empty = updates disabled (local test builds).
set(UPDATE_MANIFEST_URL "" CACHE STRING "URL https del latest.json de actualizaciones")

if(NOT PLUGIN_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "PLUGIN_VERSION debe ser x.y.z, no '${PLUGIN_VERSION}'")
endif()
set(PLUGIN_VERSION_MAJOR ${CMAKE_MATCH_1})
set(PLUGIN_VERSION_MINOR ${CMAKE_MATCH_2})

# Optional: copy the plugin here after every build, e.g.
#   -DRESOLUME_EXTRA_EFFECTS_DIR="C:/Users/<you>/Documents/Resolume Arena/Extra Effects"
set(RESOLUME_EXTRA_EFFECTS_DIR "" CACHE PATH "Carpeta Extra Effects de Resolume donde copiar el plugin")

add_library(ScreenCapture MODULE
    src/ScreenCapture.cpp
    src/ScreenCapture.h
    src/CaptureEngine.h
    src/CaptureTargets.cpp
    src/CaptureTargets.h
    src/Updater.cpp
    src/Updater.h
    src/UpdateDialog.h
    src/Platform.h
    src/FileLog.cpp
    src/FileLog.h
    src/Thumbnail.cpp
    ${FFGL_DIR}/FFGLSDK.cpp
)

target_compile_features(ScreenCapture PRIVATE cxx_std_17)
target_include_directories(ScreenCapture PRIVATE ${FFGL_DIR})
target_compile_definitions(ScreenCapture PRIVATE
    PLUGIN_VERSION_STRING="${PLUGIN_VERSION}"
    PLUGIN_VERSION_MAJOR=${PLUGIN_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PLUGIN_VERSION_MINOR}
    UPDATE_MANIFEST_URL="${UPDATE_MANIFEST_URL}"
)

if(WIN32)
    target_sources(ScreenCapture PRIVATE
        src/win/CaptureEngineWin.cpp
        src/win/CaptureTargetsWin.cpp
        src/win/PlatformWin.cpp
        src/win/UpdateDialogWin.cpp
        src/win/ScreenCapture.manifest
    )
    target_include_directories(ScreenCapture PRIVATE ${GLEW_DIR}/include)
    target_compile_definitions(ScreenCapture PRIVATE GLEW_STATIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_compile_options(ScreenCapture PRIVATE /EHsc /utf-8 /bigobj /MP)
    # The bundled glew32s.lib was built against the static CRT; we use the dll CRT like FFGL.
    target_link_options(ScreenCapture PRIVATE /NODEFAULTLIB:LIBCMT)
    target_link_libraries(ScreenCapture PRIVATE
        opengl32
        d3d11
        dxgi
        dwmapi
        windowsapp
        winhttp
        bcrypt
        shell32
        user32
        gdi32
        ${GLEW_DIR}/lib/Release/x64/glew32s.lib
    )
    if(RESOLUME_EXTRA_EFFECTS_DIR)
        add_custom_command(TARGET ScreenCapture POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${RESOLUME_EXTRA_EFFECTS_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ScreenCapture> "${RESOLUME_EXTRA_EFFECTS_DIR}"
            COMMENT "Copiando ScreenCapture.dll a ${RESOLUME_EXTRA_EFFECTS_DIR}"
        )
    endif()
else()
    target_sources(ScreenCapture PRIVATE
        src/mac/CaptureEngineMac.mm
        src/mac/CaptureTargetsMac.mm
        src/mac/PlatformMac.mm
        src/mac/UpdateDialogMac.mm
    )
    set_target_properties(ScreenCapture PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION bundle
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/src/mac/Info.plist.in
        MACOSX_BUNDLE_GUI_IDENTIFIER com.jurecode.resolume-screen-capture
        MACOSX_BUNDLE_BUNDLE_NAME "Captura Pantalla"
        MACOSX_BUNDLE_BUNDLE_VERSION ${PLUGIN_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PLUGIN_VERSION}
    )
    target_compile_definitions(ScreenCapture PRIVATE GL_SILENCE_DEPRECATION)
    target_compile_options(ScreenCapture PRIVATE $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
    target_link_libraries(ScreenCapture PRIVATE
        "-framework OpenGL"
        "-framework AppKit"
        "-framework CoreGraphics"
        "-framework CoreMedia"
        "-framework CoreVideo"
        "-framework Foundation"
        # Weak so the plugin still loads (without capture) on macOS older than 12.3.
        "-weak_framework ScreenCaptureKit"
    )
    if(RESOLUME_EXTRA_EFFECTS_DIR)
        add_custom_command(TARGET ScreenCapture POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory $<TARGET_BUNDLE_DIR:ScreenCapture> "${RESOLUME_EXTRA_EFFECTS_DIR}/ScreenCapture.bundle"
            COMMENT "Copiando ScreenCapture.bundle a ${RESOLUME_EXTRA_EFFECTS_DIR}"
        )
    endif()
endif()
