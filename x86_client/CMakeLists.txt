cmake_minimum_required(VERSION 3.16)
project(multiclient LANGUAGES CXX)

# Configuration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Setup variables
set(MULTICLIENT_TARGET "test" CACHE STRING "Target implementation to build")
set(COMMON_DIR "${CMAKE_SOURCE_DIR}/common")

# Check target
set(TARGET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/targets/${MULTICLIENT_TARGET}")
if(NOT EXISTS "${TARGET_DIR}")
	message(FATAL_ERROR "Selected target '${MULTICLIENT_TARGET}' does not exist: ${TARGET_DIR}")
endif()

message("Selected TARGET \"${MULTICLIENT_TARGET}\" at ${TARGET_DIR}.")

# Executable
add_executable(multiclient
	main.cpp
	backend.cpp
)

find_package(gflags REQUIRED)
target_link_libraries(multiclient PRIVATE
	gflags
)

target_include_directories(multiclient PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}
	${COMMON_DIR}
	${TARGET_DIR}
)

target_compile_definitions(multiclient PRIVATE
	MULTICLIENT_TARGET="${MULTICLIENT_TARGET}"
)


#
# Target
#

# Rolex client
if(MULTICLIENT_TARGET STREQUAL "rolex")
	set(ROLEX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/rolex")

	find_package(Boost REQUIRED COMPONENTS coroutine)

	if(NOT Boost_COROUTINE_FOUND)
		message(FATAL_ERROR "Failed to find Boost::Coroutine.")
	else()
		target_link_libraries(multiclient PRIVATE
			${Boost_COROUTINE_LIBRARY}
		)
	endif()

	target_sources(multiclient PRIVATE
		${ROLEX_ROOT}/deps/r2/src/logging.cc
		${ROLEX_ROOT}/deps/r2/src/sshed.cc
		${ROLEX_ROOT}/benchs/terminate.cc
		${TARGET_DIR}/target.cpp
	)

	target_include_directories(multiclient PRIVATE
		${ROLEX_ROOT}/..
		${ROLEX_ROOT}/xutils
		${ROLEX_ROOT}
		${ROLEX_ROOT}/deps
		${ROLEX_ROOT}/deps/r2/src
		${ROLEX_ROOT}/deps/rlib
		${ROLEX_ROOT}/xcomm
		${Boost_INCLUDE_DIRS}
	)

	target_link_libraries(multiclient PRIVATE
		gflags
		ibverbs
		pthread
		${Boost_LIBRARIES}
	)

# DPDK client using KVPacket
elseif(MULTICLIENT_TARGET STREQUAL "DPDK")
	find_package(PkgConfig REQUIRED)
	pkg_check_modules(DPDK REQUIRED "libdpdk")

	target_include_directories(multiclient PRIVATE
		${DPDK_INCLUDE_DIRS}
		${CMAKE_SOURCE_DIR}/../common
	)

	target_link_libraries(multiclient PRIVATE
		${DPDK_LINK_LIBRARIES}
	)

	target_sources(multiclient PRIVATE
		${COMMON_DIR}/dpdk.cpp
		${COMMON_DIR}/protocol.cpp
		${TARGET_DIR}/target.cpp
	)

	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mssse3")

else()
	message(FATAL_ERROR "No target selected. Use -DMULTICLIENT_TARGET=[DPDK,rolex].")
endif()
