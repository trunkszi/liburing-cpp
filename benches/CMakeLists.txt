# Bench slices own their fragment and optional dependency setup; the scaffold downloads nothing.
file(GLOB slice_fragments CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/T[0-9]*.cmake")
foreach(fragment IN LISTS slice_fragments)
  include("${fragment}")
endforeach()
