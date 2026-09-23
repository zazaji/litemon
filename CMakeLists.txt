cmake_minimum_required(VERSION 3.22)
project(LiteMon VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_AUTORCC OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LITEMON_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(LITEMON_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(LITEMON_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(LITEMON_BUILD_TESTS "Build tests" ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets Sql)

add_library(litemon_project_options INTERFACE)
target_compile_features(litemon_project_options INTERFACE cxx_std_20)
target_compile_options(litemon_project_options INTERFACE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
if(LITEMON_WARNINGS_AS_ERRORS)
    target_compile_options(litemon_project_options INTERFACE $<$<CXX_COMPILER_ID:GNU,Clang>:-Werror>)
endif()
if(LITEMON_ENABLE_ASAN)
    target_compile_options(litemon_project_options INTERFACE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(litemon_project_options INTERFACE -fsanitize=address)
endif()
if(LITEMON_ENABLE_UBSAN)
    target_compile_options(litemon_project_options INTERFACE -fsanitize=undefined -fno-omit-frame-pointer)
    target_link_options(litemon_project_options INTERFACE -fsanitize=undefined)
endif()

add_library(litemon_core STATIC
    src/model.h
    src/apppaths.h src/apppaths.cpp
    src/appconfig.h src/appconfig.cpp
    src/linuxutils.h src/linuxutils.cpp
    src/gpucollector.h src/gpucollector.cpp
    src/collector.h src/collector.cpp
    src/database.h src/database.cpp
    src/diagnostics.h src/diagnostics.cpp
)
target_include_directories(litemon_core PUBLIC src)
target_link_libraries(litemon_core PUBLIC Qt6::Core Qt6::Sql litemon_project_options)
target_compile_definitions(litemon_core PUBLIC LITEMON_VERSION="${PROJECT_VERSION}")

add_executable(litemon-collector src/collector_main.cpp)
target_link_libraries(litemon-collector PRIVATE litemon_core)

add_executable(litemon
    src/gui_main.cpp
    src/chartwidget.h src/chartwidget.cpp
    src/treemaplayout.h
    src/treemapwidget.h src/treemapwidget.cpp
    src/settingsdialog.h src/settingsdialog.cpp
    src/mainwindow.h src/mainwindow.cpp
)
target_link_libraries(litemon PRIVATE litemon_core Qt6::Gui Qt6::Widgets)

include(GNUInstallDirs)
install(TARGETS litemon litemon-collector RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES packaging/io.github.litemon.LiteMon.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES packaging/io.github.litemon.LiteMon.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
install(FILES packaging/io.github.litemon.LiteMon.svg DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
install(FILES packaging/litemon-collector.service DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system)

include(CTest)
if(BUILD_TESTING AND LITEMON_BUILD_TESTS)
    add_executable(test_gpu_parser tests/test_gpu_parser.cpp)
    target_link_libraries(test_gpu_parser PRIVATE litemon_core)
    add_test(NAME gpu_parser COMMAND test_gpu_parser)

    add_executable(test_database tests/test_database.cpp)
    target_link_libraries(test_database PRIVATE litemon_core)
    add_test(NAME database COMMAND test_database)

    add_executable(test_config tests/test_config.cpp)
    target_link_libraries(test_config PRIVATE litemon_core)
    add_test(NAME config COMMAND test_config)

    add_executable(test_collector tests/test_collector.cpp)
    target_link_libraries(test_collector PRIVATE litemon_core)
    add_test(NAME collector COMMAND test_collector)

    add_executable(test_parsers tests/test_parsers.cpp)
    target_link_libraries(test_parsers PRIVATE litemon_core)
    add_test(NAME parsers COMMAND test_parsers)
endif()

set(CPACK_PACKAGE_NAME "litemon")
set(CPACK_PACKAGE_VENDOR "LiteMon contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight local historical system monitor")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "LiteMon contributors")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libqt6sql6-sqlite")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_GENERATOR "TGZ;DEB")
include(CPack)
