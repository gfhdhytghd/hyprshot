cmake_minimum_required(VERSION 3.19)
project(HyprCapture VERSION "0.2.0")

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

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Svg)
find_package(LayerShellQt REQUIRED)

add_library(hyprcapture SHARED
  src/plugin/main.cpp
  src/plugin/artifact_capture.cpp
  src/plugin/session_launcher.cpp
  src/shared/config.cpp
  src/shared/protocol.cpp
)

target_include_directories(hyprcapture PRIVATE src ${HYPR_DEPS_INCLUDE_DIRS})
target_link_directories(hyprcapture PRIVATE ${HYPR_DEPS_LIBRARY_DIRS})
target_link_libraries(hyprcapture PRIVATE ${HYPR_DEPS_LIBRARIES})

add_executable(hyprcapture-ui
  src/ui/main.cpp
  src/ui/clipboard_utils.cpp
  src/ui/capture_overlay.cpp
  src/ui/result_thumbnail.cpp
  src/ui/watermark.cpp
  src/shared/config.cpp
  src/shared/protocol.cpp
)

target_include_directories(hyprcapture-ui PRIVATE src)
target_link_libraries(hyprcapture-ui PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Svg LayerShellQt::Interface)

if(BUILD_TESTING)
  add_executable(hyprcapture-config-test
    tests/config_test.cpp
    src/shared/config.cpp
    src/shared/protocol.cpp
  )
  target_include_directories(hyprcapture-config-test PRIVATE src)
  add_test(NAME hyprcapture-config-test COMMAND hyprcapture-config-test)

  add_executable(hyprcapture-watermark-test
    tests/watermark_test.cpp
    src/ui/watermark.cpp
    src/shared/config.cpp
  )
  target_include_directories(hyprcapture-watermark-test PRIVATE src)
  target_link_libraries(hyprcapture-watermark-test PRIVATE Qt6::Gui Qt6::Svg)
  add_test(NAME hyprcapture-watermark-test COMMAND hyprcapture-watermark-test)

  add_executable(hyprcapture-security-test
    tests/security_test.cpp
    src/ui/clipboard_utils.cpp
  )
  target_include_directories(hyprcapture-security-test PRIVATE src)
  target_link_libraries(hyprcapture-security-test PRIVATE Qt6::Core Qt6::Gui)
  add_test(NAME hyprcapture-security-test COMMAND hyprcapture-security-test)
endif()

install(TARGETS hyprcapture LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(TARGETS hyprcapture-ui RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
