cmake_minimum_required(VERSION 3.10)
project(BankSync C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

find_package(Threads REQUIRED)

include_directories(include)

add_executable(banksync
    src/main.c
    src/bank.c
)
target_link_libraries(banksync PRIVATE Threads::Threads)

add_executable(test_bank
    tests/test_bank.c
    src/bank.c
)
target_link_libraries(test_bank PRIVATE Threads::Threads)
target_include_directories(test_bank PRIVATE include)

add_executable(test_thread_pool
    tests/test_thread_pool.c
    src/thread_pool.c
)
target_link_libraries(test_thread_pool PRIVATE Threads::Threads)
target_include_directories(test_thread_pool PRIVATE include)
