cmake_minimum_required (VERSION 3.10)
project(rubberbridge)

add_subdirectory(imtui)
add_subdirectory(src)

install(TARGETS rubberbridge
  RUNTIME DESTINATION bin
)