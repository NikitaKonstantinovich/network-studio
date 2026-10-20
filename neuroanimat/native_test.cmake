string(TIMESTAMP stamp "%Y%m%d_%H%M%S")
set(dir "${WORK}/${stamp}")
execute_process(COMMAND "${GUI}" --animat-smoke "${dir}" --config "${INPUT}"
    RESULT_VARIABLE result TIMEOUT 55)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Neuroanimat Win32/DX11 smoke failed: ${dir}")
endif()
file(READ "${dir}/renderer.txt" log)
if(NOT log MATCHES "animat_win32_dx11=1" OR NOT log MATCHES "error=\n"
   OR NOT log MATCHES "native_viewport_checks=8"
   OR NOT log MATCHES "animat_rows=[2-9][0-9]+")
    message(FATAL_ERROR "Incomplete native animat run: ${log}")
endif()
foreach(index 3 11 19 27)
    if(NOT EXISTS "${dir}/animat_${index}_1.png")
        message(FATAL_ERROR "Missing native tab screenshot ${index}")
    endif()
endforeach()
message(STATUS "Neuroanimat native screenshots and exports: ${dir}")
