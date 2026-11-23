add_executable(ge_core_tests core_tests.cpp)
target_link_libraries(ge_core_tests PRIVATE ge_core)
ge_target_defaults(ge_core_tests)

add_executable(ge_content_tests content_tests.cpp)
target_link_libraries(ge_content_tests PRIVATE ge_content)
ge_target_defaults(ge_content_tests)
add_dependencies(ge_content_tests ge_cook_fixture)
if(MINGW)
    target_link_options(ge_content_tests PRIVATE -municode)
endif()

add_executable(ge_streaming_tests streaming_tests.cpp)
target_link_libraries(ge_streaming_tests PRIVATE ge_world)
ge_target_defaults(ge_streaming_tests)

add_executable(ge_state_tests state_tests.cpp)
target_link_libraries(ge_state_tests PRIVATE ge_world)
ge_target_defaults(ge_state_tests)

add_test(NAME ge.core COMMAND ge_core_tests)
add_test(NAME ge.content COMMAND ge_content_tests
    --golden "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/package-v1.hex"
    --fixture "${GE_COOKED_FIXTURE}"
)
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
add_test(NAME ge.native_package
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_native_package.py"
        --binary "$<TARGET_FILE:ge_headless>"
        --fixture "${GE_COOKED_FIXTURE}"
)
add_test(NAME ge.repository
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_repo.py"
)
add_test(NAME ge.agent_state
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/agent_state.py" check
)
add_test(NAME ge.agent_state_tests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_agent_state.py"
)
add_test(NAME ge.repository_policy_tests
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_repo_policy.py"
)
add_test(NAME ge.build_manifest
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_build_manifest.py"
)
add_test(NAME ge.tool_boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_tool_boundaries.py"
        --binary "$<TARGET_FILE:ge_headless>"
        --fixture "${GE_COOKED_FIXTURE}"
)

set_tests_properties(ge.core ge.content ge.streaming ge.state PROPERTIES
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
set_tests_properties(ge.native_package PROPERTIES
    LABELS "python;content;native-input"
    TIMEOUT 120
)
set_tests_properties(ge.repository ge.repository_policy_tests PROPERTIES
    LABELS "python;repository"
    TIMEOUT 60
)
set_tests_properties(ge.agent_state PROPERTIES
    LABELS "python;repository;workflow"
    TIMEOUT 60
)
set_tests_properties(ge.agent_state_tests PROPERTIES
    LABELS "python;repository;workflow"
    TIMEOUT 120
)
set_tests_properties(ge.build_manifest PROPERTIES
    LABELS "python;build;provenance"
    TIMEOUT 60
)
set_tests_properties(ge.tool_boundaries PROPERTIES
    LABELS "python;integration;tools"
    TIMEOUT 120
)

include("${PROJECT_SOURCE_DIR}/cmake/StateIoFaultTests.cmake")
