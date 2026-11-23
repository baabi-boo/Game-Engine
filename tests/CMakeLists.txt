add_executable(ge_core_tests core_tests.cpp)
target_link_libraries(ge_core_tests PRIVATE ge_core)
ge_target_defaults(ge_core_tests)

add_executable(ge_streaming_tests streaming_tests.cpp)
target_link_libraries(ge_streaming_tests PRIVATE ge_world)
ge_target_defaults(ge_streaming_tests)

add_executable(ge_state_tests state_tests.cpp)
target_link_libraries(ge_state_tests PRIVATE ge_world)
ge_target_defaults(ge_state_tests)

add_test(NAME ge.core COMMAND ge_core_tests)
add_test(NAME ge.streaming COMMAND ge_streaming_tests)
add_test(NAME ge.state COMMAND ge_state_tests)
add_test(NAME ge.cooker
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_cooker.py"
)
add_test(NAME ge.headless
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_headless.py"
        --binary "$<TARGET_FILE:ge_headless>"
        --fixture "${GE_COOKED_FIXTURE}"
)
add_test(NAME ge.repository
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_repo.py"
)
add_test(NAME ge.repository_policy_tests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_repo_policy.py"
)

set_tests_properties(ge.core ge.streaming ge.state PROPERTIES
    LABELS "native;contract"
    TIMEOUT 60
)
set_tests_properties(ge.cooker PROPERTIES
    LABELS "python;content"
    TIMEOUT 120
)
set_tests_properties(ge.headless PROPERTIES
    LABELS "python;integration"
    TIMEOUT 120
)
set_tests_properties(ge.repository ge.repository_policy_tests PROPERTIES
    LABELS "python;repository"
    TIMEOUT 60
)
