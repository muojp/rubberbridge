set(EXECUTABLE_OUTPUT_PATH "../bin")
add_executable(rubberbridge client.cpp)
target_link_libraries(rubberbridge PRIVATE imtui-ncurses rubberband jack m)
