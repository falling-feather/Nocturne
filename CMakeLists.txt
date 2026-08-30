cmake_minimum_required(VERSION 3.21)

project(FeatherNote VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Sql)

qt_add_executable(FeatherNote WIN32
    src/main.cpp
    src/Database.h
    src/Database.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/NoteEditor.h
    src/NoteEditor.cpp
    src/StickyNoteWindow.h
    src/StickyNoteWindow.cpp
)

target_link_libraries(FeatherNote PRIVATE
    Qt6::Widgets
    Qt6::Sql
)

if(WIN32)
    target_link_libraries(FeatherNote PRIVATE user32)
    target_compile_definitions(FeatherNote PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
endif()

target_compile_definitions(FeatherNote PRIVATE
    FEATHERNOTE_VERSION="${PROJECT_VERSION}"
)

set_target_properties(FeatherNote PROPERTIES
    OUTPUT_NAME "FeatherNote"
)

include(GNUInstallDirs)
install(TARGETS FeatherNote
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

include(CTest)
if(BUILD_TESTING)
    qt_add_executable(FeatherNotePersistenceTest
        tests/DatabaseSmoke.cpp
        src/Database.h
        src/Database.cpp
    )
    target_include_directories(FeatherNotePersistenceTest PRIVATE src)
    target_link_libraries(FeatherNotePersistenceTest PRIVATE Qt6::Sql)
    add_test(NAME persistence_smoke COMMAND FeatherNotePersistenceTest)
endif()
