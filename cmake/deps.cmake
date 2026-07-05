include(cmake/get_cpm.cmake)

if (omo_DOWNLOAD_BOOST)
  set(BOOST_INCLUDE_LIBRARIES system json)
  CPMAddPackage(
    NAME BOOST
    VERSION 1.87.0
    URL https://github.com/boostorg/boost/releases/download/boost-1.87.0/boost-1.87.0-cmake.tar.xz
    OPTIONS "BOOST_ENABLE_CMAKE ON"
  )
  unset(BOOST_INCLUDE_LIBRARIES)
endif()

# cmake tools
CPMAddPackage(
  NAME CCOZY
  GITHUB_REPOSITORY kelbon/ccozy
  GIT_TAG v0.8.2
)

include(${CCOZY_SOURCE_DIR}/ccozy_tools.cmake)

# C++20 coroutines (kelcorolib)
CPMAddPackage(
  NAME KELCORO
  GITHUB_REPOSITORY kelbon/kelcoro
  GIT_TAG v1.4.3
)

# move_only_function, function ref etc (anyanylib)
CPMAddPackage(
  NAME ANYANY
  GITHUB_REPOSITORY kelbon/AnyAny
  GIT_TAG v1.2.1
)

# on_scope_exit / on_scope_failure (zallib)
CPMAddPackage(
  NAME LOGIC_GUARDS
  GITHUB_REPOSITORY kelbon/logic_guards
  GIT_TAG        v1.0.0
)

if (omo_ENABLE_TESTING)
  CPMAddPackage(
    NAME MOKO3
    GITHUB_REPOSITORY kelbon/moko3
    GIT_TAG v0.9.4
  )
endif()

find_package(Threads REQUIRED)
