cmake_minimum_required(VERSION 3.16)
project(EsportsLiveServer VERSION 0.1.0 LANGUAGES CXX)

# 禁用编译器扩展并强制C++11，避免本机编过但换环境后依赖更新的语言特性。
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
# 通过导入目标传递FFmpeg头文件和链接参数，不硬编码本机库路径。
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libavfilter libswresample libswscale)
find_package(Threads REQUIRED)

add_library(project_options INTERFACE)
target_compile_options(project_options INTERFACE -Wall -Wextra -Wpedantic)

add_library(esports_common src/common/logger.cpp)
target_include_directories(esports_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(esports_common PUBLIC Threads::Threads project_options)

add_executable(ffmpeg_env_check src/env_check_main.cpp)
target_link_libraries(ffmpeg_env_check PRIVATE esports_common PkgConfig::FFMPEG)

# 核心媒体库供直播与回放程序复用；main只组装参数，媒体处理不依赖命令行工具。
add_library(esports_media
    src/input/stream_input.cpp
    src/output/stream_output.cpp
    src/output/async_output.cpp
    src/recording/recorder.cpp
    src/monitor/metrics.cpp
    src/codec/media_codec.cpp
    src/codec/video_converter.cpp
    src/input/video_source.cpp
    src/mixer/video_mixer.cpp
    src/input/audio_source.cpp
    src/sync/audio_resampler.cpp
    src/mixer/audio_mixer.cpp
    src/manager/live_pipeline.cpp)
target_link_libraries(esports_media PUBLIC esports_common PkgConfig::FFMPEG)
add_library(esports_config src/config/live_config.cpp)
target_link_libraries(esports_config PUBLIC esports_media)
target_include_directories(esports_config SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/picojson)

add_executable(esports_live src/live_main.cpp)
target_link_libraries(esports_live PRIVATE esports_config)

add_executable(esports_replay src/replay_main.cpp)
target_link_libraries(esports_replay PRIVATE esports_media)

# 安装只交付三个运行工具，测试夹具不进入安装目录。
install(TARGETS esports_live esports_replay ffmpeg_env_check RUNTIME DESTINATION bin)
install(DIRECTORY config/ DESTINATION share/esports-live-server/config FILES_MATCHING PATTERN "*.json")

include(CTest)
if(BUILD_TESTING)
    add_test(NAME ffmpeg_environment COMMAND ffmpeg_env_check)
    add_executable(logger_test tests/logger_test.cpp)
    target_link_libraries(logger_test PRIVATE esports_common)
    add_test(NAME logger_concurrency COMMAND logger_test)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME process_runner
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/process_runner_test.py
        ${CMAKE_CURRENT_SOURCE_DIR}/scripts)
    set_tests_properties(process_runner PROPERTIES TIMEOUT 15)
    find_program(FFMPEG_EXECUTABLE ffmpeg)
    find_program(FFPROBE_EXECUTABLE ffprobe)
    if(NOT FFMPEG_EXECUTABLE OR NOT FFPROBE_EXECUTABLE)
        message(FATAL_ERROR "媒体集成测试需要 ffmpeg 和 ffprobe；仅构建可设置 BUILD_TESTING=OFF")
    endif()
    add_executable(video_reference_test tests/video_reference_test.cpp)
    target_link_libraries(video_reference_test PRIVATE esports_media)
    add_test(NAME video_reference COMMAND video_reference_test)
    set_tests_properties(video_reference PROPERTIES TIMEOUT 15)
    add_test(NAME metrics_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/metrics_integration.py
        --program $<TARGET_FILE:esports_live> --script ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run-stability.py
        --ffmpeg ${FFMPEG_EXECUTABLE} --ffprobe ${FFPROBE_EXECUTABLE}
        --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    set_tests_properties(metrics_integration PROPERTIES TIMEOUT 60)
    add_executable(recording_write_failure_fixture tests/recording_write_failure_fixture.cpp)
    target_link_libraries(recording_write_failure_fixture PRIVATE esports_media)
    add_test(NAME config_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/config_integration.py
        --program $<TARGET_FILE:esports_live> --sample ${CMAKE_CURRENT_SOURCE_DIR}/config/server.json
        --ffmpeg ${FFMPEG_EXECUTABLE} --ffprobe ${FFPROBE_EXECUTABLE}
        --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    set_tests_properties(config_integration PROPERTIES TIMEOUT 60)
    add_test(NAME recording_write_failure
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/recording_write_failure.py
        --fixture $<TARGET_FILE:recording_write_failure_fixture> --program $<TARGET_FILE:esports_live>
        --ffmpeg ${FFMPEG_EXECUTABLE} --ffprobe ${FFPROBE_EXECUTABLE}
        --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    set_tests_properties(recording_write_failure PROPERTIES TIMEOUT 45)
    add_test(NAME recording_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/recording_integration.py
        --program $<TARGET_FILE:esports_live> --replay $<TARGET_FILE:esports_replay>
        --ffmpeg ${FFMPEG_EXECUTABLE} --ffprobe ${FFPROBE_EXECUTABLE}
        --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    set_tests_properties(recording_integration PROPERTIES TIMEOUT 120)
    add_test(NAME input_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/input_integration.py
        --program $<TARGET_FILE:esports_live> --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    set_tests_properties(input_integration PROPERTIES TIMEOUT 30)
    add_test(NAME codec_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/codec_integration.py
        --program $<TARGET_FILE:esports_live>
        --ffmpeg ${FFMPEG_EXECUTABLE} --ffprobe ${FFPROBE_EXECUTABLE}
        --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    add_test(NAME live_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/live_integration.py
        --program $<TARGET_FILE:esports_live>
        --ffmpeg ${FFMPEG_EXECUTABLE} --ffprobe ${FFPROBE_EXECUTABLE}
        --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    set_tests_properties(live_integration PROPERTIES TIMEOUT 120)
    add_executable(packet_queue_test tests/packet_queue_test.cpp)
    target_link_libraries(packet_queue_test PRIVATE esports_media)
    add_test(NAME packet_queue COMMAND packet_queue_test)
    set_tests_properties(packet_queue PROPERTIES TIMEOUT 15)
    add_test(NAME output_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/output_integration.py
        --program $<TARGET_FILE:esports_live> --fixture $<TARGET_FILE:output_recovery_fixture>
        --ffmpeg ${FFMPEG_EXECUTABLE} --ffprobe ${FFPROBE_EXECUTABLE}
        --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    set_tests_properties(output_integration PROPERTIES TIMEOUT 90)
    add_executable(output_recovery_fixture tests/output_recovery_fixture.cpp)
    target_link_libraries(output_recovery_fixture PRIVATE esports_media)
    add_executable(audio_sync_test tests/audio_sync_test.cpp)
    target_link_libraries(audio_sync_test PRIVATE esports_media)
    add_test(NAME audio_sync COMMAND audio_sync_test)
    set_tests_properties(audio_sync PROPERTIES TIMEOUT 90)
    add_executable(frame_queue_test tests/frame_queue_test.cpp)
    target_link_libraries(frame_queue_test PRIVATE esports_media)
    add_test(NAME frame_queue COMMAND frame_queue_test)
    set_tests_properties(frame_queue PROPERTIES TIMEOUT 15)
    add_test(NAME video_layout_integration
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/video_layout_integration.py
        --program $<TARGET_FILE:esports_live>
        --ffmpeg ${FFMPEG_EXECUTABLE} --ffprobe ${FFPROBE_EXECUTABLE}
        --work ${CMAKE_CURRENT_BINARY_DIR}/test-media)
    set_tests_properties(video_layout_integration PROPERTIES TIMEOUT 90)
    set_tests_properties(codec_integration PROPERTIES TIMEOUT 90)
    set_tests_properties(ffmpeg_environment logger_concurrency PROPERTIES TIMEOUT 15)
endif()
