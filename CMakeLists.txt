cmake_minimum_required(VERSION 3.25)

project(observex
    VERSION 0.3.0
    DESCRIPTION "Reads the screen in real time and reacts like a human player"
    LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "observex is Windows-only: it uses DXGI Desktop Duplication and Direct3D 11.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

option(OBSERVEX_BUILD_TESTS "Build the observex unit tests" ON)

include(cmake/Dependencies.cmake)
include(cmake/TensorRT.cmake)
include(cmake/Nvenc.cmake)

# Warning flags for our own targets only (third-party code is left alone).
add_library(observex_warnings INTERFACE)
if(MSVC)
    target_compile_options(observex_warnings INTERFACE /W4 /permissive- /utf-8)
else()
    target_compile_options(observex_warnings INTERFACE -Wall -Wextra -Wpedantic)
endif()

# Core: platform-free building blocks (lock-free hand-overs, statistics, marks on images, the review's undo history,
# zoom and kind learning, recording files, the live log's look lines and a clip's lines and guesses, what the
# heuristics take and give, the mirror test). Unit tested.
add_library(observex_core STATIC
    src/core/Attacker.cpp
    src/core/BoxLearner.cpp
    src/core/CameraFocus.cpp
    src/core/CameraTurn.cpp
    src/core/ClipGuesses.cpp
    src/core/ClipLines.cpp
    src/core/CloseUp.cpp
    src/core/Costs.cpp
    src/core/Depth.cpp
    src/core/Distances.cpp
    src/core/DropPick.cpp
    src/core/Features.cpp
    src/core/Flee.cpp
    src/core/GameStats.cpp
    src/core/Geodata.cpp
    src/core/Ground.cpp
    src/core/HpWatch.cpp
    src/core/ImageView.cpp
    src/core/KindLearner.cpp
    src/core/LiveLines.cpp
    src/core/LookAround.cpp
    src/core/MouseCamera.cpp
    src/core/Log.cpp
    src/core/LootWatch.cpp
    src/core/Marks.cpp
    src/core/Mirror.cpp
    src/core/ModelInfo.cpp
    src/core/MonsterMemory.cpp
    src/core/Recordings.cpp
    src/core/ReviewHistory.cpp
    src/core/ReviewLines.cpp
    src/core/SkillRefresh.cpp
    src/core/SkillUse.cpp
    src/core/StateClipboard.cpp
    src/core/StateTree.cpp
    src/core/TargetHitWatch.cpp
    src/core/TargetSelect.cpp
    src/core/TargetWatch.cpp
    src/core/Tiles.cpp
    src/core/UiLayout.cpp
    src/core/WorldPosition.cpp
    src/core/XpWatch.cpp)
target_include_directories(observex_core PUBLIC src)
target_link_libraries(observex_core PRIVATE observex_warnings)

# Heuristics (src/heuristics): what observex makes of the model's looks before it shows them, today the tracker and
# the rules around it. Built twice: as observex_rules for the tests, the bench and the app's own fallback, and as
# observex_heuristics.dll, which the app loads and loads again whenever it is rebuilt, so a heuristics change shows
# in the running app: build only that target (`--target observex_heuristics`), as the app's exe is locked while it
# runs. Unit tested (the tracker).
add_library(observex_rules STATIC
    src/heuristics/DepthRules.cpp
    src/heuristics/DepthTerrain.cpp
    src/heuristics/DepthTilt.cpp
    src/heuristics/GroundMotion.cpp
    src/heuristics/KeyLabels.cpp
    src/heuristics/LocationReader.cpp
    src/heuristics/MessageLines.cpp
    src/heuristics/Miniatures.cpp
    src/heuristics/MinimapCone.cpp
    src/heuristics/MinimapScroll.cpp
    src/heuristics/NamePlate.cpp
    src/heuristics/PickupReader.cpp
    src/heuristics/Rules.cpp
    src/heuristics/SnapRules.cpp
    src/heuristics/Snapper.cpp
    src/heuristics/StatsReader.cpp
    src/heuristics/TextOcr.cpp
    src/heuristics/Tracker.cpp
    src/heuristics/TurnPitch.cpp)
# windowsapp: Windows' own OCR (heuristics/TextOcr, through C++/WinRT), for the target's name.
target_link_libraries(observex_rules PUBLIC observex_core windowsapp PRIVATE observex_warnings)
add_library(observex_heuristics SHARED src/heuristics/DepthPlugin.cpp src/heuristics/Plugin.cpp)
target_link_libraries(observex_heuristics PRIVATE observex_rules observex_warnings)
# Next to the app: the Release one with C:\_ai\observex\observex.exe.
set_target_properties(observex_heuristics PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR})
# Reading the game's bars and numbers (heuristics/StatsReader) as observex_stats.dll, a DLL of its own, loaded again
# whenever it is rebuilt likewise (engine/StatsLibrary; `--target observex_stats`).
add_library(observex_stats SHARED src/heuristics/StatsPlugin.cpp)
target_link_libraries(observex_stats PRIVATE observex_rules observex_warnings)
set_target_properties(observex_stats PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR})

