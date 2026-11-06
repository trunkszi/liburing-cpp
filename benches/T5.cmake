add_executable(futex2_bench futex2/futex2_bench.cc)
liburing_configure_internal(futex2_bench NO_SANITIZERS)
set_target_properties(futex2_bench PROPERTIES INTERPROCEDURAL_OPTIMIZATION FALSE)
target_compile_options(futex2_bench PRIVATE -O3 -fno-lto)
