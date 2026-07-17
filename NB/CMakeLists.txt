# Newbility(NB) 传输节点构建
#
# 多平台自适应:
#   - third_party/picoquic/prebuilt/<platform>/ 有静态库 -> 直接链接(快)
#   - 缺失 -> configure 阶段调 build_libs.sh 从 vendored 源码现编并落缓存
# platform 标识与 build_libs.sh 一致: <os>-<arch> 小写, 如 linux-x86_64。
cmake_minimum_required(VERSION 3.16)
project(newbility C)

include(CTest)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

if(BUILD_TESTING)
  add_executable(nb_fec_test tools/test_fec.c src/nb_fec.c src/nb_fec_rs.c)
  target_include_directories(nb_fec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_fec_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_fec_protocol COMMAND nb_fec_test)

  add_executable(nb_fec_rs_test tools/test_fec_rs.c src/nb_fec_rs.c)
  target_include_directories(nb_fec_rs_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_fec_rs_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_fec_rs COMMAND nb_fec_rs_test)

  add_executable(nb_ring_test tools/test_ring.c src/nb_ring.c)
  target_include_directories(nb_ring_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_ring_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_ring COMMAND nb_ring_test)

  add_executable(nb_auth_test tools/test_auth.c src/nb_auth.c)
  target_include_directories(nb_auth_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_auth_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_auth_test OpenSSL::Crypto)
  add_test(NAME nb_auth COMMAND nb_auth_test)

  add_executable(nb_control_test tools/test_control.c src/nb_control.c)
  target_include_directories(nb_control_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(nb_control_test PRIVATE _GNU_SOURCE)
  target_compile_options(nb_control_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_control COMMAND nb_control_test)

  add_executable(nb_routes_test tools/test_routes.c src/nb_routes.c)
  target_include_directories(nb_routes_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_routes_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_routes COMMAND nb_routes_test)

  add_executable(nb_udp_test tools/test_udp.c src/nb_udp.c)
  target_include_directories(nb_udp_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_udp_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_udp COMMAND nb_udp_test)
endif()

string(TOLOWER "${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}" NB_PLATFORM)
set(PQ_DIR ${CMAKE_SOURCE_DIR}/third_party/picoquic)
set(PQ_PREBUILT ${PQ_DIR}/prebuilt/${NB_PLATFORM})

# prebuilt 缺失 -> 从 vendored 源码现编(多平台就绪)
if(NOT EXISTS ${PQ_PREBUILT}/libpicoquic-core.a)
  message(STATUS "NB: no prebuilt picoquic for '${NB_PLATFORM}', building from source ...")
  execute_process(
    COMMAND bash ${PQ_DIR}/build_libs.sh ${NB_PLATFORM}
    RESULT_VARIABLE _pq_rc)
  if(NOT _pq_rc EQUAL 0)
    message(FATAL_ERROR "NB: picoquic build_libs.sh failed (rc=${_pq_rc})")
  endif()
else()
  message(STATUS "NB: using prebuilt picoquic for '${NB_PLATFORM}'")
endif()

add_executable(nb_node src/nb_node.c src/nb_policy.c src/nb_fec.c src/nb_fec_rs.c src/nb_ring.c src/nb_auth.c src/nb_control.c src/nb_routes.c src/nb_udp.c src/log/log4c.c)
target_compile_definitions(nb_node PRIVATE _GNU_SOURCE)
target_compile_options(nb_node PRIVATE -O2 -Wall)
target_include_directories(nb_node PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${PQ_DIR}/src/picoquic
  ${PQ_DIR}/src/loglib
  ${PQ_DIR}/src/picotls/include)

set(PQ_LIBS
  ${PQ_PREBUILT}/libpicoquic-core.a
  ${PQ_PREBUILT}/libpicoquic-log.a
  ${PQ_PREBUILT}/libpicotls-openssl.a
  ${PQ_PREBUILT}/libpicotls-core.a
  ${PQ_PREBUILT}/libpicotls-minicrypto.a)

# 静态库间循环依赖 -> GNU ld group(Linux)。非 GNU ld 平台(如 macOS ld64)可去掉 group。
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  target_link_libraries(nb_node
    -Wl,--start-group ${PQ_LIBS} -Wl,--end-group
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
else()
  target_link_libraries(nb_node ${PQ_LIBS}
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads)
endif()
