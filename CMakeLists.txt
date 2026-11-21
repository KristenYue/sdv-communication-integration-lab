cmake_minimum_required(VERSION 3.15)
project(sdv_communication_integration_lab VERSION 0.1.0 LANGUAGES CXX)

find_package(Threads REQUIRED)

option(SDV_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wconversion -Wshadow)
  if(SDV_ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
endif()

add_library(sdv_core
  cpp/src/diagnostic_manager.cpp
  cpp/src/fault_injector.cpp
  cpp/src/idempotency_journal.cpp
  cpp/src/someip.cpp
  cpp/src/someip_udp.cpp
  cpp/src/vehicle_gateway.cpp
  cpp/src/vehicle_diagnostic_service.cpp
  cpp/src/vehicle_someip_adapter.cpp
  cpp/src/vehicle_service_runtime.cpp
)
target_include_directories(sdv_core PUBLIC cpp/include)
target_link_libraries(sdv_core PUBLIC Threads::Threads)
if(WIN32)
  target_link_libraries(sdv_core PUBLIC ws2_32)
endif()

add_executable(sdv_gateway_demo cpp/src/main.cpp)
target_link_libraries(sdv_gateway_demo PRIVATE sdv_core)

add_executable(sdv_gateway_cli cpp/src/gateway_cli.cpp)
target_link_libraries(sdv_gateway_cli PRIVATE sdv_core)

add_executable(sdv_service_runtime_demo cpp/src/service_runtime_demo.cpp)
target_link_libraries(sdv_service_runtime_demo PRIVATE sdv_core)

add_executable(sdv_someip_vehicle_server cpp/src/someip_vehicle_server.cpp)
target_link_libraries(sdv_someip_vehicle_server PRIVATE sdv_core)

add_executable(sdv_someip_vehicle_client cpp/src/someip_vehicle_client.cpp)
target_link_libraries(sdv_someip_vehicle_client PRIVATE sdv_core)

add_executable(sdv_someip_body_client cpp/src/someip_body_client.cpp)
target_link_libraries(sdv_someip_body_client PRIVATE sdv_core)

add_executable(sdv_someip_diagnostic_client cpp/src/someip_diagnostic_client.cpp)
target_link_libraries(sdv_someip_diagnostic_client PRIVATE sdv_core)

add_executable(sdv_idempotency_evidence cpp/src/idempotency_evidence.cpp)
target_link_libraries(sdv_idempotency_evidence PRIVATE sdv_core)

add_executable(sdv_restart_recovery_evidence cpp/src/restart_recovery_evidence.cpp)
target_link_libraries(sdv_restart_recovery_evidence PRIVATE sdv_core)

add_executable(sdv_concurrency_evidence cpp/src/concurrency_evidence.cpp)
target_link_libraries(sdv_concurrency_evidence PRIVATE sdv_core)

add_executable(sdv_performance_matrix cpp/src/performance_matrix.cpp)
target_link_libraries(sdv_performance_matrix PRIVATE sdv_core)
if(WIN32)
  target_link_libraries(sdv_performance_matrix PRIVATE psapi)
endif()

add_executable(sdv_diagnostic_evidence cpp/src/diagnostic_evidence.cpp)
target_link_libraries(sdv_diagnostic_evidence PRIVATE sdv_core)

add_executable(sdv_diagnostic_service_evidence cpp/src/diagnostic_service_evidence.cpp)
target_link_libraries(sdv_diagnostic_service_evidence PRIVATE sdv_core)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(sdv_socketcan_gateway cpp/src/socketcan_gateway.cpp)
  target_link_libraries(sdv_socketcan_gateway PRIVATE sdv_core)
endif()

enable_testing()
add_executable(sdv_core_tests cpp/tests/test_vehicle_gateway.cpp)
target_link_libraries(sdv_core_tests PRIVATE sdv_core)
add_test(NAME sdv_core_tests COMMAND sdv_core_tests)

add_executable(sdv_service_runtime_tests cpp/tests/test_vehicle_service_runtime.cpp)
target_link_libraries(sdv_service_runtime_tests PRIVATE sdv_core)
add_test(NAME sdv_service_runtime_tests COMMAND sdv_service_runtime_tests)

add_executable(sdv_someip_tests cpp/tests/test_someip.cpp)
target_link_libraries(sdv_someip_tests PRIVATE sdv_core)
add_test(NAME sdv_someip_tests COMMAND sdv_someip_tests)

add_executable(sdv_vehicle_someip_adapter_tests cpp/tests/test_vehicle_someip_adapter.cpp)
target_link_libraries(sdv_vehicle_someip_adapter_tests PRIVATE sdv_core)
add_test(NAME sdv_vehicle_someip_adapter_tests COMMAND sdv_vehicle_someip_adapter_tests)

add_executable(sdv_diagnostic_manager_tests cpp/tests/test_diagnostic_manager.cpp)
target_link_libraries(sdv_diagnostic_manager_tests PRIVATE sdv_core)
add_test(NAME sdv_diagnostic_manager_tests COMMAND sdv_diagnostic_manager_tests)

