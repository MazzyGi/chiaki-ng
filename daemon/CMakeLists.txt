cmake_minimum_required(VERSION 3.16)
project(chiaki-relay-daemon C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Threads REQUIRED)

add_executable(chiaki-relay
	src/main.c
	src/session.c
	src/net.c
)

target_include_directories(chiaki-relay PRIVATE
	include
	${CMAKE_SOURCE_DIR}/lib/include
	${CMAKE_BINARY_DIR}/lib/include
)

# link against the chiaki-lib built by the root project
target_link_libraries(chiaki-relay PRIVATE chiaki-lib Threads::Threads)
