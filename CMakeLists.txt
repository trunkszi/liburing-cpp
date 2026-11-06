cmake_minimum_required(VERSION 3.30)
project(liburing VERSION 0.1.0 DESCRIPTION "C++26 runtime-free io_uring and futex2 proactor framework" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
option(LIBURING_BUILD_TESTS "Build deterministic tests" ${PROJECT_IS_TOP_LEVEL})
option(LIBURING_BUILD_BENCHES "Build benchmarks from registered slice fragments" OFF)
option(LIBURING_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
include(LiburingCompilerOptions)
include(LiburingReflection)
include(LiburingSanitizers)
include(LiburingTargets)
include(LiburingCompileFail)
include(LiburingCodegenGates)
liburing_detect_reflection()
liburing_create_libraries()

# Slice-owned fragments register hosted libraries or tools without changing this file.
file(GLOB slice_fragments CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/T[0-9]*.cmake")
foreach(fragment IN LISTS slice_fragments)
  include("${fragment}")
endforeach()
if(LIBURING_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
if(LIBURING_BUILD_BENCHES)
  add_subdirectory(benches)
endif()

install(TARGETS liburing_abi liburing_sys liburing_core liburing_ops liburing_resources liburing_futex2 liburing EXPORT liburing-targets)
install(DIRECTORY DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" OPTIONAL)
install(EXPORT liburing-targets NAMESPACE liburing:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liburing")
configure_package_config_file(cmake/liburing-config.cmake.in
  "${CMAKE_CURRENT_BINARY_DIR}/liburing-config.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liburing")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/liburing-config-version.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMajorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/liburing-config.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/liburing-config-version.cmake"
  cmake/LiburingReflection.cmake cmake/LiburingCompilerOptions.cmake
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/liburing")
