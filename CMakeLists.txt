cmake_minimum_required(VERSION 3.20)
project(ResolumeScreenCapture VERSION 1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "Este plugin usa Windows Graphics Capture: compilalo en Windows con Visual Studio 2022.")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Resolume Arena 7 es de 64 bits: configura con -A x64.")
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

# Optional: copy the dll here after every build, e.g.
#   -DRESOLUME_EXTRA_EFFECTS_DIR="C:/Users/<you>/Documents/Resolume Arena/Extra Effects"
set(RESOLUME_EXTRA_EFFECTS_DIR "" CACHE PATH "Carpeta Extra Effects de Resolume donde copiar el plugin")

add_library(ScreenCapture MODULE
    src/ScreenCapture.cpp
    src/ScreenCapture.h
    src/CaptureTargets.cpp
    src/CaptureTargets.h
    src/WgcCapture.cpp
    src/WgcCapture.h
    src/Updater.cpp
    src/FileLog.cpp
    src/FileLog.h
    src/Updater.h
    ${FFGL_DIR}/FFGLSDK.cpp
)

target_compile_features(ScreenCapture PRIVATE cxx_std_17)
target_include_directories(ScreenCapture PRIVATE ${FFGL_DIR} ${GLEW_DIR}/include)
target_compile_definitions(ScreenCapture PRIVATE
    GLEW_STATIC
    NOMINMAX
    _CRT_SECURE_NO_WARNINGS
    PLUGIN_VERSION_STRING="${PLUGIN_VERSION}"
    PLUGIN_VERSION_MAJOR=${PLUGIN_VERSION_MAJOR}
    PLUGIN_VERSION_MINOR=${PLUGIN_VERSION_MINOR}
    UPDATE_MANIFEST_URL="${UPDATE_MANIFEST_URL}"
)
target_compile_options(ScreenCapture PRIVATE /EHsc /utf-8 /bigobj /MP)
target_link_libraries(ScreenCapture PRIVATE
    opengl32
    d3d11
    dxgi
    dwmapi
    windowsapp
    winhttp
    bcrypt
    shell32
    ${GLEW_DIR}/lib/Release/x64/glew32s.lib
)

if(RESOLUME_EXTRA_EFFECTS_DIR)
    add_custom_command(TARGET ScreenCapture POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RESOLUME_EXTRA_EFFECTS_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ScreenCapture> "${RESOLUME_EXTRA_EFFECTS_DIR}"
        COMMENT "Copiando ScreenCapture.dll a ${RESOLUME_EXTRA_EFFECTS_DIR}"
    )
endif()
