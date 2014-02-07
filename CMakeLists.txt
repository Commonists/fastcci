cmake_minimum_required(VERSION 2.6)
project(fastcci)

# HTTP database server
add_executable(fastcci_server fastcci_server.cc)
LINK_DIRECTORIES(/usr/local/lib)
target_link_libraries(fastcci_server onion pthread)

# build the DB files
add_executable(fastcci_build_db fastcci_build_db.cc)

# command line inspection tools
add_executable(fastcci_circulartest fastcci_circulartest.cc)
add_executable(fastcci_intersection2 fastcci_intersection2.cc)
add_executable(fastcci_subcats fastcci_subcats.cc)
add_executable(fastcci_path fastcci_path.cc)

install (TARGETS fastcci_server fastcci_build_db DESTINATION /usr/bin COMPONENT binaries)
