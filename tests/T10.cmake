foreach(example IN ITEMS proactor_subsystems fixed_files_and_buffer_ring_tcp_echo completion_socket_io proactor_modes)
  add_test(NAME example_${example} COMMAND ${example})
  set_tests_properties(example_${example} PROPERTIES TIMEOUT 30)
endforeach()
