cmake_minimum_required(VERSION 3.15)
project(sdv_communication_integration_lab VERSION 0.1.0 LANGUAGES CXX)

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
  cpp/src/fault_injector.cpp
  cpp/src/vehicle_gateway.cpp
)
target_include_directories(sdv_core PUBLIC cpp/include)

add_executable(sdv_gateway_demo cpp/src/main.cpp)
target_link_libraries(sdv_gateway_demo PRIVATE sdv_core)

add_executable(sdv_gateway_cli cpp/src/gateway_cli.cpp)
target_link_libraries(sdv_gateway_cli PRIVATE sdv_core)

enable_testing()
add_executable(sdv_core_tests cpp/tests/test_vehicle_gateway.cpp)
target_link_libraries(sdv_core_tests PRIVATE sdv_core)
add_test(NAME sdv_core_tests COMMAND sdv_core_tests)

