cmake_minimum_required(VERSION 3.22)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" CATWHALE_VERSION LIMIT_COUNT 1)
project(CatwhalePet VERSION "${CATWHALE_VERSION}" LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
find_package(Qt6 6.6 REQUIRED COMPONENTS Core Gui Network Quick Widgets)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(catwhale-pet
  src/main.cpp
  src/config.cpp
  src/pet_window.cpp
  src/controller.cpp
  src/panels.cpp
  src/video.cpp
  src/services.cpp
  src/agent_dock.cpp
  src/niri.cpp
)
target_link_libraries(catwhale-pet PRIVATE Qt6::Quick Qt6::Widgets Qt6::Network PkgConfig::FFMPEG)
set_target_properties(catwhale-pet PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)
target_compile_definitions(catwhale-pet PRIVATE
  CATWHALE_VERSION="${PROJECT_VERSION}"
  CATWHALE_BUILD_ASSETS="${CMAKE_CURRENT_SOURCE_DIR}/assets"
  CATWHALE_BUILD_ICON="${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.png"
  CATWHALE_INSTALL_ASSETS="${CMAKE_INSTALL_FULL_DATADIR}/catwhale-pet"
  CATWHALE_RELATIVE_ASSETS="../${CMAKE_INSTALL_DATADIR}/catwhale-pet"
)
target_compile_options(catwhale-pet PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS catwhale-pet RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY assets/video DESTINATION "${CMAKE_INSTALL_DATADIR}/catwhale-pet")
install(FILES assets/states.json assets/director-defaults.json assets/card-preview.png
  assets/icon.png assets/tray.png LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/catwhale-pet")
install(FILES assets/icon.png DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps" RENAME catwhale-pet.png)
install(FILES assets/catwhale-pet.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

if(BUILD_TESTING)
  add_executable(video-test tests/video_test.cpp src/video.cpp)
  target_include_directories(video-test PRIVATE src)
  target_link_libraries(video-test PRIVATE Qt6::Gui PkgConfig::FFMPEG)
  add_test(NAME video-decode COMMAND video-test "${CMAKE_CURRENT_SOURCE_DIR}/assets/video")
  set_tests_properties(video-decode PROPERTIES TIMEOUT 60)
  add_test(NAME native-smoke COMMAND catwhale-pet --smoke-test --renderer=software --decode=cpu --isolated)
  add_test(NAME hardware-fallback-smoke COMMAND catwhale-pet --smoke-test --renderer=software --isolated)
  add_test(NAME settings-smoke COMMAND catwhale-pet
    --smoke-test --open-settings --renderer=software --decode=cpu --isolated)
  set_tests_properties(native-smoke hardware-fallback-smoke settings-smoke PROPERTIES
    TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