# Engine: the real-time loop (screen capture on its own thread and GPU device), the recorder, the recogniser (a
# trained model on the GPU through TensorRT), its live log while recording, the clip buffer (the last 10 s through
# the GPU's video encoder) and the clip player (Media Foundation), the finder (the same model over a recording), the
# kind update after a review and the refresher that makes old recordings' guesses fresh for it. No UI code.
add_library(observex_engine STATIC
    src/engine/BoxSnapper.cpp
    src/engine/ClipPlayer.cpp
    src/engine/ClipDrops.cpp
    src/engine/ClipGround.cpp
    src/engine/ClipRecorder.cpp
    src/engine/ClipSnapper.cpp
    src/engine/CrashLog.cpp
    src/engine/DepthFinder.cpp
    src/engine/DepthNet.cpp
    src/engine/DepthStore.cpp
    src/engine/Detector.cpp
    src/engine/DropFinder.cpp
    src/engine/Finder.cpp
    src/engine/Gpu.cpp
    src/engine/GreyMaker.cpp
    src/engine/GroundFinder.cpp
    src/engine/HeuristicsLibrary.cpp
    src/engine/ImageChannel.cpp
    src/engine/ImageFile.cpp
    src/engine/KindUpdater.cpp
    src/engine/LiveLog.cpp
    src/engine/MouseWatch.cpp
    src/engine/PicoController.cpp
    src/engine/Pipeline.cpp
    src/engine/Recognizer.cpp
    src/engine/Recorder.cpp
    src/engine/Refresher.cpp
    src/engine/ScreenCapture.cpp
    src/engine/StatsLibrary.cpp
    src/engine/TensorRt.cpp
    src/engine/UiFinder.cpp)
target_compile_definitions(observex_engine PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(observex_engine
    PUBLIC observex_core observex_rules d3d11 dxgi
    PRIVATE windowscodecs d3dcompiler cabinet mfplat mfreadwrite mfuuid setupapi advapi32 dbghelp observex_nvenc observex_tensorrt observex_warnings)

# App: the window and the UI. The icon and fonts are built into the exe (see src/app/observex.rc.in).
set(OBSERVEX_SOURCE_DIR ${CMAKE_SOURCE_DIR})
configure_file(src/app/observex.rc.in ${CMAKE_BINARY_DIR}/observex.rc @ONLY)
add_executable(observex
    src/app/App.cpp
    src/app/ClipView.cpp
    src/app/GeodataView.cpp
    src/app/DebugView.cpp
    src/app/LogView.cpp
    src/app/MainWindow.cpp
    src/app/MouseTest.cpp
    src/app/Overlay.cpp
    src/app/PicoLink.cpp
    src/app/Review.cpp
    src/app/ReviewLog.cpp
    src/app/ScreenImage.cpp
    src/app/ScreenPicker.cpp
    src/app/Settings.cpp
    src/app/HotKey.cpp
    src/app/ShotHook.cpp
    src/app/Snapshot.cpp
    src/app/StateTreeView.cpp
    src/app/SkillsView.cpp
    src/app/StatsPanel.cpp
    src/app/Theme.cpp
    src/app/Thumbnails.cpp
    src/app/Widgets.cpp
    src/app/main.cpp
    ${CMAKE_BINARY_DIR}/observex.rc)
target_link_libraries(observex PRIVATE observex_engine imgui dwmapi cfgmgr32 observex_warnings)
add_dependencies(observex observex_heuristics observex_stats) # Building the app builds the DLLs it loads.

# Debug builds keep a console window for stderr; other configs are a plain windowed app.
# The Release build goes straight into the project folder, so C:\_ai\observex\observex.exe is always the newest.
set_target_properties(observex PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}
    WIN32_EXECUTABLE $<NOT:$<CONFIG:Debug>>
    VS_DEBUGGER_WORKING_DIRECTORY $<TARGET_FILE_DIR:observex>)
