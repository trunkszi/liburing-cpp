liburing_add_test(smoke_test unit/smoke_test.cc)
liburing_add_compile_fail(smoke_compile_fail compile_fail/smoke_compile_fail.cc)

# Each slice owns tests/T<N>.cmake and calls liburing_add_test/liburing_add_compile_fail there.
# CONFIGURE_DEPENDS discovers new fragments without edits to this shared entry point.
file(GLOB slice_fragments CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/T[0-9]*.cmake")
foreach(fragment IN LISTS slice_fragments)
  include("${fragment}")
endforeach()

# A source caret snippet must not satisfy the diagnostic matcher.
liburing_add_compile_fail(caret_diagnostic_control compile_fail/smoke_compile_fail.cc EXPECT_DIAGNOSTIC_REJECTION)
target_compile_definitions(caret_diagnostic_control PRIVATE LIBURING_CARET_FAILURE_CONTROL=1)

if(LIBURING_ENABLE_SANITIZERS)
  liburing_add_test(ubsan_failure_control unit/smoke_test.cc)
  target_compile_definitions(ubsan_failure_control PRIVATE LIBURING_UBSAN_FAILURE_CONTROL=1)
  set_tests_properties(ubsan_failure_control PROPERTIES WILL_FAIL TRUE)
endif()

# This object must stay free of sanitizer instrumentation even in sanitizer presets.
liburing_add_codegen_probes(native_smoke_probe unit/smoke_test.cc)

set(native_smoke_nm "/root/.local/toolchain/clang-glibc/bin/llvm-nm")
if(NOT EXISTS "${native_smoke_nm}" AND CMAKE_NM)
  set(native_smoke_nm "${CMAKE_NM}")
endif()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/native_smoke_probe_audit.cmake" [=[
if(NOT PROBE_OBJECTS)
  message(FATAL_ERROR "No object files provided for native_smoke_probe_audit")
endif()
foreach(obj IN LISTS PROBE_OBJECTS)
  if(NOT EXISTS "${obj}")
    execute_process(
      COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --target native_smoke_probe
      RESULT_VARIABLE build_rc)
    if(NOT build_rc EQUAL 0 OR NOT EXISTS "${obj}")
      message(FATAL_ERROR "Missing native_smoke_probe object file: ${obj}")
    endif()
  endif()
  execute_process(
    COMMAND "${NM_TOOL}" "${obj}"
    RESULT_VARIABLE nm_rc
    OUTPUT_VARIABLE nm_out
    ERROR_VARIABLE nm_err)
  if(NOT nm_rc EQUAL 0)
    message(FATAL_ERROR "nm failed on ${obj}: ${nm_err}")
  endif()
  if(nm_out MATCHES "(__asan|__ubsan|__cxa_throw)")
    message(FATAL_ERROR "Forbidden sanitizer or exception symbol in ${obj}:\n${nm_out}")
  endif()
endforeach()
]=])
add_test(
  NAME native_smoke_probe_audit
  COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_BINARY_DIR}"
    "-DNM_TOOL=${native_smoke_nm}"
    "-DPROBE_OBJECTS=$<TARGET_OBJECTS:native_smoke_probe>"
    -P "${CMAKE_CURRENT_BINARY_DIR}/native_smoke_probe_audit.cmake")

enable_language(C)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mixed_consumer.c" "int main(void) { return 0; }\n")
add_executable(mixed_c_consumer "${CMAKE_CURRENT_BINARY_DIR}/mixed_consumer.c")
target_link_libraries(mixed_c_consumer PRIVATE liburing::liburing)
target_compile_options(mixed_c_consumer PRIVATE -Wall -Wextra -Werror)
target_link_options(mixed_c_consumer PRIVATE -Wl,-z,stack-size=8388608)
add_test(NAME mixed_c_consumer COMMAND mixed_c_consumer)
