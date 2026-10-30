# DJFFNY.exe, le lanceur tout-en-un (installation, reglages, mod, jeu). Projet a part du jeu :
# il ne contient rien du jeu et se construit seul, avec Visual Studio :
#
#   cmake -S lanceur -B build_lanceur -A x64
#   cmake --build build_lanceur --config Release
#
# Il faut le SDK WebView2 de Microsoft (paquet NuGet Microsoft.Web.WebView2), extrait dans
# tiers/webview2/sdk, ou indique par -DWEBVIEW2_SDK=<...>/build/native. Le chargeur est lie en
# statique et le runtime C aussi : DJFFNY.exe tient en un fichier.
cmake_minimum_required(VERSION 3.20)
project(djffny CXX)

set(WEBVIEW2_SDK "${CMAKE_CURRENT_SOURCE_DIR}/../tiers/webview2/sdk/build/native" CACHE PATH
    "Dossier build/native du paquet NuGet Microsoft.Web.WebView2")
if(NOT EXISTS "${WEBVIEW2_SDK}/include/WebView2.h")
    message(FATAL_ERROR "SDK WebView2 introuvable dans ${WEBVIEW2_SDK} (voir l'en-tete de ce fichier)")
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_executable(DJFFNY WIN32 djffny.cpp djffny.rc)
file(GLOB IMAGES "${CMAKE_CURRENT_SOURCE_DIR}/images/*")
set_source_files_properties(djffny.rc PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/page.html;${IMAGES}")
target_compile_features(DJFFNY PRIVATE cxx_std_17)
target_compile_definitions(DJFFNY PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_compile_options(DJFFNY PRIVATE /utf-8 /W3)
target_include_directories(DJFFNY PRIVATE "${WEBVIEW2_SDK}/include")
target_link_libraries(DJFFNY PRIVATE "${WEBVIEW2_SDK}/x64/WebView2LoaderStatic.lib"
    winhttp bcrypt shlwapi ole32 shell32 user32 advapi32 version)
