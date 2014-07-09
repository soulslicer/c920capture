cmake_minimum_required(VERSION 2.8)

add_executable (capture c920capture.h capture.cpp uvch264.h)

#target_link_libraries(libv4l2)