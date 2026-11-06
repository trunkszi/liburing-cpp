# Keep native evidence separate from instrumented executable targets.
function(liburing_add_codegen_probe_library target)
  liburing_add_codegen_probes(${target} ${ARGN})
endfunction()
liburing_add_codegen_probe_library(liburing_probes src/probes/proactor_probes.cc)