if(MSVC)
    # Windowed builds still start at main() instead of WinMain().
    target_link_options(observex PRIVATE $<$<NOT:$<CONFIG:Debug>>:/ENTRY:mainCRTStartup>)
endif()
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT observex)

# Bench: measures the pipeline from a console, without the app window (see bench/Bench.cpp).
add_executable(observex_bench bench/Bench.cpp bench/DepthBench.cpp bench/DistanceBench.cpp bench/GeodataBench.cpp
                              bench/StatsBench.cpp bench/TurnsBench.cpp)
target_link_libraries(observex_bench PRIVATE observex_engine dwmapi observex_warnings)
add_dependencies(observex_bench observex_heuristics) # For --heuristics.

# Release keeps PDBs next to what it builds (observex.pdb and the DLLs' in the project folder), so a crash's call chain
# in observex.log names our functions and lines (engine/CrashLog); it optimises as before.
if(MSVC)
    foreach(target observex_core observex_rules observex_engine observex_heuristics observex_stats observex
                   observex_bench)
        set_property(TARGET ${target} PROPERTY
            MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo,Release>:ProgramDatabase>")
    endforeach()
    foreach(target observex_heuristics observex_stats observex observex_bench)
        target_link_options(${target} PRIVATE
            $<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
    endforeach()
endif()

if(OBSERVEX_BUILD_TESTS)
    enable_testing()
    add_executable(observex_tests
        tests/AttackerTests.cpp
        tests/BoxLearnerTests.cpp
        tests/CameraFocusTests.cpp
        tests/ClipGuessesTests.cpp
        tests/ClipLinesTests.cpp
        tests/CloseUpTests.cpp
        tests/CostsTests.cpp
        tests/DepthTerrainTests.cpp
        tests/DepthTests.cpp
        tests/DistancesTests.cpp
        tests/DropPickTests.cpp
        tests/FeaturesTests.cpp
        tests/FleeTests.cpp
        tests/GameStatsTests.cpp
        tests/GeodataTests.cpp
        tests/GroundTests.cpp
        tests/HpWatchTests.cpp
        tests/ImageViewTests.cpp
        tests/KeyLabelsTests.cpp
        tests/KindLearnerTests.cpp
        tests/LiveLinesTests.cpp
        tests/LogTests.cpp
        tests/LookAroundTests.cpp
        tests/LootWatchTests.cpp
        tests/MarksTests.cpp
        tests/MirrorTests.cpp
        tests/MonsterMemoryTests.cpp
        tests/MouseCameraTests.cpp
        tests/ModelInfoTests.cpp
        tests/PickupTests.cpp
        tests/QuestionTests.cpp
        tests/RecordingsTests.cpp
        tests/ReviewHistoryTests.cpp
        tests/ReviewLinesTests.cpp
        tests/ShotKeyTests.cpp
        tests/SkillRefreshTests.cpp
        tests/SkillUseTests.cpp
        tests/StatWindowTests.cpp
        tests/SnapRulesTests.cpp
        tests/SnapperTests.cpp
        tests/StateClipboardTests.cpp
        tests/StateTreeTests.cpp
        tests/TargetHitWatchTests.cpp
        tests/TargetSelectTests.cpp
        tests/TargetWatchTests.cpp
        tests/TilesTests.cpp
        tests/TrackerTests.cpp
        tests/TripleBufferTests.cpp
        tests/TurnPitchTests.cpp
        tests/UiLayoutTests.cpp
        tests/test_main.cpp)
    target_link_libraries(observex_tests PRIVATE observex_rules doctest::doctest observex_warnings)
    add_test(NAME observex_tests COMMAND observex_tests)
endif()
