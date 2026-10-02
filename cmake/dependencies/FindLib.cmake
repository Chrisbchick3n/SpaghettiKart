if(WIN32)
  find_package(Ogg CONFIG REQUIRED)
  find_package(Vorbis CONFIG REQUIRED)
elseif(ANDROID)
  # Nothing in the game, libultraship or Torch links against Ogg/Vorbis, and the
  # NDK has no system copy, so don't drag them in. SDL2 comes from
  # libultraship/cmake/dependencies/android.cmake.
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  find_package(Threads REQUIRED)
  find_library(ANDROID_LOG_LIBRARY log REQUIRED)
  find_library(ANDROID_LIBRARY android REQUIRED)
  set(ADDITIONAL_LIBRARY_DEPENDENCIES SDL2::SDL2 Threads::Threads
                                      ${ANDROID_LIBRARY} ${ANDROID_LOG_LIBRARY})
elseif(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  set(ADDITIONAL_LIBRARY_DEPENDENCIES -lglad SDL2::SDL2)
elseif(CMAKE_SYSTEM_NAME STREQUAL "CafeOS")
  set(ADDITIONAL_LIBRARY_DEPENDENCIES "$<$<CONFIG:Debug>:-Wl,--wrap=abort>")
  target_include_directories(${PROJECT_NAME} PRIVATE
                             ${DEVKITPRO}/portlibs/wiiu/include/)
else()
  find_package(Ogg REQUIRED)
  find_package(Vorbis REQUIRED)
endif()

if(NOT CMAKE_SYSTEM_NAME MATCHES "NintendoSwitch|CafeOS" AND NOT ANDROID)
  set(ADDITIONAL_LIBRARY_DEPENDENCIES Ogg::ogg Vorbis::vorbis
                                      Vorbis::vorbisenc Vorbis::vorbisfile)
endif()

# Android's GL comes from libultraship, which picks GLESv3 there; the game
# itself makes no GL calls, so don't pull a second GLES version in alongside it.
if(UNIX AND NOT APPLE AND NOT ANDROID)
  if(USE_OPENGLES)
    find_library(GLESv2_LIBRARY GLESv2 REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${GLESv2_LIBRARY})
  else()
    find_package(OpenGL REQUIRED)
    target_link_libraries(${PROJECT_NAME} PRIVATE OpenGL::GL)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
  find_package(SDL2)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE ${SDL2_INCLUDE_DIRS})

if(NOT USE_OPENGLES)
  target_include_directories(${PROJECT_NAME} PRIVATE ${GLEW_INCLUDE_DIRS})
endif()

target_link_libraries(${PROJECT_NAME}
                      PRIVATE torch ${ADDITIONAL_LIBRARY_DEPENDENCIES})

# Online netplay: sockets on Windows, and a build id so only identical builds can race together.
if(WIN32)
  target_link_libraries(${PROJECT_NAME} PRIVATE ws2_32)
endif()
# Everyone in a session must run identical game code. The id is the exact source commit, so builds of the same
# commit for different systems (Windows, Mac, Linux, Android) can race each other.
execute_process(
  COMMAND git -c safe.directory=${CMAKE_SOURCE_DIR} rev-parse --short=12 HEAD
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  OUTPUT_VARIABLE NETPLAY_COMMIT
  ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NETPLAY_COMMIT STREQUAL "")
  set(NETPLAY_COMMIT "${PROJECT_VERSION}")
endif()
target_compile_definitions(${PROJECT_NAME} PRIVATE NETPLAY_BUILD_ID="SpaghettiKart-${NETPLAY_COMMIT}")
# Don't let the compiler fuse multiply+add into one instruction: ARM chips (phones, Apple Silicon) would then
# compute physics slightly differently from x86 PCs and online races would desync.
if(NOT MSVC)
  target_compile_options(${PROJECT_NAME} PRIVATE -ffp-contract=off)
endif()
# The online server used by "Host a game" / "Join a game" (room codes). "host" or "host:port".
# CI fills it from the NETPLAY_RELAY repository variable; players can still override it in the game.
set(NETPLAY_DEFAULT_RELAY "$ENV{NETPLAY_RELAY}" CACHE STRING "Default netplay relay server address (host or host:port)")
if(NETPLAY_DEFAULT_RELAY)
  target_compile_definitions(${PROJECT_NAME} PRIVATE NETPLAY_DEFAULT_RELAY="${NETPLAY_DEFAULT_RELAY}")
endif()
