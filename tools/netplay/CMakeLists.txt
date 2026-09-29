cmake_minimum_required(VERSION 3.16)
project(spaghetti-netplay-tools CXX)
# Standalone build of the netplay relay and its loopback test. Needs no game dependencies:
#   cmake -S tools/netplay -B build-netplay && cmake --build build-netplay
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(NP ${CMAKE_CURRENT_SOURCE_DIR}/../../src/enhancements/netplay)
set(NP_CORE ${NP}/NetplaySocket.cpp ${NP}/NetplayServer.cpp ${NP}/NetplayClient.cpp)
find_package(Threads REQUIRED)

add_executable(spaghetti-netplay-relay RelayMain.cpp RoomRelay.cpp ${NP_CORE})
add_executable(netplay-loopback-test NetplayLoopbackTest.cpp RoomRelay.cpp ${NP_CORE})
foreach(t spaghetti-netplay-relay netplay-loopback-test)
  target_include_directories(${t} PRIVATE ${NP} ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(${t} PRIVATE Threads::Threads)
  if(WIN32)
    target_link_libraries(${t} PRIVATE ws2_32)
  endif()
endforeach()
