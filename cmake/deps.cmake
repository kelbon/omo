include(cmake/get_cpm.cmake)

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
  GIT_TAG v1.2.0
)

# move_only_function, function ref etc (anyanylib)
CPMAddPackage(
  NAME ANYANY
  GITHUB_REPOSITORY kelbon/AnyAny
  GIT_TAG v1.1.0
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
    GIT_TAG v0.9.3
  )
endif()

find_package(Threads REQUIRED)
