foreach(example IN ITEMS proactor_subsystems fixed_files_and_buffer_ring_tcp_echo completion_socket_io proactor_modes)
  add_executable(${example} "${PROJECT_SOURCE_DIR}/examples/${example}.cc")
  liburing_configure_internal(${example})
endforeach()
