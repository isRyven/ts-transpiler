cmake_minimum_required(VERSION 3.12)
project(ts-transpiler VERSION 0.1.0 LANGUAGES C CXX)

include(deps/CMakeUtils/CMakeUtils.cmake)

add_subdirectory(deps/dumbjs)
target_compile_definitions(dumbjs PRIVATE NOSTACKCHECKS)
add_subdirectory(deps/un7z)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -D_GNU_SOURCE -Wno-unused-parameter")
	if(CMAKE_C_COMPILER_ID MATCHES "Clang")
        set(CMAKE_C_FLAGS  "${CMAKE_C_FLAGS} -Wunreachable-code")
    endif()
	set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -s -flto -Wno-array-bounds -Wno-format-truncation -O2")
	set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -g -ggdb")
endif()

set(OUTPUT_PAK_DIR "${CMAKE_CURRENT_BINARY_DIR}/pak")
set(OUTPUT_PAK_PATH "${OUTPUT_PAK_DIR}/data.7z")
file(MAKE_DIRECTORY ${OUTPUT_PAK_DIR})
file(GLOB_RECURSE assets RELATIVE "${PROJECT_SOURCE_DIR}/assets" "${PROJECT_SOURCE_DIR}/assets/*")
file(COPY "${PROJECT_SOURCE_DIR}/assets/" DESTINATION "${OUTPUT_PAK_DIR}")
execute_process(
    COMMAND ${CMAKE_COMMAND} -E tar "cfv" ${OUTPUT_PAK_PATH} --format=7zip ${assets}
    WORKING_DIRECTORY ${OUTPUT_PAK_DIR}
)

file_intern(${OUTPUT_PAK_PATH} pak_data pak_data_c)

add_executable(tss main.c libstd.c libstd.h ${pak_data_c})
target_link_libraries(tss dumbjs un7z)
