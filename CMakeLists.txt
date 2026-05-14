cmake_minimum_required(VERSION 3.20)
project(omo)

option(omo_DOWNLOAD_BOOST "if true, OMO will download boost itself" ON)
option(omo_ENABLE_TESTING "enables testing" OFF)
option(omo_ENABLE_ASAN "build ASAN (for CI)" OFF)
option(omo_ENABLE_UBSAN "build UBSAN (for CI)" OFF)

include(cmake/deps.cmake)

add_library(omolib STATIC
    # add new .cpp files here. GLOB for libraries is bad practice  
    src/stream_parser.cpp
)

target_include_directories(omolib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_link_libraries(omolib PUBLIC
    ${CMAKE_THREAD_LIBS_INIT}
    anyanylib
    kelcorolib
    zallib
    Boost::json
)

set_target_properties(omolib PROPERTIES
	LINKER_LANGUAGE CXX
	CXX_STANDARD 20
	CMAKE_CXX_STANDARD_REQUIRED ON
)

if(omo_ENABLE_TESTING)
	include(CTest)
	add_subdirectory(test)
endif()

# generates launch.json, places compile_commands.json correctly for clangd
ccozy_on_configuration_end_vscode()
