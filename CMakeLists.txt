cmake_minimum_required(VERSION 2.6)
project(fastcci)

# HTTP database server (libwebsockets)
add_executable(fastcci_server fastcci_server_lws.cc)
LINK_DIRECTORIES(/usr/local/lib)
target_link_libraries(fastcci_server websockets pthread)

# build the DB files
add_executable(fastcci_build_db fastcci_build_db.cc)

# command line inspection tools
add_executable(fastcci_circulartest fastcci_circulartest.cc)
add_executable(fastcci_tarjan fastcci_tarjan.cc)
add_executable(fastcci_intersection2 fastcci_intersection2.cc)
add_executable(fastcci_subcats fastcci_subcats.cc)
add_executable(fastcci_path fastcci_path.cc)
add_executable(fastcci_dbinfo fastcci_dbinfo.cc)
add_executable(fastcci_pfs_search fastcci_pfs_search.cc)
add_executable(fastcci_diamond fastcci_diamond.cc)
add_executable(fastcci_subcatstats fastcci_subcatstats.cc)
add_executable(fastcci_subcatcount fastcci_subcatcount.cc)

install (TARGETS fastcci_server fastcci_build_db DESTINATION /usr/bin COMPONENT binaries)
