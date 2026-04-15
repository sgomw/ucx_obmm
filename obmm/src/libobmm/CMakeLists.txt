cmake_minimum_required(VERSION 3.10)

project(OBMM_SO C)

aux_source_directory(${CMAKE_CURRENT_LIST_DIR} LIBOBMM_DIR_SRCS)
add_library(OBMM_SO SHARED ${LIBOBMM_DIR_SRCS})
target_link_libraries(OBMM_SO PRIVATE)

target_include_directories(OBMM_SO PRIVATE ${CMAKE_CURRENT_LIST_DIR})

set(OBMM_NAME obmm)
set_target_properties(OBMM_SO PROPERTIES OUTPUT_NAME ${OBMM_NAME}
    VERSION 1.0.1
	SOVERSION 1)

target_compile_options(OBMM_SO PRIVATE -Wall -Wextra -Wfloat-equal -fno-common -std=gnu99 -fvisibility=hidden
	-Wuninitialized -Wno-error -Wno-error=format -Wundef -Wunused -Wdate-time -Wshadow -Wvla
	-Wdisabled-optimization -Wempty-body -Wignored-qualifiers -Wimplicit-fallthrough=3
	-Wtype-limits -Wshift-negative-value -Wswitch-default -Wframe-larger-than=8192
	-Wshift-overflow=2 -Wwrite-strings -Wmissing-format-attribute -Wformat-nonliteral
	-Wduplicated-cond -Wtrampolines -Wlogical-op -Wsuggest-attribute=format
	-Wduplicated-branches -Wmissing-include-dirs -Wformat-signedness -Wmissing-declarations
	-Wreturn-local-addr -Wredundant-decls -Wfloat-conversion -Wmissing-prototypes
	-Wstrict-prototypes)
if(GCOV)
	# coverage test version
	target_compile_options(OBMM_SO PRIVATE -O0 -fprofile-arcs -ftest-coverage
		-fkeep-inline-functions -fkeep-static-functions)
	target_link_libraries(OBMM_SO PRIVATE gcov)
else()
	# release version
	target_link_options(OBMM_SO PRIVATE -s)
endif()

install(TARGETS OBMM_SO LIBRARY DESTINATION lib OPTIONAL)
