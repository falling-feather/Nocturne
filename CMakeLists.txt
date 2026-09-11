cmake_minimum_required(VERSION 3.21)

project(Nocturne VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Sql)
include(scripts/MathRuntime.cmake)

qt_add_executable(NocturneAudio WIN32 src/AudioMain.cpp src/VoiceDecode.cpp src/VoiceWave.cpp)
target_link_libraries(NocturneAudio PRIVATE Qt6::Core)
if(WIN32)
    target_link_libraries(NocturneAudio PRIVATE mfplat mfreadwrite mfuuid ole32)
endif()

qt_add_executable(Nocturne WIN32
    src/main.cpp
    src/BackupManager.h
    src/BackupManager.cpp
    src/Branding.h
    src/Branding.cpp
    src/Database.h
    src/Database.cpp
    src/GlobalHotkey.h
    src/GlobalHotkey.cpp
    src/MainWindow.h
    src/MainWindow.cpp
    src/DocumentImporter.h
    src/DocumentImporter.cpp
    src/NotebookTree.h
    src/NotebookTree.cpp
    src/FolderComboBox.h
    src/FolderComboBox.cpp
    src/DocumentOutline.h
    src/DocumentOutline.cpp
    src/NoteEditor.h
    src/NoteEditor.cpp
    src/MathSupport.h
    src/MathSupport.cpp
    src/NocturneDialogs.h
    src/NocturneDialogs.cpp
    src/NocturneStyle.h
    src/NocturneStyle.cpp
    src/StickyNoteWindow.h
    src/StickyNoteWindow.cpp
    src/WindowChrome.h
    src/WindowChrome.cpp
    assets/nocturne.qrc
)

target_link_libraries(Nocturne PRIVATE
    Qt6::Widgets
    Qt6::Sql
    NocturneMathRuntime
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
add_dependencies(Nocturne NocturneAudio)
configure_file(scripts/install-voice.ps1 "${CMAKE_CURRENT_BINARY_DIR}/install-voice.ps1" COPYONLY)
file(COPY scripts/voice-licenses DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")

include(GNUInstallDirs)
install(TARGETS Nocturne NocturneAudio
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(FILES scripts/install-voice.ps1 DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY scripts/voice-licenses DESTINATION ${CMAKE_INSTALL_BINDIR})

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

    qt_add_executable(NocturnePerformanceTest
        tests/DatabasePerformance.cpp
        src/Database.h
        src/Database.cpp
    )
    target_include_directories(NocturnePerformanceTest PRIVATE src)
    target_link_libraries(NocturnePerformanceTest PRIVATE Qt6::Sql)
    add_test(NAME performance_smoke COMMAND NocturnePerformanceTest)
    set_tests_properties(performance_smoke PROPERTIES TIMEOUT 120)

    qt_add_executable(NocturneDesktopUiTest
        tests/DesktopUiSmoke.cpp
        tests/DocumentWorkflow.cpp
        tests/WorkspaceUiWorkflow.cpp
        src/BackupManager.h
        src/BackupManager.cpp
        src/Branding.h
        src/Branding.cpp
        src/Database.h
        src/Database.cpp
        src/GlobalHotkey.h
        src/GlobalHotkey.cpp
        src/MainWindow.h
        src/MainWindow.cpp
        src/DocumentImporter.h
        src/DocumentImporter.cpp
        src/NotebookTree.h
        src/NotebookTree.cpp
        src/FolderComboBox.h
        src/FolderComboBox.cpp
        src/DocumentOutline.h
        src/DocumentOutline.cpp
        src/NoteEditor.h
        src/NoteEditor.cpp
        src/MathSupport.h
        src/MathSupport.cpp
        src/NocturneDialogs.h
        src/NocturneDialogs.cpp
        src/NocturneStyle.h
        src/NocturneStyle.cpp
        src/StickyNoteWindow.h
        src/StickyNoteWindow.cpp
        src/WindowChrome.h
        src/WindowChrome.cpp
        assets/nocturne.qrc
    )
    target_include_directories(NocturneDesktopUiTest PRIVATE src)
    target_link_libraries(NocturneDesktopUiTest PRIVATE Qt6::Widgets Qt6::Sql NocturneMathRuntime)
    if(WIN32)
        target_link_libraries(NocturneDesktopUiTest PRIVATE user32)
        target_compile_definitions(NocturneDesktopUiTest PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
        )
    endif()
    add_test(NAME desktop_ui_smoke
             COMMAND NocturneDesktopUiTest "${CMAKE_CURRENT_BINARY_DIR}/ui-smoke")
    set_tests_properties(desktop_ui_smoke PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

    qt_add_executable(NocturneBackupTest
        tests/BackupSmoke.cpp
        src/BackupManager.h
        src/BackupManager.cpp
        src/Database.h
        src/Database.cpp
    )
    target_include_directories(NocturneBackupTest PRIVATE src)
    target_link_libraries(NocturneBackupTest PRIVATE Qt6::Sql)
    target_compile_definitions(NocturneBackupTest PRIVATE
        NOCTURNE_VERSION="${PROJECT_VERSION}"
    )
    add_test(NAME backup_smoke COMMAND NocturneBackupTest)
    set_tests_properties(backup_smoke PROPERTIES TIMEOUT 60)

    qt_add_executable(NocturneWorkspaceTest tests/WorkspaceSmoke.cpp src/Database.h src/Database.cpp)
    target_include_directories(NocturneWorkspaceTest PRIVATE src)
    target_link_libraries(NocturneWorkspaceTest PRIVATE Qt6::Sql)
    add_test(NAME workspace_smoke COMMAND NocturneWorkspaceTest)
    set_tests_properties(workspace_smoke PROPERTIES TIMEOUT 60)
endif()

# These services participate in the same SQLite transactions as note writes.
foreach(nocturne_target Nocturne NocturnePersistenceTest NocturnePerformanceTest NocturneDesktopUiTest NocturneBackupTest NocturneWorkspaceTest)
    if(TARGET ${nocturne_target})
        target_sources(${nocturne_target} PRIVATE src/WorkspaceStore.h src/WorkspaceStore.cpp src/SourceFile.h src/SourceFile.cpp)
    endif()
endforeach()

foreach(nocturne_ui_target Nocturne NocturneDesktopUiTest)
    if(TARGET ${nocturne_ui_target})
    target_sources(${nocturne_ui_target} PRIVATE src/WorkspaceUi.cpp src/HistoryUi.cpp src/FindBar.h src/FindBar.cpp src/TextDiff.h src/TextDiff.cpp)
    target_sources(${nocturne_ui_target} PRIVATE src/AiExchange.h src/AiExchange.cpp src/McpServer.cpp src/AiUi.cpp)
    target_sources(${nocturne_ui_target} PRIVATE src/VoiceAudio.h src/VoiceAudio.cpp src/VoiceWave.cpp src/VoiceUi.cpp)
    if(WIN32)
        target_link_libraries(${nocturne_ui_target} PRIVATE winmm)
    endif()
    target_compile_definitions(${nocturne_ui_target} PRIVATE NOCTURNE_VERSION="${PROJECT_VERSION}")
    endif()
endforeach()
