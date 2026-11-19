cmake_minimum_required(VERSION 3.10)
project(dpa_client)

# Configuration
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -DALLOW_EXPERIMENTAL_API -std=c++20 -Wall -w -Wextra -pedantic")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wold-style-cast -Wno-unused-function -Wno-missing-braces -Wno-unused-parameter")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-address-of-packed-member -Wno-deprecated-declarations -Wno-missing-field-initializers -Wno-old-style-cast -Wno-write-strings")

# dpa application
set(DEV_APP_NAME dpa_client_dev)
set(dpacc_build_script ${CMAKE_SOURCE_DIR}/../third_party/dpacc/build_dpacc.sh)

set(dev_program_lib_name ${DEV_APP_NAME})
set(dev_app_build_dir ${CMAKE_BINARY_DIR})
set(dev_program_user_lib)
list(APPEND dev_sources
	${CMAKE_SOURCE_DIR}/dpa_client_dev.c
	${CMAKE_SOURCE_DIR}/../third_party/wrapper_flexio/wrapper_flexio_device.c)
list(APPEND dev_program_lib_include_dir
	${CMAKE_SOURCE_DIR}/../third_party/wrapper_flexio)

add_custom_command(OUTPUT dpacc_${DEV_APP_NAME}
	COMMAND ${dpacc_build_script} --application
		--app_name ${dev_program_lib_name}
		--dpacc_build_dir ${CMAKE_BINARY_DIR}
		--additional_dpacc_options \" --mcpu=nv-dpa-bf3\"
		--external_cc_options "-w,-O3,-funroll-loops,-finline-functions,-finline-limit=1000,-fomit-frame-pointer,-falign-functions=32,-falign-loops=32,-DE_MODE_LE,-ffreestanding,-mabi=lp64,-mno-relax,-mcmodel=medany,-nostdlib,-Wdouble-promotion,-D__DEVICE__"
		--additional_include_directories ${dev_program_lib_include_dir}
		--srcs ${dev_sources}
)

add_custom_target(dpacc_${DEV_APP_NAME}_target ALL
	DEPENDS dpacc_${DEV_APP_NAME}
)


# host application
set(HOST_APP_NAME dpa_client_host)
add_executable(${HOST_APP_NAME}
	main.cpp
	${CMAKE_SOURCE_DIR}/../third_party/wrapper_flexio/wrapper_flexio.cpp
	${CMAKE_SOURCE_DIR}/../common/numautil.cpp
	${CMAKE_SOURCE_DIR}/../common/log.cpp
)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBFLEXIO IMPORTED_TARGET libflexio REQUIRED)
pkg_check_modules(DOCA IMPORTED_TARGET doca-dpa doca-common doca-rdma doca-pcc doca-dma REQUIRED)

add_dependencies(${HOST_APP_NAME} dpacc_${DEV_APP_NAME}_target)

target_include_directories(${HOST_APP_NAME} PRIVATE
	../third_party/wrapper_flexio
	../common
	../
)

target_link_libraries(${HOST_APP_NAME}
	PkgConfig::LIBFLEXIO
	PkgConfig::DOCA
	numa
	ibverbs
	mlx5
	${dev_app_build_dir}/${dev_program_lib_name}.a
)
