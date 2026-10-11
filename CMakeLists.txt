cmake_minimum_required(VERSION 3.21)
project(linmtr VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Defines BUILD_TESTING (ON by default) and enables ctest.
include(CTest)

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets Network)
qt_standard_project_setup()
# qt_standard_project_setup() adds a $ORIGIN install RPATH for bundled shared
# libraries. LinMTR installs none and links the system Qt, so drop it.
set(CMAKE_INSTALL_RPATH "")

file(GLOB_RECURSE LINMTR_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/src/*.h
)
list(REMOVE_ITEM LINMTR_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp)

# The library holds all code except main(), so that the tests link the same
# objects as the binary.
qt_add_library(linmtr_lib STATIC ${LINMTR_SOURCES})
target_include_directories(linmtr_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(linmtr_lib PUBLIC Qt6::Widgets Qt6::Network)
target_compile_definitions(linmtr_lib PUBLIC LINMTR_VERSION="${PROJECT_VERSION}")
target_compile_options(linmtr_lib PUBLIC -Wall -Wextra)

qt_add_executable(linmtr src/main.cpp)
target_link_libraries(linmtr PRIVATE linmtr_lib)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS linmtr RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/linmtr.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/icons/linmtr.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/linmtr.1 DESTINATION ${CMAKE_INSTALL_MANDIR}/man1)
