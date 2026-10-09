# Newbility(NB) 传输节点构建
#
# 多平台自适应:
#   - third_party/picoquic/prebuilt/<platform>/ 有静态库 -> 直接链接(快)
#   - 缺失 -> configure 阶段调 build_libs.sh 从 vendored 源码现编并落缓存
# platform 标识与 build_libs.sh 一致: <os>-<arch> 小写, 如 linux-x86_64。
cmake_minimum_required(VERSION 3.16)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NB_PROJECT_VERSION LIMIT_COUNT 1)
project(newbility VERSION "${NB_PROJECT_VERSION}" LANGUAGES C)

include(CTest)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

add_library(nb_connect STATIC src/nb_connect.c)
target_include_directories(nb_connect PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_options(nb_connect PRIVATE -O2 -Wall -Wextra -Werror)

if(BUILD_TESTING)
  add_executable(nb_connect_test tools/test_connect.c)
  target_link_libraries(nb_connect_test PRIVATE nb_connect)
  target_include_directories(nb_connect_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_connect_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_connect_test COMMAND nb_connect_test)

  add_executable(nb_exit_recovery_test tools/test_exit_recovery.c src/nb_metrics.c
    src/nb_yfe2_metrics.c src/nb_yfe2_negotiation.c src/nb_yfe2_profile.c src/nb_yfe2_adaptive.c)
  target_link_libraries(nb_exit_recovery_test PRIVATE nb_connect OpenSSL::Crypto)
  target_include_directories(nb_exit_recovery_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_exit_recovery_test PRIVATE -O2 -Wall -Wextra -Werror)
  add_test(NAME nb_exit_recovery_test COMMAND nb_exit_recovery_test)

  add_executable(nb_fec_test tools/test_fec.c src/nb_fec.c src/nb_fec_rs.c)
  target_include_directories(nb_fec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_fec_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_fec_protocol COMMAND nb_fec_test)

  add_executable(nb_fec_policy_test tools/test_fec_policy.c src/nb_fec_policy.c)
  target_include_directories(nb_fec_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_fec_policy_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_fec_policy COMMAND nb_fec_policy_test)

  add_executable(nb_fec_rs_test tools/test_fec_rs.c src/nb_fec_rs.c)
  target_include_directories(nb_fec_rs_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_fec_rs_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_fec_rs_test Threads::Threads)
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

  add_executable(nb_auth_async_test tools/test_auth_async.c src/nb_auth.c src/nb_auth_async.c)
  target_include_directories(nb_auth_async_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_auth_async_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_auth_async_test OpenSSL::Crypto Threads::Threads)
  add_test(NAME nb_auth_async COMMAND nb_auth_async_test)

  add_executable(nb_control_test tools/test_control.c src/nb_control.c)
  target_include_directories(nb_control_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(nb_control_test PRIVATE _GNU_SOURCE)
  target_compile_options(nb_control_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_control COMMAND nb_control_test)

  add_executable(nb_transport_profile_test tools/test_transport_profile.c src/nb_transport_profile.c)
  target_include_directories(nb_transport_profile_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(nb_transport_profile_test PRIVATE _GNU_SOURCE)
  target_compile_options(nb_transport_profile_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_transport_profile COMMAND nb_transport_profile_test)

  add_executable(nb_transport_seed_test tools/test_transport_seed.c src/nb_transport_seed.c)
  target_include_directories(nb_transport_seed_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_transport_seed_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_transport_seed COMMAND nb_transport_seed_test)

  add_executable(nb_routes_test tools/test_routes.c src/nb_routes.c)
  target_include_directories(nb_routes_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_routes_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_routes COMMAND nb_routes_test)

  add_executable(nb_udp_test tools/test_udp.c src/nb_udp.c)
  target_include_directories(nb_udp_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_udp_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_udp COMMAND nb_udp_test)

  add_executable(nb_udp_probe_echo_test tools/test_udp_probe_echo.c src/nb_udp_probe_echo.c src/nb_udp.c)
  target_include_directories(nb_udp_probe_echo_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_udp_probe_echo_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_udp_probe_echo COMMAND nb_udp_probe_echo_test)

  add_executable(nb_udp_queue_test tools/test_udp_queue.c src/nb_udp_queue.c)
  target_include_directories(nb_udp_queue_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_udp_queue_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_udp_queue COMMAND nb_udp_queue_test)

  add_executable(nb_udp_fec_test tools/test_udp_fec.c src/nb_udp_fec.c src/nb_udp.c src/nb_fec_rs.c)
  target_include_directories(nb_udp_fec_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_udp_fec_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_udp_fec_test Threads::Threads)
  add_test(NAME nb_udp_fec COMMAND nb_udp_fec_test)

  add_executable(nb_yfe2_profile_test tools/test_yfe2_profile.c src/nb_yfe2_profile.c)
  target_include_directories(nb_yfe2_profile_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_yfe2_profile_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_yfe2_profile_test OpenSSL::Crypto)
  add_test(NAME nb_yfe2_profile COMMAND nb_yfe2_profile_test)

  add_executable(nb_yfe2_negotiation_test tools/test_yfe2_negotiation.c
    src/nb_yfe2_negotiation.c src/nb_yfe2_profile.c)
  target_include_directories(nb_yfe2_negotiation_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_yfe2_negotiation_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_yfe2_negotiation_test OpenSSL::Crypto)
  add_test(NAME nb_yfe2_negotiation COMMAND nb_yfe2_negotiation_test)

  add_executable(nb_yfe2_wire_test tools/test_yfe2_wire.c src/nb_yfe2_wire.c)
  target_include_directories(nb_yfe2_wire_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_yfe2_wire_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_yfe2_wire COMMAND nb_yfe2_wire_test)

  add_executable(nb_yfe2_block_test tools/test_yfe2_block.c src/nb_yfe2_block.c
    src/nb_yfe2_wire.c src/nb_fec_rs.c)
  target_include_directories(nb_yfe2_block_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_yfe2_block_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_yfe2_block_test Threads::Threads)
  add_test(NAME nb_yfe2_block COMMAND nb_yfe2_block_test)

  add_executable(nb_yfe2_worker_test tools/test_yfe2_worker.c src/nb_yfe2_worker.c
    src/nb_yfe2_block.c src/nb_yfe2_wire.c src/nb_fec_rs.c)
  target_include_directories(nb_yfe2_worker_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(nb_yfe2_worker_test PRIVATE _GNU_SOURCE)
  target_compile_options(nb_yfe2_worker_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_yfe2_worker_test Threads::Threads)
  add_test(NAME nb_yfe2_worker COMMAND nb_yfe2_worker_test)

  add_executable(nb_yfe2_rx_test tools/test_yfe2_rx.c src/nb_yfe2_rx.c
    src/nb_yfe2_block.c src/nb_yfe2_wire.c src/nb_udp.c src/nb_fec_rs.c)
  target_include_directories(nb_yfe2_rx_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_yfe2_rx_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_yfe2_rx_test Threads::Threads)
  add_test(NAME nb_yfe2_rx COMMAND nb_yfe2_rx_test)

  add_executable(nb_yfe2_adaptive_test tools/test_yfe2_adaptive.c src/nb_yfe2_adaptive.c)
  target_include_directories(nb_yfe2_adaptive_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_yfe2_adaptive_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_yfe2_adaptive COMMAND nb_yfe2_adaptive_test)

  add_executable(nb_yfe2_node_contract_test tools/test_yfe2_node_contract.c
    src/nb_yfe2_node.c src/nb_udp.c src/nb_yfe2_negotiation.c src/nb_yfe2_profile.c)
  target_include_directories(nb_yfe2_node_contract_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_yfe2_node_contract_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_yfe2_node_contract_test OpenSSL::Crypto)
  add_test(NAME nb_yfe2_node_contract COMMAND nb_yfe2_node_contract_test)

  add_executable(nb_yfe2_metrics_test tools/test_yfe2_metrics.c src/nb_yfe2_metrics.c
    src/nb_yfe2_negotiation.c src/nb_yfe2_profile.c src/nb_yfe2_adaptive.c)
  target_include_directories(nb_yfe2_metrics_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_yfe2_metrics_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_yfe2_metrics_test OpenSSL::Crypto)
  add_test(NAME nb_yfe2_metrics COMMAND nb_yfe2_metrics_test)

  add_executable(nb_lstream_test tools/test_lstream.c src/nb_lstream.c src/nb_tenant_wire.c)
  target_include_directories(nb_lstream_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_lstream_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_lstream COMMAND nb_lstream_test)

  add_executable(nb_v2_metadata_test tools/test_v2_metadata.c src/nb_v2_metadata.c)
  target_include_directories(nb_v2_metadata_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_v2_metadata_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_v2_metadata COMMAND nb_v2_metadata_test)

  add_executable(nb_pmtu_test tools/test_pmtu.c src/nb_pmtu.c)
  target_include_directories(nb_pmtu_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_pmtu_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_pmtu COMMAND nb_pmtu_test)

  add_executable(nb_udp_io_test tools/test_udp_io.c src/nb_udp_io.c)
  target_include_directories(nb_udp_io_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(nb_udp_io_test PRIVATE _GNU_SOURCE)
  target_compile_options(nb_udp_io_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_udp_io COMMAND nb_udp_io_test)

  add_executable(nb_udp_lifecycle_test tools/test_udp_lifecycle.c src/nb_udp.c src/nb_udp_lifecycle.c)
  target_include_directories(nb_udp_lifecycle_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_udp_lifecycle_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_udp_lifecycle COMMAND nb_udp_lifecycle_test)

  add_executable(nb_policy_test tools/test_policy.c src/nb_policy.c)
  target_include_directories(nb_policy_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_policy_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_policy COMMAND nb_policy_test)

  add_executable(nb_live_test tools/test_live.c src/nb_live.c)
  target_include_directories(nb_live_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_live_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_live COMMAND nb_live_test)

  add_executable(nb_wait_test tools/test_wait.c src/nb_wait.c)
  target_include_directories(nb_wait_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_definitions(nb_wait_test PRIVATE _GNU_SOURCE)
  target_compile_options(nb_wait_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_wait COMMAND nb_wait_test)

  add_executable(nb_probe_test tools/test_probe.c src/nb_probe.c)
  target_include_directories(nb_probe_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_probe_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_probe COMMAND nb_probe_test)

  add_executable(nb_metrics_test tools/test_metrics.c src/nb_metrics.c
    src/nb_yfe2_metrics.c src/nb_yfe2_negotiation.c src/nb_yfe2_profile.c src/nb_yfe2_adaptive.c)
  target_include_directories(nb_metrics_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_metrics_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_metrics_test OpenSSL::Crypto)
  add_test(NAME nb_metrics COMMAND nb_metrics_test)

  add_executable(nb_session_index_test tools/test_session_index.c src/nb_session_index.c)
  target_include_directories(nb_session_index_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_session_index_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_session_index COMMAND nb_session_index_test)

  add_executable(nb_session_memory_test tools/test_session_memory.c)
  target_include_directories(nb_session_memory_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/picoquic/src/picoquic
    ${CMAKE_SOURCE_DIR}/third_party/picoquic/src/loglib
    ${CMAKE_SOURCE_DIR}/third_party/picoquic/src/picotls/include)
  target_compile_options(nb_session_memory_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_session_memory COMMAND nb_session_memory_test)

  add_executable(nb_bridge_test tools/test_bridge.c src/nb_bridge.c)
  target_include_directories(nb_bridge_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_bridge_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_bridge COMMAND nb_bridge_test)

  add_executable(nb_send_test tools/test_send.c src/nb_send.c src/nb_ring.c src/nb_live.c)
  target_include_directories(nb_send_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_send_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_send COMMAND nb_send_test)

  add_executable(nb_path_test tools/test_path.c src/nb_path.c)
  target_include_directories(nb_path_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_path_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_path COMMAND nb_path_test)

  add_executable(nb_pool_health_test tools/test_pool_health.c src/nb_pool_health.c)
  target_include_directories(nb_pool_health_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_pool_health_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_pool_health COMMAND nb_pool_health_test)

  add_executable(nb_dns_test tools/test_dns.c src/nb_dns.c)
  target_include_directories(nb_dns_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_dns_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_dns_test Threads::Threads resolv)
  add_test(NAME nb_dns COMMAND nb_dns_test)

  add_executable(nb_tenant_test tools/test_tenant.c src/nb_tenant.c src/nb_tenant_shared.c)
  target_include_directories(nb_tenant_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_tenant_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_tenant COMMAND nb_tenant_test)

  add_executable(nb_whitelist_test tools/test_whitelist.c src/nb_whitelist.c)
  target_include_directories(nb_whitelist_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(nb_whitelist_test PRIVATE -O2 -Wall -Wextra)
  add_test(NAME nb_whitelist COMMAND nb_whitelist_test)

  add_executable(nb_shard_config_test tools/test_shard_config.c src/nb_shard.c src/nb_instance.c src/nb_transport_profile.c src/log/log4c.c)
  target_include_directories(nb_shard_config_test PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/picoquic/src/picoquic
    ${CMAKE_SOURCE_DIR}/third_party/picoquic/src/loglib
    ${CMAKE_SOURCE_DIR}/third_party/picoquic/src/picotls/include)
  target_compile_definitions(nb_shard_config_test PRIVATE _GNU_SOURCE)
  target_compile_options(nb_shard_config_test PRIVATE -O2 -Wall -Wextra)
  target_link_libraries(nb_shard_config_test Threads::Threads)
  add_test(NAME nb_shard_config COMMAND nb_shard_config_test)

  foreach(_nb_test_target
      nb_connect_test nb_exit_recovery_test nb_fec_test nb_fec_policy_test nb_fec_rs_test nb_ring_test nb_auth_test nb_control_test nb_transport_profile_test nb_transport_seed_test
      nb_auth_async_test nb_routes_test nb_udp_test nb_udp_probe_echo_test nb_udp_queue_test nb_udp_fec_test nb_yfe2_profile_test nb_yfe2_negotiation_test nb_yfe2_wire_test nb_yfe2_block_test nb_yfe2_worker_test nb_yfe2_rx_test nb_yfe2_adaptive_test nb_yfe2_node_contract_test nb_yfe2_metrics_test nb_lstream_test nb_v2_metadata_test nb_pmtu_test nb_udp_io_test nb_udp_lifecycle_test nb_policy_test nb_live_test nb_wait_test nb_probe_test nb_metrics_test
      nb_session_index_test nb_session_memory_test nb_bridge_test nb_send_test nb_path_test
      nb_pool_health_test nb_dns_test nb_tenant_test nb_whitelist_test nb_shard_config_test)
    target_compile_options(${_nb_test_target} PRIVATE -UNDEBUG -Werror)
  endforeach()
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

add_executable(nb_node src/nb_node.c src/nb_instance.c src/nb_transport_profile.c src/nb_transport_seed.c src/nb_shard.c src/nb_policy.c src/nb_live.c src/nb_wait.c src/nb_probe.c src/nb_udp_probe_echo.c src/nb_lstream.c src/nb_metrics.c src/nb_session_index.c src/nb_bridge.c src/nb_send.c src/nb_path.c src/nb_pool.c src/nb_pool_health.c src/nb_pmtu.c src/nb_dns.c src/nb_fec.c src/nb_fec_policy.c src/nb_fec_rs.c src/nb_yfe2_profile.c src/nb_yfe2_negotiation.c src/nb_yfe2_wire.c src/nb_yfe2_block.c src/nb_yfe2_worker.c src/nb_yfe2_rx.c src/nb_yfe2_adaptive.c src/nb_yfe2_node.c src/nb_yfe2_metrics.c src/nb_ring.c src/nb_auth.c src/nb_auth_async.c src/nb_control.c src/nb_routes.c src/nb_tenant.c src/nb_tenant_shared.c src/nb_tenant_wire.c src/nb_udp.c src/nb_udp_queue.c src/nb_udp_fec.c src/nb_udp_io.c src/nb_udp_lifecycle.c src/nb_whitelist.c src/log/log4c.c)
target_link_libraries(nb_node nb_connect)
target_compile_definitions(nb_node PRIVATE _GNU_SOURCE NB_VERSION_PRODUCT="V200R001C03" NB_VERSION_SEMANTIC="${PROJECT_VERSION}")
target_compile_options(nb_node PRIVATE -O2 -Wall -Wextra -Werror)
target_include_directories(nb_node PRIVATE
  ${CMAKE_SOURCE_DIR}/src
  ${PQ_DIR}/src/picoquic
  ${PQ_DIR}/src/loglib
  ${PQ_DIR}/src/picotls/include)
if(BUILD_TESTING)
  add_test(NAME nb_node_version COMMAND nb_node --version)
  set_tests_properties(nb_node_version PROPERTIES PASS_REGULAR_EXPRESSION "V200R001C03 \\(2.1.3\\)")
endif()

set(PQ_LIBS
  ${PQ_PREBUILT}/libpicoquic-core.a
  ${PQ_PREBUILT}/libpicoquic-log.a
  ${PQ_PREBUILT}/libpicotls-openssl.a
  ${PQ_PREBUILT}/libpicotls-core.a
  ${PQ_PREBUILT}/libpicotls-minicrypto.a)

if(BUILD_TESTING)
  add_executable(nb_yfe2_pico_limit_test tools/test_yfe2_pico_limit.c)
  target_include_directories(nb_yfe2_pico_limit_test PRIVATE
    ${PQ_DIR}/src/picoquic ${PQ_DIR}/src/loglib ${PQ_DIR}/src/picotls/include)
  target_compile_options(nb_yfe2_pico_limit_test PRIVATE -O2 -Wall -Wextra -Werror)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_link_libraries(nb_yfe2_pico_limit_test
      -Wl,--start-group ${PQ_LIBS} -Wl,--end-group
      OpenSSL::SSL OpenSSL::Crypto Threads::Threads resolv)
  else()
    target_link_libraries(nb_yfe2_pico_limit_test ${PQ_LIBS}
      OpenSSL::SSL OpenSSL::Crypto Threads::Threads resolv)
  endif()
  add_test(NAME nb_yfe2_pico_limit COMMAND nb_yfe2_pico_limit_test)
endif()

# 静态库间循环依赖 -> GNU ld group(Linux)。非 GNU ld 平台(如 macOS ld64)可去掉 group。
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
  target_link_libraries(nb_node
    -Wl,--start-group ${PQ_LIBS} -Wl,--end-group
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads resolv)
else()
  target_link_libraries(nb_node ${PQ_LIBS}
    OpenSSL::SSL OpenSSL::Crypto Threads::Threads resolv)
endif()

if(BUILD_TESTING AND UNIX)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME nb_shard_runtime
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/test_shard_runtime.py
              --binary $<TARGET_FILE:nb_node>)
    set_tests_properties(nb_shard_runtime PROPERTIES TIMEOUT 90)
  endif()
endif()
