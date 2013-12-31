cmake_minimum_required(VERSION 2.6)
project(fastcci)

add_executable(fastcci_intersection fastcci_intersection.cc)
LINK_DIRECTORIES(/usr/local/lib)
target_link_libraries(fastcci_intersection onion pthread)

add_executable(fastcci fastcci.cc)
add_executable(fastcci_circulartest fastcci_circulartest.cc)
add_executable(fastcci_intersection2 fastcci_intersection2.cc)
add_executable(fastcci_subcats fastcci_subcats.cc)
add_executable(fastcci_path fastcci_path.cc)
