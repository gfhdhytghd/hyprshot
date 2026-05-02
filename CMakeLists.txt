cmake_minimum_required(VERSION 3.19)
project(hyprshot VERSION "0.1.0")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

include(CTest)
include(GNUInstallDirs)

add_compile_definitions(WLR_USE_UNSTABLE)
add_compile_options(
  -g
  -Wall
  -Wextra
  -Wno-unused-parameter
  -Wno-unused-value
  -Wno-missing-field-initializers
  -Wno-narrowing
  -Wno-pointer-arith
  --no-gnu-unique
)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPR_DEPS REQUIRED hyprland)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets)

add_library(hyprshot SHARED
  src/plugin/main.cpp
  src/plugin/session_launcher.cpp
  src/shared/config.cpp
  src/shared/protocol.cpp
)

target_include_directories(hyprshot PRIVATE src ${HYPR_DEPS_INCLUDE_DIRS})
target_link_directories(hyprshot PRIVATE ${HYPR_DEPS_LIBRARY_DIRS})
target_link_libraries(hyprshot PRIVATE ${HYPR_DEPS_LIBRARIES})

add_executable(hyprshot-ui
  src/ui/main.cpp
  src/ui/capture_overlay.cpp
  src/ui/result_thumbnail.cpp
  src/shared/config.cpp
  src/shared/protocol.cpp
)

target_include_directories(hyprshot-ui PRIVATE src)
target_link_libraries(hyprshot-ui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)

if(BUILD_TESTING)
  add_executable(hyprshot-config-test
    tests/config_test.cpp
    src/shared/config.cpp
    src/shared/protocol.cpp
  )
  target_include_directories(hyprshot-config-test PRIVATE src)
  add_test(NAME hyprshot-config-test COMMAND hyprshot-config-test)
endif()

install(TARGETS hyprshot LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS hyprshot-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
