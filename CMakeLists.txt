cmake_minimum_required(VERSION 3.28)

# Dynamically locate vcpkg root without hardcoding paths
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VCPKG_ROOT} AND EXISTS "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake")
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
  else()
    find_program(VCPKG_EXECUTABLE vcpkg)
    if(VCPKG_EXECUTABLE)
      get_filename_component(VCPKG_ROOT_DIR "${VCPKG_EXECUTABLE}" DIRECTORY)
      if(EXISTS "${VCPKG_ROOT_DIR}/scripts/buildsystems/vcpkg.cmake")
        set(CMAKE_TOOLCHAIN_FILE "${VCPKG_ROOT_DIR}/scripts/buildsystems/vcpkg.cmake" CACHE STRING "")
      endif()
    endif()
  endif()
endif()

if(NOT DEFINED VCPKG_TARGET_TRIPLET)
  set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
endif()

if(CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_TOOLCHAIN_FILE}")
  include("${CMAKE_TOOLCHAIN_FILE}")
endif()

project(KryptonUpdater
  VERSION 0.1.0
  DESCRIPTION "Native WinUI 3 installer and background updater for Krypton"
  LANGUAGES C CXX)

if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

file(READ "${CMAKE_SOURCE_DIR}/resources/product-version.json" KR_VERSION_JSON)
string(JSON KR_DEFAULT_VERSION GET "${KR_VERSION_JSON}" version)
set(KR_PRODUCT_VERSION "${KR_DEFAULT_VERSION}" CACHE STRING "Four-part Windows product/file version")
if(NOT KR_PRODUCT_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+\\.[0-9]+$")
  message(FATAL_ERROR "KR_PRODUCT_VERSION must contain four numeric components")
endif()
string(REPLACE "." ";" KR_VERSION_PARTS "${KR_PRODUCT_VERSION}")
foreach(KR_COMPONENT IN LISTS KR_VERSION_PARTS)
  if(KR_COMPONENT GREATER 65535)
    message(FATAL_ERROR "Windows version components must not exceed 65535")
  endif()
endforeach()
string(REPLACE "." "," KR_FILE_VERSION_COMMAS "${KR_PRODUCT_VERSION}")
string(TIMESTAMP KR_COPYRIGHT_YEAR "%Y" UTC)

function(kr_add_version_resource target description comments)
  set(KR_FILE_DESCRIPTION "${description}")
  set(KR_INTERNAL_NAME "${target}")
  set(KR_ORIGINAL_FILENAME "${target}.exe")
  set(KR_FILE_COMMENTS "${comments}")
  set(KR_VERSION_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
  configure_file(resources/version_info.rc.in "${KR_VERSION_RESOURCE}" @ONLY)
  target_sources(${target} PRIVATE "${KR_VERSION_RESOURCE}")
endfunction()

option(KR_BUILD_TESTS "Build updater unit test suites" ON)
option(KR_ALLOW_UNSIGNED "Allow unsigned legacy binaries in development" OFF)
set(KR_OFFLINE_PAYLOAD "" CACHE FILEPATH "Optional path to an MSI, EXE, or 7z file to embed for offline installation")
set(KR_OFFLINE_MANIFEST "" CACHE FILEPATH "SHA-256 manifest for an embedded offline archive")
set(KR_OFFLINE_SIGNATURE "" CACHE FILEPATH "Krypton release signature for the embedded manifest")
option(KR_REQUIRE_OFFLINE "Require an embedded archive and SHA-256 manifest" OFF)
if(KR_REQUIRE_OFFLINE AND (NOT KR_OFFLINE_PAYLOAD OR NOT KR_OFFLINE_MANIFEST OR NOT KR_OFFLINE_SIGNATURE))
  message(FATAL_ERROR "An offline build requires both a payload and its manifest")
endif()

function(kr_apply_compile_options target)
  if(MSVC)
    target_compile_options(${target} PRIVATE
      /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc /bigobj)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
  if(WIN32)
    target_compile_definitions(${target} PRIVATE
      NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE POCO_STATIC)
    if(KR_ALLOW_UNSIGNED)
      target_compile_definitions(${target} PRIVATE KR_ALLOW_UNSIGNED)
    endif()
  endif()
endfunction()

find_package(Poco CONFIG REQUIRED SevenZip Foundation JSON)

set(KR_ZUCCHINI_EXECUTABLE
  "${CMAKE_SOURCE_DIR}/../Krypton/build/src/out/Default/KryptonPatch.exe"
  CACHE FILEPATH "Statically linked Google Zucchini patch helper")
if(NOT EXISTS "${KR_ZUCCHINI_EXECUTABLE}")
  message(FATAL_ERROR "Build KryptonPatch.exe in the existing Chromium graph before building the updater")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${KR_ZUCCHINI_EXECUTABLE}")
file(SHA256 "${KR_ZUCCHINI_EXECUTABLE}" KR_ZUCCHINI_SHA256)
file(TO_CMAKE_PATH "${KR_ZUCCHINI_EXECUTABLE}" KR_ZUCCHINI_RESOURCE)

function(kr_embed_zucchini target)
  target_compile_definitions(${target} PRIVATE
    KR_EMBED_ZUCCHINI_FILE="${KR_ZUCCHINI_RESOURCE}")
endfunction()

# --- Common C++23 Logic Library -----------------------------------------------
add_library(kr_updater_common STATIC
  src/common/cpp/release_manifest.cpp
  src/windows/cpp/security/release_security.cpp
  src/windows/cpp/signed_update_service.cpp
  src/common/cpp/compatibility.cpp
  src/common/cpp/semver.cpp
  src/common/cpp/thread_pool.cpp
  src/common/cpp/manifest_verifier.cpp
  src/common/cpp/archive_extractor.cpp
  src/common/cpp/update_state_machine.cpp
  src/common/cpp/updater_engine.cpp
  src/windows/cpp/security/authenticode_verifier.cpp
  src/windows/cpp/security/path_sanitizer.cpp
  src/windows/cpp/security/os_capability.cpp
  src/windows/cpp/http_downloader.cpp
  src/windows/cpp/payload_runner.cpp
  src/windows/cpp/prerequisites.cpp
  src/windows/cpp/chromium_install.cpp
  src/windows/cpp/legacy_uninstall.cpp
  src/windows/cpp/offline_payload.cpp
  src/windows/cpp/version_manager.cpp
    src/windows/cpp/install_cleanup.cpp
  src/windows/cpp/shell_registrar.cpp
)
target_include_directories(kr_updater_common PUBLIC
  src/common/include
  src/windows/include
)
target_link_libraries(kr_updater_common PUBLIC
  Poco::SevenZip
  Poco::Foundation
  Poco::JSON
  msdelta.lib
  Rstrtmgr.lib
  bcrypt.lib
  winhttp.lib
  wintrust.lib
  crypt32.lib
  shell32.lib
  ole32.lib
  propsys.lib
  user32.lib
  gdi32.lib
  dwmapi.lib
  uxtheme.lib
  windowsapp.lib
  comctl32.lib
  version.lib
  taskschd.lib
)
kr_apply_compile_options(kr_updater_common)
if(KR_BUILD_TESTS)
  target_compile_definitions(kr_updater_common PRIVATE KR_UPDATER_TEST_SEAMS)
endif()
target_compile_definitions(kr_updater_common PRIVATE
  KR_ZUCCHINI_SHA256="${KR_ZUCCHINI_SHA256}")

# --- KryptonUpdater.exe -------------------------------------------------------
set(UPDATER_SOURCES
  src/windows/cpp/main.cpp
  src/windows/cpp/ui/headless_view.cpp
  src/windows/cpp/ui/winui_installer_window.cpp
  src/windows/cpp/resources.rc
  src/windows/manifest/krypton_updater.manifest
)

add_executable(KryptonSetup WIN32 ${UPDATER_SOURCES})
target_link_libraries(KryptonSetup PRIVATE kr_updater_common)
kr_apply_compile_options(KryptonSetup)
kr_add_version_resource(KryptonSetup "Krypton Setup" "Offline installer for the Krypton browser")
kr_embed_zucchini(KryptonSetup)

add_executable(KryptonWebSetup WIN32 ${UPDATER_SOURCES})
target_link_libraries(KryptonWebSetup PRIVATE kr_updater_common)
target_compile_definitions(KryptonWebSetup PRIVATE KR_WEB_INSTALLER)
kr_apply_compile_options(KryptonWebSetup)
kr_add_version_resource(KryptonWebSetup "Krypton Web Setup" "Downloads and installs the signed stable Krypton release")
kr_embed_zucchini(KryptonWebSetup)

add_executable(KryptonUninstaller WIN32
  src/windows/cpp/uninstaller_main.cpp
  src/windows/cpp/ui/headless_view.cpp
  src/windows/cpp/ui/winui_installer_window.cpp
  src/windows/cpp/resources.rc
  src/windows/manifest/krypton_updater.manifest)
target_link_libraries(KryptonUninstaller PRIVATE kr_updater_common)
kr_apply_compile_options(KryptonUninstaller)
kr_add_version_resource(KryptonUninstaller "Krypton Uninstaller" "Remove Krypton while preserving browser profiles")

add_executable(KryptonUpdater WIN32
  src/windows/cpp/updater_main.cpp
  src/windows/cpp/helper_resources.rc
  src/windows/manifest/krypton_updater.manifest)
target_include_directories(KryptonUpdater PRIVATE src/windows/include)
target_link_libraries(KryptonUpdater PRIVATE kr_updater_common)
kr_apply_compile_options(KryptonUpdater)
kr_add_version_resource(KryptonUpdater "Krypton Updater" "Signed GitHub release updates for the Krypton browser")
kr_embed_zucchini(KryptonUpdater)

# --- kr_ui_preview.exe (Standalone interactive UI preview) --------------------
add_executable(kr_ui_preview WIN32
  src/windows/cpp/ui_preview_main.cpp
  src/windows/cpp/ui/winui_installer_window.cpp
  src/windows/cpp/resources.rc
  src/windows/manifest/krypton_updater.manifest
)
target_link_libraries(kr_ui_preview PRIVATE kr_updater_common)
kr_apply_compile_options(kr_ui_preview)
kr_add_version_resource(kr_ui_preview "Krypton Setup Preview" "Simulated Krypton setup and uninstall interface")

if(KR_OFFLINE_PAYLOAD AND NOT EXISTS "${KR_OFFLINE_PAYLOAD}")
  message(FATAL_ERROR "Offline payload does not exist: ${KR_OFFLINE_PAYLOAD}")
endif()
if(KR_OFFLINE_PAYLOAD)
  message(STATUS "Embedding offline payload: ${KR_OFFLINE_PAYLOAD}")
  file(TO_CMAKE_PATH "${KR_OFFLINE_PAYLOAD}" KR_PAYLOAD_ESCAPED)
  target_compile_definitions(KryptonSetup PRIVATE
    KR_EMBED_PAYLOAD_FILE=\"${KR_PAYLOAD_ESCAPED}\")
endif()

if(KR_OFFLINE_MANIFEST)
  if(NOT EXISTS "${KR_OFFLINE_MANIFEST}")
    message(FATAL_ERROR "Offline manifest does not exist: ${KR_OFFLINE_MANIFEST}")
  endif()
  file(TO_CMAKE_PATH "${KR_OFFLINE_MANIFEST}" KR_MANIFEST_ESCAPED)
  target_compile_definitions(KryptonSetup PRIVATE
    KR_EMBED_MANIFEST_FILE=\"${KR_MANIFEST_ESCAPED}\")
endif()

if(KR_OFFLINE_SIGNATURE)
  if(NOT EXISTS "${KR_OFFLINE_SIGNATURE}")
    message(FATAL_ERROR "Offline signature does not exist: ${KR_OFFLINE_SIGNATURE}")
  endif()
  file(TO_CMAKE_PATH "${KR_OFFLINE_SIGNATURE}" KR_SIGNATURE_ESCAPED)
  target_compile_definitions(KryptonSetup PRIVATE KR_EMBED_SIGNATURE_FILE=\"${KR_SIGNATURE_ESCAPED}\")
endif()

# --- krypton.exe (Lightweight root launcher stub ~50 KB) -----------------------
add_executable(krypton WIN32
  src/windows/cpp/helper_resources.rc
  src/common/cpp/compatibility.cpp
  src/windows/cpp/security/os_capability.cpp
  src/windows/cpp/launcher_main.cpp
  src/windows/cpp/version_manager.cpp
  src/windows/cpp/security/authenticode_verifier.cpp
  src/common/cpp/semver.cpp
)
target_include_directories(krypton PRIVATE
  src/common/include
  src/windows/include
)
target_link_libraries(krypton PRIVATE
  shell32.lib
  user32.lib
  wintrust.lib
  crypt32.lib
)
kr_apply_compile_options(krypton)
kr_add_version_resource(krypton "Krypton Launcher" "Krypton legacy browser launcher")

# --- Automated Unit Tests -----------------------------------------------------
if(KR_BUILD_TESTS)
  enable_testing()

  add_executable(semver_test tests/semver_test.cpp src/common/cpp/semver.cpp)
  target_include_directories(semver_test PRIVATE src/common/include)
  kr_apply_compile_options(semver_test)
  add_test(NAME semver COMMAND semver_test)

  add_executable(manifest_verifier_test
    tests/manifest_verifier_test.cpp
    src/common/cpp/manifest_verifier.cpp
    src/common/cpp/semver.cpp)
  target_include_directories(manifest_verifier_test PRIVATE src/common/include)
  kr_apply_compile_options(manifest_verifier_test)
  add_test(NAME manifest_verifier COMMAND manifest_verifier_test)

  add_executable(path_sanitizer_test
    tests/path_sanitizer_test.cpp
    src/windows/cpp/security/path_sanitizer.cpp)
  target_include_directories(path_sanitizer_test PRIVATE
    src/common/include
    src/windows/include)
  kr_apply_compile_options(path_sanitizer_test)
  add_test(NAME path_sanitizer COMMAND path_sanitizer_test)

  add_executable(thread_pool_test
    tests/thread_pool_test.cpp
    src/common/cpp/thread_pool.cpp)
  target_include_directories(thread_pool_test PRIVATE src/common/include)
  kr_apply_compile_options(thread_pool_test)
  add_test(NAME thread_pool COMMAND thread_pool_test)

  add_executable(archive_extractor_test
    tests/archive_extractor_test.cpp)
  target_include_directories(archive_extractor_test PRIVATE
    src/common/include
    src/windows/include)
  target_link_libraries(archive_extractor_test PRIVATE kr_updater_common)
  kr_apply_compile_options(archive_extractor_test)
  add_test(NAME archive_extractor COMMAND archive_extractor_test)

  add_executable(offline_engine_test tests/offline_engine_test.cpp)
  target_link_libraries(offline_engine_test PRIVATE kr_updater_common)
  target_compile_definitions(offline_engine_test PRIVATE
    KR_OFFLINE_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures\")
  kr_apply_compile_options(offline_engine_test)
  add_test(NAME offline_engine COMMAND offline_engine_test)

  add_executable(prerequisites_test tests/prerequisites_test.cpp)
  target_link_libraries(prerequisites_test PRIVATE kr_updater_common)
  kr_apply_compile_options(prerequisites_test)
  add_test(NAME prerequisites COMMAND prerequisites_test)

  add_executable(compatibility_test tests/compatibility_test.cpp)
  target_link_libraries(compatibility_test PRIVATE kr_updater_common)
  kr_apply_compile_options(compatibility_test)
  add_test(NAME compatibility COMMAND compatibility_test)

  add_executable(native_install_test tests/native_install_test.cpp)
  target_link_libraries(native_install_test PRIVATE kr_updater_common)
  kr_apply_compile_options(native_install_test)
  add_test(NAME native_install COMMAND native_install_test)
endif()
