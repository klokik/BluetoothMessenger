cmake_minimum_required(VERSION 3.8.2)
include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(mesh_badge)

target_sources(app PRIVATE src/main.c src/mesh.c src/periphs.c src/${BOARD}.c)

add_definitions(-DTEAM=$(TEAM) -DNODE=$(NODE))

add_custom_target(invalidate_files ALL COMMAND ${CMAKE_COMMAND} -E touch
		${CMAKE_SOURCE_DIR}/src/main.c ${CMAKE_SOURCE_DIR}/src/mesh.c)
add_dependencies(flash invalidate_files)
