cmake_minimum_required(VERSION 3.20)
project(BenshiNode VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# ---------------------------------------------------------------------------
# Vendored SBC codec (Apache-2.0, google/libsbc)
# ---------------------------------------------------------------------------
add_library(sbc STATIC
  third_party/sbc/sbc.c
  third_party/sbc/bits.c
)
target_include_directories(sbc
  PUBLIC  third_party/sbc/include
  PRIVATE third_party/sbc
)
if(MSVC)
  target_compile_options(sbc PRIVATE /W3)
endif()

# ---------------------------------------------------------------------------
# GSM 06.10 codec (TU Berlin reference implementation, see COPYRIGHT)
# ---------------------------------------------------------------------------
add_library(gsm STATIC
  third_party/gsm/src/add.c
  third_party/gsm/src/code.c
  third_party/gsm/src/decode.c
  third_party/gsm/src/gsm_create.c
  third_party/gsm/src/gsm_decode.c
  third_party/gsm/src/gsm_destroy.c
  third_party/gsm/src/gsm_encode.c
  third_party/gsm/src/gsm_explode.c
  third_party/gsm/src/gsm_implode.c
  third_party/gsm/src/gsm_option.c
  third_party/gsm/src/long_term.c
  third_party/gsm/src/lpc.c
  third_party/gsm/src/preprocess.c
  third_party/gsm/src/rpe.c
  third_party/gsm/src/short_term.c
  third_party/gsm/src/table.c
)
target_include_directories(gsm PUBLIC third_party/gsm/inc)
if(MSVC)
  target_compile_options(gsm PRIVATE /W3 /wd4244)
endif()

# ---------------------------------------------------------------------------
# Portable core
# ---------------------------------------------------------------------------
add_library(benshinode_core STATIC
  src/Config.cpp
  src/Logger.cpp
  src/radio/GaiaProtocol.cpp
  src/radio/AudioFraming.cpp
  src/radio/SbcCodec.cpp
  src/node/DtmfDecoder.cpp
  src/node/DtmfCommands.cpp
  src/node/ToneGenerator.cpp
  src/node/WavClip.cpp
  src/link/G711.cpp
  src/link/Resampler.cpp
  src/link/LinkProtocol.cpp
  src/echolink/EchoLinkProtocol.cpp
)
target_include_directories(benshinode_core PUBLIC src)
target_link_libraries(benshinode_core PUBLIC sbc)

# ---------------------------------------------------------------------------
# Windows application
# ---------------------------------------------------------------------------
if(WIN32)
  set(_winrt_include "")
  file(GLOB _winrt_dirs "C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt")
  if(_winrt_dirs)
    list(SORT _winrt_dirs)
    list(REVERSE _winrt_dirs)
    list(GET _winrt_dirs 0 _winrt_include)
    message(STATUS "C++/WinRT headers: ${_winrt_include}")
  else()
    message(FATAL_ERROR "C++/WinRT headers not found (Windows SDK required)")
  endif()

  add_executable(BenshiNode
    src/main.cpp
    src/bluetooth/RfcommChannel.cpp
    src/radio/Vr76Audio.cpp
    src/radio/RadioSession.cpp
    src/node/NodeEngine.cpp
    src/util/WavWriter.cpp
    src/link/LinkCrypto.cpp
    src/link/LinkSocket.cpp
    src/link/LinkServer.cpp
    src/link/LinkClient.cpp
    src/echolink/EchoLinkClient.cpp
  )
  target_include_directories(BenshiNode PRIVATE ${_winrt_include})
  target_link_libraries(BenshiNode PRIVATE benshinode_core gsm windowsapp ole32
                                               ws2_32 bcrypt)
  set_target_properties(BenshiNode PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
  )
else()
  message(STATUS "Only the core/test targets are built on non-Windows platforms")
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(BenshiNode_tests tests/core_tests.cpp)
target_link_libraries(BenshiNode_tests PRIVATE benshinode_core gsm)
add_test(NAME BenshiNode_tests COMMAND BenshiNode_tests)
