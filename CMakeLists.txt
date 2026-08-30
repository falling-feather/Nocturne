cmake_minimum_required(VERSION 3.21)

project(Nocturne VERSION 0.1.2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Sql)

qt_add_executable(Nocturne WIN32
    src/main.cpp
    src/Branding.h
    src/Branding.cpp
    src/Database.h
    src/Database.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/NoteEditor.h
    src/NoteEditor.cpp
    src/StickyNoteWindow.h
    src/StickyNoteWindow.cpp
    assets/nocturne.qrc
)

target_link_libraries(Nocturne PRIVATE
    Qt6::Widgets
    Qt6::Sql
)

if(WIN32)
    # windres 无法可靠处理中文源码路径；把 RC 与 ICO 复制到纯英文构建目录再编译。
    set(NOCTURNE_RESOURCE_DIR "${CMAKE_CURRENT_BINARY_DIR}/branding")
    file(MAKE_DIRECTORY "${NOCTURNE_RESOURCE_DIR}")
    configure_file(assets/nocturne.rc "${NOCTURNE_RESOURCE_DIR}/nocturne.rc" @ONLY)
    configure_file(assets/nocturne.ico "${NOCTURNE_RESOURCE_DIR}/nocturne.ico" COPYONLY)
    target_sources(Nocturne PRIVATE "${NOCTURNE_RESOURCE_DIR}/nocturne.rc")
    target_link_libraries(Nocturne PRIVATE user32)
    target_compile_definitions(Nocturne PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()

target_compile_definitions(Nocturne PRIVATE
    NOCTURNE_VERSION="${PROJECT_VERSION}"
)

set_target_properties(Nocturne PROPERTIES
    OUTPUT_NAME "Nocturne"
)

include(GNUInstallDirs)
install(TARGETS Nocturne
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(NocturnePersistenceTest
        tests/DatabaseSmoke.cpp
        src/Database.h
        src/Database.cpp
    )
    target_include_directories(NocturnePersistenceTest PRIVATE src)
    target_link_libraries(NocturnePersistenceTest PRIVATE Qt6::Sql)
    add_test(NAME persistence_smoke COMMAND NocturnePersistenceTest)
endif()
