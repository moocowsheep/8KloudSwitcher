cmake_minimum_required(VERSION 3.25)
project(KloudSwitcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(Threads REQUIRED)
find_package(Vulkan REQUIRED)

add_library(kloud_core STATIC
  src/core/MediaClock.cpp
  src/core/Stats.cpp
  src/engine/SwitcherCore.cpp
  src/audio/MixerCore.cpp
  src/audio/AudioEngine.cpp
  src/core/Ini.cpp
  src/core/Json.cpp
  src/core/SrtUrl.cpp
  src/ctl/ControlProtocol.cpp
)
target_include_directories(kloud_core PUBLIC src)
target_link_libraries(kloud_core PUBLIC Threads::Threads)
target_compile_options(kloud_core PRIVATE -Wall -Wextra)

# -- shaders: GLSL -> SPIR-V (glslc) -> embedded C arrays --
find_program(GLSLC glslc REQUIRED)
set(KLOUD_SHADER_HDRS "")
function(kloud_shader name src)
  set(spv ${CMAKE_BINARY_DIR}/shaders/${name}.spv)
  set(hdr ${CMAKE_BINARY_DIR}/shaders/${name}.spv.h)
  add_custom_command(OUTPUT ${hdr}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/shaders
    COMMAND ${GLSLC} -O --target-env=vulkan1.3 -I ${CMAKE_SOURCE_DIR}/shaders
            -o ${spv} ${CMAKE_SOURCE_DIR}/shaders/${src}
    COMMAND ${CMAKE_COMMAND} -DIN=${spv} -DOUT=${hdr} -DNAME=${name}
            -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${src} ${CMAKE_SOURCE_DIR}/shaders/ycbcr.glsl
            ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    VERBATIM)
  list(APPEND KLOUD_SHADER_HDRS ${hdr})
  set(KLOUD_SHADER_HDRS ${KLOUD_SHADER_HDRS} PARENT_SCOPE)
endfunction()
kloud_shader(composite_comp composite.comp)
kloud_shader(multiview_tile_comp multiview_tile.comp)
kloud_shader(pack_uyvy_comp pack_uyvy.comp)
kloud_shader(pack_nv12_comp pack_nv12.comp)
kloud_shader(proxy_down_comp proxy_down.comp)
add_custom_target(kloud_shaders DEPENDS ${KLOUD_SHADER_HDRS})

add_library(kloud_gpu STATIC
  src/gpu/VkEngine.cpp
  src/gpu/UploadRing.cpp
  src/gpu/Nv12Ring.cpp
  src/gpu/Compositor.cpp
)
target_link_libraries(kloud_gpu PUBLIC kloud_core Vulkan::Vulkan)
target_include_directories(kloud_gpu PUBLIC ${CMAKE_BINARY_DIR})
target_compile_options(kloud_gpu PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
add_dependencies(kloud_gpu kloud_shaders)

# -- CUDA driver API (libcuda from the driver; headers from the toolkit) --
# Hints cover Debian/Ubuntu multiarch (libcuda from the nvidia driver package,
# cuda.h from nvidia-cuda-toolkit or a /usr/local/cuda runfile install) as well
# as Arch's /opt/cuda and the /usr/lib64 distros.
find_library(CUDA_DRIVER_LIB cuda
  HINTS /usr/lib/${CMAKE_LIBRARY_ARCHITECTURE} /usr/lib /usr/lib64
        /usr/local/cuda/lib64/stubs /opt/cuda/lib64/stubs)
find_path(CUDA_DRIVER_INC cuda.h
  HINTS /usr/local/cuda/include /opt/cuda/include /usr/include)
if(NOT CUDA_DRIVER_LIB OR NOT CUDA_DRIVER_INC)
  message(FATAL_ERROR "CUDA driver API not found (libcuda.so + cuda.h). "
                      "On Debian/Ubuntu install the NVIDIA driver plus "
                      "nvidia-cuda-toolkit (or set CUDA_DRIVER_INC).")
endif()
message(STATUS "CUDA driver: ${CUDA_DRIVER_LIB} (headers: ${CUDA_DRIVER_INC})")
add_library(CudaDriver INTERFACE)
target_link_libraries(CudaDriver INTERFACE ${CUDA_DRIVER_LIB})
target_include_directories(CudaDriver SYSTEM INTERFACE ${CUDA_DRIVER_INC})

find_package(PkgConfig REQUIRED)

# -- FFmpeg. Point KLOUD_FFMPEG_PREFIX at an LGPL build (see
# packaging/build-ffmpeg-lgpl.sh) for anything that gets distributed: a distro
# FFmpeg is configured --enable-gpl, which makes it GPL-2.0-or-later and would
# force this MPL-2.0 project's binaries to be conveyed under the GPL -- which in
# turn does not combine with the proprietary CUDA/NVENC and DeckLink pieces.
set(KLOUD_FFMPEG_PREFIX "" CACHE PATH
    "Prefix of an LGPL FFmpeg build; empty uses the system FFmpeg")
if(KLOUD_FFMPEG_PREFIX)
  get_filename_component(KLOUD_FFMPEG_PREFIX "${KLOUD_FFMPEG_PREFIX}" ABSOLUTE
                         BASE_DIR ${CMAKE_SOURCE_DIR})
  if(NOT EXISTS ${KLOUD_FFMPEG_PREFIX}/lib/pkgconfig/libavcodec.pc)
    message(FATAL_ERROR
      "KLOUD_FFMPEG_PREFIX=${KLOUD_FFMPEG_PREFIX} has no lib/pkgconfig/libavcodec.pc. "
      "Build one with packaging/build-ffmpeg-lgpl.sh.")
  endif()
  # Search this prefix only, so a system FFmpeg cannot satisfy the check.
  set(ENV{PKG_CONFIG_LIBDIR} ${KLOUD_FFMPEG_PREFIX}/lib/pkgconfig)
  set(KLOUD_FFMPEG_LIBDIR ${KLOUD_FFMPEG_PREFIX}/lib)
endif()

pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
  libavcodec libavformat libavutil libavfilter libswresample libswscale)

pkg_check_modules(FFNVCODEC REQUIRED IMPORTED_TARGET ffnvcodec)

if(KLOUD_FFMPEG_PREFIX)
  unset(ENV{PKG_CONFIG_LIBDIR})
  message(STATUS "FFmpeg: LGPL build at ${KLOUD_FFMPEG_PREFIX} "
                 "(libavcodec ${FFMPEG_libavcodec_VERSION})")
else()
  message(WARNING
    "FFmpeg: using the system build (libavcodec ${FFMPEG_libavcodec_VERSION}). "
    "Distro FFmpeg is configured --enable-gpl, so binaries linking it must be "
    "conveyed under the GPL and cannot be distributed under this project's "
    "MPL-2.0 terms alongside the proprietary CUDA/DeckLink runtimes. Fine for "
    "local development; run packaging/build-ffmpeg-lgpl.sh and set "
    "-DKLOUD_FFMPEG_PREFIX for anything you ship.")
endif()

add_library(kloud_media STATIC
  src/media/CudaCtx.cpp
  src/media/FfmpegNvenc.cpp
  src/media/NvencDirect.cpp
  src/media/VideoEncoder.cpp
  src/media/AacEncoder.cpp
  src/media/AvLog.cpp
)
target_link_libraries(kloud_media PUBLIC kloud_gpu CudaDriver PkgConfig::FFMPEG
                      PkgConfig::FFNVCODEC ${CMAKE_DL_LIBS})
target_compile_options(kloud_media PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

# -- OMT (Open Media Transport): optional, vendored in third_party/omt.
# Build once with third_party/{libvmx,libomt} (see docs/omt.md); libomt.so is
# a NativeAOT build that dlopens libvmx.so from the same directory.
set(OMT_SDK_DIR ${CMAKE_SOURCE_DIR}/third_party/omt CACHE PATH "OMT SDK root")
if(EXISTS ${OMT_SDK_DIR}/include/libomt.h AND EXISTS ${OMT_SDK_DIR}/lib/libomt.so)
  set(OMT_FOUND TRUE)
  add_library(OMT::omt SHARED IMPORTED)
  set_target_properties(OMT::omt PROPERTIES
    IMPORTED_LOCATION ${OMT_SDK_DIR}/lib/libomt.so
    INTERFACE_INCLUDE_DIRECTORIES ${OMT_SDK_DIR}/include
    # libomt.so carries no SONAME. Without this, CMake links it by absolute
    # path and that build-tree path lands in DT_NEEDED, so an installed or
    # relocated binary looks for the library where it was compiled. Setting
    # this makes the link go through -L/-l, leaving DT_NEEDED as "libomt.so"
    # for the RPATH to resolve.
    IMPORTED_NO_SONAME TRUE)
  set(_omt_libdir ${OMT_SDK_DIR}/lib)
  message(STATUS "OMT SDK found: ${OMT_SDK_DIR}")
  # libomt-c extension: pre-built decoder pool (the stock NativeAOT libomt
  # lacks it). Both the header and the library must have it, so a header
  # copied over an older .so cannot produce an unresolved symbol at link.
  include(CheckCXXSymbolExists)
  include(CheckLibraryExists)
  # Re-probe every configure: the SDK directory can be swapped underneath a
  # cached result.
  unset(KLOUD_OMT_HDR_PREWARM CACHE)
  unset(KLOUD_OMT_LIB_PREWARM CACHE)
  set(CMAKE_REQUIRED_INCLUDES ${OMT_SDK_DIR}/include)
  set(CMAKE_REQUIRED_LIBRARIES ${OMT_SDK_DIR}/lib/libomt.so)
  check_cxx_symbol_exists(omt_video_decoder_prewarm libomt.h KLOUD_OMT_HDR_PREWARM)
  unset(CMAKE_REQUIRED_INCLUDES)
  unset(CMAKE_REQUIRED_LIBRARIES)
  check_library_exists(${OMT_SDK_DIR}/lib/libomt.so omt_video_decoder_prewarm
                       "" KLOUD_OMT_LIB_PREWARM)
  if(KLOUD_OMT_HDR_PREWARM AND KLOUD_OMT_LIB_PREWARM)
    set(OMT_PREWARM TRUE)
    message(STATUS "OMT decoder prewarm: available (libomt-c)")
  else()
    set(OMT_PREWARM FALSE)
    message(STATUS "OMT decoder prewarm: not in this libomt (first frame of "
                   "each connection pays the decoder build)")
  endif()
else()
  set(OMT_FOUND FALSE)
  message(WARNING
    "OMT SDK not found at ${OMT_SDK_DIR} - building WITHOUT OMT input, the "
    "OMT program/clean/multiview senders, and the bench tools. third_party/ is "
    "gitignored, so a fresh clone never has it: run packaging/build-omt.sh "
    "(see docs/omt.md).")
endif()

# -- DeckLink: optional, headers vendored in third_party/decklink/include
# (copied from the Blackmagic DeckLink SDK; see docs/decklink.md). There is no
# link-time library -- DeckLinkAPIDispatch.cpp dlopens libDeckLinkAPI.so from
# the installed Desktop Video driver at runtime.
set(DECKLINK_SDK_DIR ${CMAKE_SOURCE_DIR}/third_party/decklink
    CACHE PATH "DeckLink SDK root (include/ holds the SDK headers)")
if(EXISTS ${DECKLINK_SDK_DIR}/include/DeckLinkAPI.h AND
   EXISTS ${DECKLINK_SDK_DIR}/include/DeckLinkAPIDispatch.cpp)
  set(DECKLINK_FOUND TRUE)
  message(STATUS "DeckLink SDK found: ${DECKLINK_SDK_DIR}")
else()
  set(DECKLINK_FOUND FALSE)
  message(WARNING
    "DeckLink SDK not found at ${DECKLINK_SDK_DIR} - building WITHOUT any SDI "
    "support (DeckLink input and SDI program/clean output). third_party/ is "
    "gitignored, so a fresh clone never has it: see docs/decklink.md.")
endif()

add_library(kloud_engine STATIC
  src/engine/Engine.cpp
  src/ctl/ControlApply.cpp
  src/ctl/ControlServer.cpp
  src/in/SrtInput.cpp
  src/in/StillInput.cpp
  src/out/DeckLinkOutput.cpp
  src/out/FileRecorder.cpp
  src/out/OmtOutput.cpp
  src/out/SrtOutput.cpp
)
target_link_libraries(kloud_engine PUBLIC kloud_gpu kloud_media)
target_compile_options(kloud_engine PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
if(OMT_FOUND)
  target_sources(kloud_engine PRIVATE src/omt/OmtInput.cpp)
  target_compile_definitions(kloud_engine PUBLIC KLOUD_HAVE_OMT)
  if(OMT_PREWARM)
    target_compile_definitions(kloud_engine PUBLIC KLOUD_HAVE_OMT_PREWARM)
  endif()
  target_link_libraries(kloud_engine PUBLIC OMT::omt)
endif()
if(DECKLINK_FOUND)
  target_sources(kloud_engine PRIVATE
    src/decklink/DeckLinkInput.cpp
    ${DECKLINK_SDK_DIR}/include/DeckLinkAPIDispatch.cpp)
  target_compile_definitions(kloud_engine PUBLIC KLOUD_HAVE_DECKLINK)
  target_include_directories(kloud_engine PUBLIC ${DECKLINK_SDK_DIR}/include)
  target_link_libraries(kloud_engine PUBLIC ${CMAKE_DL_LIBS})
  # SDK header, not ours: it trips -Wall/-Wextra and multichar constants.
  set_source_files_properties(${DECKLINK_SDK_DIR}/include/DeckLinkAPIDispatch.cpp
    PROPERTIES COMPILE_OPTIONS "-w")
endif()

set(_kloud_rpath "")
if(OMT_FOUND)
  list(APPEND _kloud_rpath "${_omt_libdir}")
endif()
if(KLOUD_FFMPEG_LIBDIR)
  list(APPEND _kloud_rpath "${KLOUD_FFMPEG_LIBDIR}")
endif()

# Everything not resolvable from the system lands in one private libdir:
# libomt.so (a NativeAOT build that dlopens libvmx.so from its own directory)
# and, when one is used, the LGPL FFmpeg. Installed binaries carry an RPATH to
# it, so a private FFmpeg cannot be shadowed by the distro's.
include(GNUInstallDirs)
set(KLOUD_PRIVATE_LIBDIR ${CMAKE_INSTALL_FULL_LIBDIR}/8kloud-switcher)
set(_kloud_install_rpath "")
if(OMT_FOUND OR KLOUD_FFMPEG_LIBDIR)
  set(_kloud_install_rpath "${KLOUD_PRIVATE_LIBDIR}")
endif()

# Each executable is linked with its final RPATH (BUILD_WITH_INSTALL_RPATH)
# instead of having it rewritten at install time. The rewrite makes CMake pad
# every build-tree RPATH with a trailing separator so the install-time string
# fits, and an empty RPATH element tells ld.so to search the current working
# directory -- a dev binary run from a directory holding a stray
# libavcodec.so.62 would load it. So a dev build carries the build-tree paths
# and is not meant to be installed; a package build (debian/rules) sets
# KLOUD_PRIVATE_RPATH and its executables run only from the installed
# location, or with LD_LIBRARY_PATH pointing at the staged private libdir.
option(KLOUD_PRIVATE_RPATH
  "Link the executables with the installed private libdir as their only RPATH (package builds)"
  OFF)
if(KLOUD_PRIVATE_RPATH)
  set(_kloud_exe_rpath "${_kloud_install_rpath}")
  set(_kloud_tool_rpath "${_kloud_install_rpath}")
else()
  set(_kloud_exe_rpath "${_kloud_rpath}")
  set(_kloud_tool_rpath "${_omt_libdir}")
endif()

# The bench rig talks OMT end to end, so it only exists with the SDK.
if(OMT_FOUND)
  foreach(tool kloud-testgen kloud-latmeter)
    add_executable(${tool} tools/${tool}/main.cpp)
    target_link_libraries(${tool} PRIVATE kloud_core OMT::omt)
    target_include_directories(${tool} PRIVATE tools)
    target_compile_options(${tool} PRIVATE -Wall -Wextra)
    # Locate libomt.so next to the SDK without requiring ldconfig.
    set_target_properties(${tool} PROPERTIES
      INSTALL_RPATH "${_kloud_tool_rpath}" BUILD_WITH_INSTALL_RPATH ON)
  endforeach()
else()
  message(WARNING "OMT SDK not found - kloud-testgen/kloud-latmeter will not "
                  "be built, and OMT input/output will be unavailable.")
endif()

add_executable(kloud-headless tools/kloud-headless/main.cpp)
target_link_libraries(kloud-headless PRIVATE kloud_engine)
target_include_directories(kloud-headless PRIVATE tools)
set_target_properties(kloud-headless PROPERTIES
  INSTALL_RPATH "${_kloud_exe_rpath}" BUILD_WITH_INSTALL_RPATH ON)

# -- web GUI: the console page is embedded into the binary so the installed
# program is self-contained (no web root to locate at runtime).
set(KLOUD_WEB_HDRS "")
function(kloud_web_asset name src)
  set(hdr ${CMAKE_BINARY_DIR}/web/${name}.spv.h)
  add_custom_command(OUTPUT ${hdr}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/web
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_SOURCE_DIR}/web/${src} -DOUT=${hdr}
            -DNAME=${name} -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    DEPENDS ${CMAKE_SOURCE_DIR}/web/${src} ${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake
    VERBATIM)
  list(APPEND KLOUD_WEB_HDRS ${hdr})
  set(KLOUD_WEB_HDRS ${KLOUD_WEB_HDRS} PARENT_SCOPE)
endfunction()
kloud_web_asset(web_index_html index.html)
kloud_web_asset(web_app_js app.js)
kloud_web_asset(web_app_css app.css)
kloud_web_asset(web_logo_svg logo.svg)
add_custom_target(kloud_web_assets DEPENDS ${KLOUD_WEB_HDRS})

add_library(kloud_web STATIC
  src/app/ShowFile.cpp
  src/app/Session.cpp
  src/web/Http.cpp
  src/web/MjpegEncoder.cpp
  src/web/WebServer.cpp
)
target_link_libraries(kloud_web PUBLIC kloud_engine)
target_compile_options(kloud_web PRIVATE -Wall -Wextra)
add_dependencies(kloud_web kloud_web_assets)

add_executable(8kloud-switcher src/app/main.cpp)
target_link_libraries(8kloud-switcher PRIVATE kloud_web)
target_compile_options(8kloud-switcher PRIVATE -Wall -Wextra)
set_target_properties(8kloud-switcher PROPERTIES
  INSTALL_RPATH "${_kloud_exe_rpath}" BUILD_WITH_INSTALL_RPATH ON)

option(KLOUD_BUILD_TESTS "Build unit tests" ON)
if(KLOUD_BUILD_TESTS)
  # Prefer a distro Catch2 (Debian/Ubuntu: catch2 >= 3). Only fall back to
  # fetching upstream when none is installed -- a package build has no network.
  find_package(Catch2 3 QUIET)
  if(Catch2_FOUND)
    message(STATUS "Catch2: system ${Catch2_VERSION}")
  else()
    message(STATUS "Catch2: not installed, fetching v3.8.0")
    include(FetchContent)
    FetchContent_Declare(Catch2
      GIT_REPOSITORY https://github.com/catchorg/Catch2.git
      GIT_TAG v3.8.0
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
  endif()

  enable_testing()
  add_executable(kloud_tests
    tests/clock_test.cpp
    tests/ring_test.cpp
    tests/switcher_core_test.cpp
    tests/framesync_test.cpp
    tests/strip_test.cpp
    tests/audio_test.cpp
    tests/aac_test.cpp
    tests/control_protocol_test.cpp
    tests/decklink_ref_test.cpp
    tests/media_playlist_test.cpp
    tests/ini_test.cpp
    tests/json_test.cpp
    tests/srt_url_test.cpp
    tests/http_test.cpp
    tests/showfile_test.cpp
    tests/web_server_test.cpp
    tests/gpu_composite_test.cpp
    tests/interop_test.cpp
    tests/nvenc_direct_test.cpp
    tests/video_encoder_test.cpp
    tests/srt_ingest_test.cpp
    tests/avlog_test.cpp
  )
  target_link_libraries(kloud_tests PRIVATE kloud_web Catch2::Catch2WithMain)
  target_include_directories(kloud_tests PRIVATE tools)
  target_compile_definitions(kloud_tests PRIVATE
    KLOUD_TEST_FIXTURES="${CMAKE_SOURCE_DIR}/tests/fixtures")
  set_target_properties(kloud_tests PROPERTIES BUILD_RPATH "${_kloud_rpath}")

  include(Catch)
  catch_discover_tests(kloud_tests)

endif()

# ---------------------------------------------------------------------------
# Install rules (used by the Debian package; see debian/)
# ---------------------------------------------------------------------------
install(TARGETS kloud-headless 8kloud-switcher
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(OMT_FOUND)
  foreach(tool kloud-testgen kloud-latmeter)
    install(TARGETS ${tool} RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
  endforeach()
  # libomt.so dlopens libvmx.so from its own directory: keep them together in
  # a private libdir that the installed binaries carry on their RPATH.
  install(FILES ${OMT_SDK_DIR}/lib/libomt.so ${OMT_SDK_DIR}/lib/libvmx.so
          DESTINATION ${KLOUD_PRIVATE_LIBDIR})
endif()

if(KLOUD_FFMPEG_LIBDIR)
  # Ship the LGPL FFmpeg privately rather than depending on the distro's
  # GPL-configured one. Globbing at install time follows the real files and
  # their SONAME symlinks (libavcodec.so.62 -> libavcodec.so.62.11.103); the
  # bare .so development symlinks are left behind.
  file(GLOB _kloud_ffmpeg_libs "${KLOUD_FFMPEG_LIBDIR}/lib*.so.*")
  if(NOT _kloud_ffmpeg_libs)
    message(FATAL_ERROR "No shared libraries in ${KLOUD_FFMPEG_LIBDIR}")
  endif()
  install(FILES ${_kloud_ffmpeg_libs} DESTINATION ${KLOUD_PRIVATE_LIBDIR})
  install(FILES ${CMAKE_SOURCE_DIR}/packaging/build-ffmpeg-lgpl.sh
          DESTINATION ${CMAKE_INSTALL_DOCDIR}
          PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                      GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
endif()
install(FILES ${CMAKE_SOURCE_DIR}/scripts/tuning-sysctl.conf
        DESTINATION ${CMAKE_INSTALL_DOCDIR}/examples)
