cmake_minimum_required(VERSION 3.20)
project(NeuroAstroStudio LANGUAGES CXX)
option(STUDIO_PROFILE "Enable nested solver diagnostic timers (not for speed comparisons)" OFF)
option(STUDIO_BUILD_PERFORMANCE_TOOLS "Build reproducible solver benchmark tools" OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    add_compile_options(/utf-8 /W3)
endif()
set(VENDOR "${CMAKE_CURRENT_SOURCE_DIR}/vendor")
function(write_generated path contents)
    if(EXISTS "${path}")
        file(READ "${path}" old_contents)
        if(old_contents STREQUAL contents)
            return()
        endif()
    endif()
    file(WRITE "${path}" "${contents}")
endfunction()
# Build a local compatibility copy: the bundled JSON reader rejects escaped
# slashes, lacks Unicode decoding and checks the wrong pointer at end of input.
file(READ "${VENDOR}/json/json.cpp" JSON_SOURCE)
string(REPLACE "bool is_control_character(const char input)" "bool legacy_is_control_character(const char input)" JSON_SOURCE "${JSON_SOURCE}")
string(REPLACE "bool is_hex_digit(const char input)" "bool is_control_character(const char input) { return input == '/' || legacy_is_control_character(input); }\nbool is_hex_digit(const char input)" JSON_SOURCE "${JSON_SOURCE}")
string(REPLACE "while(!EMPTY_STRING(input) && stream.push(*index)" "while(!EMPTY_STRING(index) && stream.push(*index)" JSON_SOURCE "${JSON_SOURCE}")
string(REPLACE "std::isspace(next)" "std::isspace(static_cast<unsigned char>(next))" JSON_SOURCE "${JSON_SOURCE}")
string(FIND "${JSON_SOURCE}" "std::string json::parsing::decode_string(" DECODE_START)
string(FIND "${JSON_SOURCE}" "std::string json::parsing::encode_string(" DECODE_END)
if(DECODE_START LESS 0 OR DECODE_END LESS 0)
    message(FATAL_ERROR "Bundled JSON decoder changed; review compatibility adapter.")
endif()
string(SUBSTRING "${JSON_SOURCE}" 0 ${DECODE_START} JSON_BEFORE)
string(SUBSTRING "${JSON_SOURCE}" ${DECODE_END} -1 JSON_AFTER)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/json_strings.inc" JSON_DECODER)
write_generated("${CMAKE_CURRENT_BINARY_DIR}/json_standalone.cpp" "${JSON_BEFORE}\n${JSON_DECODER}\n${JSON_AFTER}")
set(CHECKPOINT_SIGNATURE "${CMAKE_CXX_COMPILER_ID}|${CMAKE_CXX_COMPILER_VERSION}|${CMAKE_SYSTEM_NAME}|${CMAKE_SYSTEM_PROCESSOR}|${CMAKE_SIZEOF_VOID_P}|${CMAKE_BUILD_TYPE}|${CMAKE_CXX_FLAGS}|${CMAKE_CXX_FLAGS_RELEASE}|profile=${STUDIO_PROFILE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/numerics/compute_threads.h")
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/numerics/compute_threads.h" thread_policy_hash)
string(APPEND CHECKPOINT_SIGNATURE "|numerics/compute_threads.h:${thread_policy_hash}")
foreach(source animat_examples.cpp animat_types.h field_settings.h animat.h animat.cpp animat_analysis.cpp animat_state.cpp animat_json.inc weight_field.h weight_field.cpp activity_field.h activity_field.cpp network.h network.cpp stimulator.cpp stimulator_json.inc experiment.cpp experiment_json.inc analysis.cpp analysis.h analysis_json.inc integrator.cpp numerics/integrator.h numerics/error_norm.h numerics/worker_pool.h numerics/worker_pool.cpp performance/profile.h io.h io.cpp checkpoint.cpp history_archive.h history_archive.cpp archive_codec.h archive_analysis.cpp json_strings.inc vendor/systems/neuro_astro/model.cuh vendor/systems/neuro_astro/variables.inc vendor/systems/neuro_astro/parameters.inc vendor/json/json.cpp vendor/json/json.h)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${source}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${source}" source_hash)
    string(APPEND CHECKPOINT_SIGNATURE "|${source}:${source_hash}")
endforeach()
string(SHA256 CHECKPOINT_BUILD_ID "${CHECKPOINT_SIGNATURE}")
write_generated("${CMAKE_CURRENT_BINARY_DIR}/checkpoint_build.h" "#define STUDIO_CHECKPOINT_BUILD_ID \"${CHECKPOINT_BUILD_ID}\"\n")
add_library(network_core animat.cpp animat_examples.cpp animat_state.cpp animat_analysis.cpp weight_field.cpp activity_field.cpp network.cpp stimulator.cpp experiment.cpp analysis.cpp integrator.cpp numerics/worker_pool.cpp io.cpp checkpoint.cpp history_archive.cpp archive_analysis.cpp generator.cpp "${CMAKE_CURRENT_BINARY_DIR}/json_standalone.cpp")
target_include_directories(network_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(network_core PRIVATE "${VENDOR}/json")
target_include_directories(network_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
if(STUDIO_PROFILE)
    target_compile_definitions(network_core PUBLIC STUDIO_PROFILE)
endif()
if(STUDIO_BUILD_PERFORMANCE_TOOLS)
    add_executable(network_studio_benchmark performance/solver_benchmark.cpp)
    target_link_libraries(network_studio_benchmark PRIVATE network_core)
    if(WIN32)
        add_executable(network_studio_lab_benchmark performance/20261020_lab6/benchmark.cpp)
        target_link_libraries(network_studio_lab_benchmark PRIVATE network_core psapi)
    endif()
endif()
find_package(Threads REQUIRED)
target_link_libraries(network_core PUBLIC Threads::Threads)
add_library(studio_weights weight_analysis.cpp)
target_link_libraries(studio_weights PUBLIC network_core)
add_library(studio_runner simulation_runner.cpp simulation_view.cpp)
target_link_libraries(studio_runner PUBLIC network_core studio_weights Threads::Threads)
add_executable(network_studio_runner_tests runner_tests.cpp)
target_link_libraries(network_studio_runner_tests PRIVATE studio_runner)
if(STUDIO_BUILD_PERFORMANCE_TOOLS)
    add_executable(network_studio_runner_benchmark performance/runner_benchmark.cpp)
    target_link_libraries(network_studio_runner_benchmark PRIVATE studio_runner)
endif()
add_executable(network_studio_cli cli.cpp analysis_cli.cpp weight_cli.cpp package_cli.cpp)
target_include_directories(network_studio_cli PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(network_studio_cli PRIVATE network_core studio_weights)
add_executable(network_studio_error_norm_tests performance/20261020_lab6/error_norm_tests.cpp)
target_link_libraries(network_studio_error_norm_tests PRIVATE network_core)
add_test(NAME error_norm_bitwise COMMAND network_studio_error_norm_tests)
add_executable(network_studio_tests tests.cpp)
target_link_libraries(network_studio_tests PRIVATE network_core)
add_executable(network_studio_generator_tests generator_tests.cpp)
target_link_libraries(network_studio_generator_tests PRIVATE network_core)
add_executable(network_studio_numerics_tests numerics_tests.cpp)
target_link_libraries(network_studio_numerics_tests PRIVATE network_core)
add_executable(network_studio_population lab2/population.cpp)
target_link_libraries(network_studio_population PRIVATE network_core)
enable_testing()
add_executable(network_studio_step_tests step_range_tests.cpp)
target_link_libraries(network_studio_step_tests PRIVATE network_core)
add_test(NAME integration_step_range COMMAND network_studio_step_tests "${CMAKE_CURRENT_BINARY_DIR}/step-range-results")
add_executable(network_studio_animat_tests animat_tests.cpp)
target_link_libraries(network_studio_animat_tests PRIVATE studio_runner)
add_test(NAME neuroanimat COMMAND network_studio_animat_tests test "${CMAKE_CURRENT_BINARY_DIR}/animat-test-results")
add_test(NAME neuroanimat_process_prepare COMMAND network_studio_animat_tests process-prepare "${CMAKE_CURRENT_BINARY_DIR}/animat-process")
add_test(NAME neuroanimat_process_resume COMMAND network_studio_animat_tests process-resume "${CMAKE_CURRENT_BINARY_DIR}/animat-process")
set_tests_properties(neuroanimat_process_prepare PROPERTIES FIXTURES_SETUP animat_process)
set_tests_properties(neuroanimat_process_resume PROPERTIES FIXTURES_REQUIRED animat_process)
set_tests_properties(neuroanimat neuroanimat_process_prepare neuroanimat_process_resume PROPERTIES TIMEOUT 120)
find_package(Python3 3.10 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    add_test(NAME integration_step_cli COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_SOURCE_DIR}/performance/20261020_step_range/verify_cli.py"
        --cli "$<TARGET_FILE:network_studio_cli>" --work "${CMAKE_CURRENT_BINARY_DIR}/step-range-cli")
    add_test(NAME reproducible_experiment_package COMMAND "${Python3_EXECUTABLE}" -X utf8
        "${CMAKE_CURRENT_SOURCE_DIR}/experiment_package/tests.py" "--cli" "$<TARGET_FILE:network_studio_cli>"
        "--output-root" "${CMAKE_CURRENT_SOURCE_DIR}/verification/experiment_package_20261020/ctest")
    set_tests_properties(reproducible_experiment_package PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
else()
    message(WARNING "Python 3 not found: reproducible_experiment_package test is unavailable")
endif()
add_executable(network_studio_weight_analysis_tests weight_analysis_tests.cpp)
target_link_libraries(network_studio_weight_analysis_tests PRIVATE studio_runner)
add_test(NAME weight_snapshots_and_statistics COMMAND network_studio_weight_analysis_tests)
set_tests_properties(weight_snapshots_and_statistics PROPERTIES TIMEOUT 120)
add_executable(network_studio_archive_tests archive_tests.cpp)
target_link_libraries(network_studio_archive_tests PRIVATE studio_runner)
if(WIN32)
    target_link_libraries(network_studio_archive_tests PRIVATE psapi)
endif()
add_test(NAME disk_history_and_analysis COMMAND network_studio_archive_tests)
set_tests_properties(disk_history_and_analysis PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME disk_history_process_resume COMMAND ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:network_studio_cli>" "-DTEST=$<TARGET_FILE:network_studio_archive_tests>"
    "-DWORK=${CMAKE_CURRENT_SOURCE_DIR}/verification/disk_history_20261020/process" -P "${CMAKE_CURRENT_SOURCE_DIR}/disk_history/process_test.cmake")
set_tests_properties(disk_history_process_resume PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
add_test(NAME simulation_runner_lifecycle COMMAND network_studio_runner_tests)
add_executable(network_studio_parallel_tests parallel_tests.cpp)
target_link_libraries(network_studio_parallel_tests PRIVATE studio_runner)
add_test(NAME parallel_solver_invariance COMMAND network_studio_parallel_tests)
set_tests_properties(parallel_solver_invariance PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
add_test(NAME thread_modes_process_resume COMMAND ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:network_studio_cli>" "-DFIXTURES=$<TARGET_FILE:network_studio_parallel_tests>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/thread-modes-process" -P "${CMAKE_CURRENT_SOURCE_DIR}/performance/20261019_thread_modes/process_test.cmake")
set_tests_properties(thread_modes_process_resume PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
set_tests_properties(simulation_runner_lifecycle PROPERTIES TIMEOUT 180)
add_executable(network_studio_cache_tests performance/cache_tests.cpp)
target_link_libraries(network_studio_cache_tests PRIVATE network_core)
add_test(NAME performance_cache_invariance COMMAND network_studio_cache_tests)
add_executable(network_studio_kernel_tests performance/kernel_tests.cpp)
target_link_libraries(network_studio_kernel_tests PRIVATE network_core)
add_test(NAME packed_kernel_reference COMMAND network_studio_kernel_tests)
add_executable(network_studio_experiment_tests experiment_tests.cpp)
target_link_libraries(network_studio_experiment_tests PRIVATE network_core)
add_executable(network_studio_analysis_tests analysis_profile_tests.cpp)
target_link_libraries(network_studio_analysis_tests PRIVATE network_core)
add_test(NAME analysis_profiles_and_intervals COMMAND network_studio_analysis_tests)
add_test(NAME analysis_export_process COMMAND ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:network_studio_cli>" "-DFIXTURES=$<TARGET_FILE:network_studio_analysis_tests>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/analysis-process" -P "${CMAKE_CURRENT_SOURCE_DIR}/experiments/analysis_process_test.cmake")
add_test(NAME spatial_analysis_and_stages COMMAND network_studio_experiment_tests)
add_test(NAME staged_process_resume COMMAND ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:network_studio_cli>" "-DFIXTURES=$<TARGET_FILE:network_studio_experiment_tests>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/staged-process" -P "${CMAKE_CURRENT_SOURCE_DIR}/experiments/process_test.cmake")
add_test(NAME network_model_and_io COMMAND network_studio_tests)
add_test(NAME network_generation COMMAND network_studio_generator_tests)
add_test(NAME numerical_regressions COMMAND network_studio_numerics_tests)
add_executable(network_studio_delay_tests delay_tests.cpp)
target_link_libraries(network_studio_delay_tests PRIVATE network_core)
add_test(NAME synaptic_delays COMMAND network_studio_delay_tests)
add_test(NAME delayed_process_resume COMMAND ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:network_studio_cli>" "-DFIXTURES=$<TARGET_FILE:network_studio_delay_tests>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/delayed-process" -P "${CMAKE_CURRENT_SOURCE_DIR}/delays/process_test.cmake")
add_executable(network_studio_checkpoint_tests checkpoint_tests.cpp)
target_link_libraries(network_studio_checkpoint_tests PRIVATE network_core)
add_test(NAME checkpoint_roundtrip COMMAND network_studio_checkpoint_tests)
add_test(NAME checkpoint_process_resume COMMAND ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:network_studio_cli>" "-DFIXTURES=$<TARGET_FILE:network_studio_checkpoint_tests>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/checkpoint-process" -P "${CMAKE_CURRENT_SOURCE_DIR}/checkpoint/process_test.cmake")
add_test(NAME population_equivalence COMMAND network_studio_population --self-test)
add_library(studio_weight_field INTERFACE)
target_link_libraries(studio_weight_field INTERFACE studio_runner)
add_executable(network_studio_stimulator_tests stimulator_tests.cpp)
target_link_libraries(network_studio_stimulator_tests PRIVATE studio_weight_field)
add_test(NAME shared_spatial_stimulators_activity COMMAND network_studio_stimulator_tests)
add_executable(network_studio_stimulator_timing_tests stimulator_timing_tests.cpp)
target_link_libraries(network_studio_stimulator_timing_tests PRIVATE network_core)
add_test(NAME stimulator_frequency_timing COMMAND network_studio_stimulator_timing_tests)
add_test(NAME stimulator_frequency_process_resume COMMAND ${CMAKE_COMMAND}
    "-DTEST=$<TARGET_FILE:network_studio_stimulator_timing_tests>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/stimulator-frequency-process" -P "${CMAKE_CURRENT_SOURCE_DIR}/stimulation/frequency_process_test.cmake")
add_test(NAME stimulator_process_resume COMMAND ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:network_studio_cli>" "-DFIXTURES=$<TARGET_FILE:network_studio_stimulator_tests>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/stimulator-process" -P "${CMAKE_CURRENT_SOURCE_DIR}/stimulation/process_test.cmake")
add_executable(network_studio_field_tests weight_field_tests.cpp)
target_link_libraries(network_studio_field_tests PRIVATE studio_weight_field)
add_test(NAME weight_field_geometry_and_weights COMMAND network_studio_field_tests)
add_executable(network_studio_long_history_tests long_history_tests.cpp)
target_link_libraries(network_studio_long_history_tests PRIVATE network_core)
add_test(NAME long_history_limits COMMAND network_studio_long_history_tests)
add_test(NAME long_history_process_resume COMMAND ${CMAKE_COMMAND}
    "-DCLI=$<TARGET_FILE:network_studio_cli>" "-DFIXTURES=$<TARGET_FILE:network_studio_long_history_tests>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/long-process" -P "${CMAKE_CURRENT_SOURCE_DIR}/long_runs/process_test.cmake")
if(WIN32)
    # The CUDAynamics fork includes an unused fullscreen header which pulls in
    # the entire CUDA application. Remove only that include in a generated copy.
    # Original vendor sources and the existing application remain untouched.
    file(READ "${VENDOR}/imgui/imgui.cpp" IMGUI_SOURCE)
    string(REPLACE "#include \"../gui/fullscreen_funcs.h\"" "// Standalone build: unused application header omitted." IMGUI_SOURCE "${IMGUI_SOURCE}")
    write_generated("${CMAKE_CURRENT_BINARY_DIR}/imgui_standalone.cpp" "${IMGUI_SOURCE}")
    # The fork adds optional 3D/heatmap fields without initializing them.
    # Normal 2D plots must start with those modes disabled.
    file(READ "${VENDOR}/implot/implot_internal.h" IMPLOT_INTERNAL)
    string(REPLACE "bool is3d;" "bool is3d = false;" IMPLOT_INTERNAL "${IMPLOT_INTERNAL}")
    string(REPLACE "bool isHeatmapSelectionModeOn;" "bool isHeatmapSelectionModeOn = false;" IMPLOT_INTERNAL "${IMPLOT_INTERNAL}")
    string(REPLACE "float* deltax;" "float* deltax = nullptr;" IMPLOT_INTERNAL "${IMPLOT_INTERNAL}")
    string(REPLACE "float* deltay;" "float* deltay = nullptr;" IMPLOT_INTERNAL "${IMPLOT_INTERNAL}")
    string(REPLACE "bool shiftClicked;" "bool shiftClicked = false;" IMPLOT_INTERNAL "${IMPLOT_INTERNAL}")
    string(REPLACE "../imgui/imgui_internal.h" "imgui_internal.h" IMPLOT_INTERNAL "${IMPLOT_INTERNAL}")
    write_generated("${CMAKE_CURRENT_BINARY_DIR}/implot_internal.h" "${IMPLOT_INTERNAL}")
    configure_file("${VENDOR}/implot/implot.cpp" "${CMAKE_CURRENT_BINARY_DIR}/implot_standalone.cpp" COPYONLY)
    configure_file("${VENDOR}/implot/implot_items.cpp" "${CMAKE_CURRENT_BINARY_DIR}/implot_items_standalone.cpp" COPYONLY)
    add_library(studio_gui animat_gui.cpp gui.cpp weight_analysis_gui.cpp archive_gui.cpp stimulator_gui.cpp experiment_gui.cpp analysis_gui.cpp weight_field_gui.cpp plot_windows.cpp plot_workspace.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/imgui_standalone.cpp" "${VENDOR}/imgui/imgui_draw.cpp"
        "${VENDOR}/imgui/imgui_tables.cpp" "${VENDOR}/imgui/imgui_widgets.cpp"
        "${CMAKE_CURRENT_BINARY_DIR}/implot_standalone.cpp" "${CMAKE_CURRENT_BINARY_DIR}/implot_items_standalone.cpp")
    target_include_directories(studio_gui PUBLIC "${VENDOR}/imgui" "${VENDOR}/implot" "${VENDOR}")
    target_link_libraries(studio_gui PUBLIC network_core studio_weight_field)
    target_compile_definitions(studio_gui PUBLIC IMGUI_ENABLE_TEST_ENGINE)
    if(MSVC)
        set_source_files_properties("${VENDOR}/imgui/imgui_widgets.cpp" PROPERTIES COMPILE_OPTIONS "/wd4244;/wd4828")
    endif()
    add_executable(network_studio WIN32 main.cpp test_hooks_stub.cpp
        "${VENDOR}/imgui/backends/imgui_impl_win32.cpp"
        "${VENDOR}/imgui/backends/imgui_impl_dx11.cpp")
    target_link_libraries(network_studio PRIVATE studio_gui d3d11 dxgi d3dcompiler windowscodecs ole32 comdlg32 shell32 dwmapi)
    add_test(NAME weight_snapshots_win32_dx11 COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DWORK=${CMAKE_CURRENT_SOURCE_DIR}/verification/weight_snapshots_20261020/native"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/weight_analysis/native_test.cmake")
    set_tests_properties(weight_snapshots_win32_dx11 PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)
    add_test(NAME runner_native_close COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/runner-native-close"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/performance/runner_native_test.cmake")
    set_tests_properties(runner_native_close PROPERTIES RUN_SERIAL TRUE TIMEOUT 40)
    add_test(NAME parallel_native_close COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/parallel-native-close"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/performance/parallel_native_test.cmake")
    set_tests_properties(parallel_native_close PROPERTIES RUN_SERIAL TRUE TIMEOUT 50)
    foreach(count 6 12 16)
        add_test(NAME thread_modes_native_${count} COMMAND ${CMAKE_COMMAND}
            "-DGUI=$<TARGET_FILE:network_studio>" "-DCOUNT=${count}" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/thread-modes-native-${count}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/performance/20261019_thread_modes/native_test.cmake")
        set_tests_properties(thread_modes_native_${count} PROPERTIES RUN_SERIAL TRUE TIMEOUT 50)
    endforeach()
    add_test(NAME disk_history_win32_dx11 COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DWORK=${CMAKE_CURRENT_SOURCE_DIR}/verification/disk_history_20261020/native"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/disk_history/native_test.cmake")
    set_tests_properties(disk_history_win32_dx11 PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME analysis_profiles_win32_dx11 COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/analysis-native"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/experiments/analysis_native_test.cmake")
    set_tests_properties(analysis_profiles_win32_dx11 PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME experiment_win32_dx11 COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DFIXTURES=$<TARGET_FILE:network_studio_experiment_tests>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/experiment-native" -P "${CMAKE_CURRENT_SOURCE_DIR}/experiments/native_test.cmake")
    add_test(NAME neuroanimat_win32_dx11 COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/neuroanimat/examples/minimal.json"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/animat-native" -P "${CMAKE_CURRENT_SOURCE_DIR}/neuroanimat/native_test.cmake")
    set_tests_properties(neuroanimat_win32_dx11 PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
    add_test(NAME stimulator_win32_dx11 COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DFIXTURES=$<TARGET_FILE:network_studio_stimulator_tests>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/stimulator-native" -P "${CMAKE_CURRENT_SOURCE_DIR}/stimulation/native_test.cmake")
    set_tests_properties(stimulator_win32_dx11 experiment_win32_dx11 PROPERTIES RUN_SERIAL TRUE)
    add_executable(network_studio_gui_tests gui_tests.cpp)
    target_include_directories(network_studio_gui_tests BEFORE PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(network_studio_gui_tests PRIVATE studio_gui)
    add_test(NAME gui_interactions_and_plots COMMAND network_studio_gui_tests)
    add_executable(network_studio_plot_tests plot_window_tests.cpp test_hooks_stub.cpp)
    target_link_libraries(network_studio_plot_tests PRIVATE studio_gui)
    add_test(NAME plot_windows_and_workspace COMMAND network_studio_plot_tests)
    add_executable(network_studio_field_gui_tests weight_field_gui_tests.cpp test_hooks_stub.cpp)
    target_link_libraries(network_studio_field_gui_tests PRIVATE studio_gui)
    add_test(NAME weight_field_gui_invariance COMMAND network_studio_field_gui_tests)
    add_test(NAME weight_field_win32_dx11 COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DFIXTURES=$<TARGET_FILE:network_studio_field_tests>"
        "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/weight-field-native" -P "${CMAKE_CURRENT_SOURCE_DIR}/weight_field/native_test.cmake")
    set_tests_properties(weight_field_win32_dx11 PROPERTIES RUN_SERIAL TRUE)
    add_test(NAME win32_dx11_plot_viewports COMMAND ${CMAKE_COMMAND}
        "-DGUI=$<TARGET_FILE:network_studio>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/viewport-smoke"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/plots/native_process_test.cmake")
    set_tests_properties(win32_dx11_plot_viewports PROPERTIES RUN_SERIAL TRUE)
endif()
