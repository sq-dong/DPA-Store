cmake_minimum_required(VERSION 3.2)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(xxx)
ADD_DEFINITIONS(-std=c++17)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -O0")

# directories
include_directories("./")
include_directories("deps")

## tests
include(benchs/Rolex/tests/tests.cmake)
enable_testing()

add_test(NAME test COMMAND coretest)
add_custom_target(check COMMAND ${CMAKE_CTEST_COMMAND} --verbose
                  DEPENDS coretest )

set(LOG_SRC "./deps/r2/src/logging.cc"  "./deps/r2/src/sshed.cc"  "./benchs/terminate.cc")

# rolex
file(GLOB rolex_SORUCES ""  "./benchs/Rolex/rolex.cc"  "./deps/r2/src/logging.cc"  "./deps/r2/src/sshed.cc"  "./benchs/terminate.cc" )
add_executable(rolex_local ${rolex_SORUCES} )
target_link_libraries(rolex_local gflags ibverbs pthread boost_system boost_coroutine)

add_executable(rolex_server "server.cpp"   "./deps/r2/src/logging.cc"  "./deps/r2/src/sshed.cc"  "./benchs/terminate.cc" )
target_link_libraries(rolex_server gflags ibverbs pthread boost_system boost_coroutine)

add_executable(rolex_client "client.cpp"   "./deps/r2/src/logging.cc"  "./deps/r2/src/sshed.cc"  "./benchs/terminate.cc" )
target_link_libraries(rolex_client gflags ibverbs pthread boost_system boost_coroutine)
