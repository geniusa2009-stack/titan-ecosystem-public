# TITAN Studio — native Qt 6 / QML desktop editor.
# Uses only LGPL-3.0-available Qt modules (Core, Gui, Network, Qml, Quick, QuickControls2)
# at runtime. Qt Shader Tools is used at build time only (to bake shaders); see
# docs/THIRD_PARTY_DEPENDENCIES.md. Qt Quick 3D (GPL/commercial) is intentionally NOT used.
# QRhi is semi-public API (GuiPrivate); the version coupling is a documented, accepted risk
# (docs/LICENSE_AND_RIGHTS_MATRIX.md §3), so Qt's generic warning is acknowledged here.
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui GuiPrivate Network Qml Quick QuickControls2 ShaderTools)
qt_standard_project_setup(REQUIRES 6.7)
qt_policy(SET QTP0004 NEW)  # generate qmldir files for qml/components

qt_add_executable(titan-studio
  src/main.cpp
  src/AppSettings.cpp src/AppSettings.h
  src/EngineClient.cpp src/EngineClient.h
  src/EngineConnection.cpp src/EngineConnection.h
  src/RemoteFrameSource.cpp src/RemoteFrameSource.h
  src/SceneModel.cpp src/SceneModel.h
  src/ViewportItem.cpp src/ViewportItem.h
  src/SmokeTest.cpp src/SmokeTest.h)

set_source_files_properties(qml/Theme.qml qml/Strings.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(titan-studio
  URI TitanStudio
  VERSION 1.0
  QML_FILES
    qml/Main.qml
    qml/Theme.qml
    qml/Strings.qml
    qml/Launcher.qml
    qml/EditorShell.qml
    qml/Hierarchy.qml
    qml/HistoryPopup.qml
    qml/Inspector.qml
    qml/WorldBuilderPanel.qml
    qml/PerformanceWorkspace.qml
    qml/ExtensionsWorkspace.qml
    qml/AssetsWorkspace.qml
    qml/PlaceholderWorkspace.qml
    qml/CommandPalette.qml
    qml/NewProjectDialog.qml
    qml/components/TButton.qml
    qml/components/TIconButton.qml
    qml/components/TTextField.qml
    qml/components/NumberField.qml
    qml/components/Vec3Editor.qml
    qml/components/SectionHeader.qml
    qml/components/Segmented.qml
    qml/components/Glyph.qml)

qt_add_shaders(titan-studio "titan_shaders"
  PREFIX "/"
  FILES shaders/mesh.vert shaders/mesh.frag shaders/frame.vert shaders/frame.frag)

target_link_libraries(titan-studio PRIVATE
  titan_core Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Network Qt6::Qml Qt6::Quick Qt6::QuickControls2)
# TitanFrameChannel.h is shared with the Unreal bridge (single definition of the frame layout).
target_include_directories(titan-studio PRIVATE src ${TITAN_BRIDGE_PUBLIC_DIR})
target_compile_definitions(titan-studio PRIVATE
  TITAN_DEV_HOST_PATH="$<TARGET_FILE:titan-engine-host>"
  TITAN_DEV_EXTENSIONS_DIR="${TITAN_EXTENSIONS_DIR}"
  TITAN_DEV_UNREAL_EDITOR="${TITAN_UE_EDITOR_EXE}"
  TITAN_DEV_UNREAL_PROJECT="${TITAN_UE_HOST_PROJECT}")
target_compile_options(titan-studio PRIVATE ${TITAN_WARNINGS})
add_dependencies(titan-studio titan-engine-host titan_ext_procedural_scatter)

set_target_properties(titan-studio PROPERTIES
  OUTPUT_NAME "TITAN Studio"
  MACOSX_BUNDLE ON
  MACOSX_BUNDLE_BUNDLE_NAME "TITAN Studio"
  MACOSX_BUNDLE_GUI_IDENTIFIER "engine.titan.studio"
  MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
  MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
  RUNTIME_OUTPUT_DIRECTORY ${TITAN_RUNTIME_DIR})

if(TITAN_BUILD_TESTS)
  # Reader side of the Unreal frame channel, against a writer that follows the bridge's protocol.
  add_executable(titan_frame_channel_tests ${CMAKE_SOURCE_DIR}/tests/test_frame_channel.cpp src/RemoteFrameSource.cpp)
  target_include_directories(titan_frame_channel_tests PRIVATE src ${TITAN_BRIDGE_PUBLIC_DIR} ${CMAKE_SOURCE_DIR}/tests)
  target_link_libraries(titan_frame_channel_tests PRIVATE Qt6::Core)
  target_compile_options(titan_frame_channel_tests PRIVATE ${TITAN_WARNINGS})
  add_test(NAME studio.frame-channel COMMAND titan_frame_channel_tests)

  add_test(NAME studio.ui-smoke
    COMMAND $<TARGET_FILE:titan-studio> --smoke-test ${CMAKE_BINARY_DIR}/smoke/SmokeProject
            --screenshot-dir ${CMAKE_BINARY_DIR}/smoke --report ${CMAKE_BINARY_DIR}/smoke/smoke-report.json)
  set_tests_properties(studio.ui-smoke PROPERTIES TIMEOUT 120 LABELS gui)
endif()
