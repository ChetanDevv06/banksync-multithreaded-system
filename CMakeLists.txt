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
    src/application.c
    src/cli.c
    src/money_utils.c
    src/thread_pool.c
    src/transaction_manager.c
    src/session_manager.c
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

add_executable(test_transaction_manager
    tests/test_transaction_manager.c
    src/transaction_manager.c
    src/bank.c
    src/thread_pool.c
)
target_link_libraries(test_transaction_manager PRIVATE Threads::Threads)
target_include_directories(test_transaction_manager PRIVATE include)

add_executable(test_session_manager
    tests/test_session_manager.c
    src/session_manager.c
    src/transaction_manager.c
    src/thread_pool.c
    src/bank.c
)
target_link_libraries(test_session_manager PRIVATE Threads::Threads)
target_include_directories(test_session_manager PRIVATE include)

add_executable(test_race_experiment
    tests/test_race_experiment.c
    src/race_experiment.c
)
target_link_libraries(test_race_experiment PRIVATE Threads::Threads)
target_include_directories(test_race_experiment PRIVATE include)

add_executable(deadlock_helper
    src/deadlock_helper.c
)
target_link_libraries(deadlock_helper PRIVATE Threads::Threads)
target_include_directories(deadlock_helper PRIVATE include)
set_target_properties(deadlock_helper PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(test_deadlock_experiment
    tests/test_deadlock_experiment.c
    src/deadlock_experiment.c
)
target_link_libraries(test_deadlock_experiment PRIVATE Threads::Threads)
target_include_directories(test_deadlock_experiment PRIVATE include)
set_target_properties(test_deadlock_experiment PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_definitions(test_deadlock_experiment PRIVATE
    "DEADLOCK_HELPER_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/deadlock_helper${CMAKE_EXECUTABLE_SUFFIX}\"")

add_executable(test_deadlock_prevention
    tests/test_deadlock_prevention.c
    src/deadlock_prevention.c
)
target_link_libraries(test_deadlock_prevention PRIVATE Threads::Threads)
target_include_directories(test_deadlock_prevention PRIVATE include)
set_target_properties(test_deadlock_prevention PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(test_application
    tests/test_application.c
    src/application.c
    src/bank.c
    src/thread_pool.c
    src/transaction_manager.c
    src/session_manager.c
)
target_link_libraries(test_application PRIVATE Threads::Threads)
target_include_directories(test_application PRIVATE include)
set_target_properties(test_application PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(test_synchronization_report
    tests/test_synchronization_report.c
    src/synchronization_report.c
    src/race_experiment.c
    src/deadlock_experiment.c
    src/deadlock_prevention.c
)
target_link_libraries(test_synchronization_report PRIVATE Threads::Threads)
target_include_directories(test_synchronization_report PRIVATE include)
set_target_properties(test_synchronization_report PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
target_compile_definitions(test_synchronization_report PRIVATE
    "DEADLOCK_HELPER_PATH=\"${CMAKE_CURRENT_BINARY_DIR}/deadlock_helper${CMAKE_EXECUTABLE_SUFFIX}\"")

add_executable(test_cli
    tests/test_cli.c
    src/cli.c
    src/application.c
    src/bank.c
    src/money_utils.c
    src/thread_pool.c
    src/transaction_manager.c
    src/session_manager.c
)
target_link_libraries(test_cli PRIVATE Threads::Threads)
target_include_directories(test_cli PRIVATE include)
set_target_properties(test_cli PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(test_money_utils
    tests/test_money_utils.c
    src/money_utils.c
)
target_link_libraries(test_money_utils PRIVATE Threads::Threads)
target_include_directories(test_money_utils PRIVATE include)
set_target_properties(test_money_utils PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(test_concurrent_sessions
    tests/test_concurrent_sessions.c
    src/application.c
    src/bank.c
    src/thread_pool.c
    src/transaction_manager.c
    src/session_manager.c
)
target_link_libraries(test_concurrent_sessions PRIVATE Threads::Threads)
target_include_directories(test_concurrent_sessions PRIVATE include)
set_target_properties(test_concurrent_sessions PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(test_concurrent_transactions
    tests/test_concurrent_transactions.c
    src/bank.c
    src/thread_pool.c
    src/transaction_manager.c
    src/session_manager.c
)
target_link_libraries(test_concurrent_transactions PRIVATE Threads::Threads)
target_include_directories(test_concurrent_transactions PRIVATE include)
set_target_properties(test_concurrent_transactions PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(test_concurrent_integration
    tests/test_concurrent_integration.c
    src/application.c
    src/bank.c
    src/thread_pool.c
    src/transaction_manager.c
    src/session_manager.c
)
target_link_libraries(test_concurrent_integration PRIVATE Threads::Threads)
target_include_directories(test_concurrent_integration PRIVATE include)
set_target_properties(test_concurrent_integration PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
