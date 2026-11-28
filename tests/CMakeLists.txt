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

add_test(NAME ge.package_contract_audit
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_package_contract_audit.py"
)
set_tests_properties(ge.package_contract_audit PROPERTIES LABELS "python;package;contract" TIMEOUT 60)

include("${PROJECT_SOURCE_DIR}/cmake/StateIoFaultTests.cmake")

# Exercise the expanded engine through its public interfaces. The coordinator
# keeps this manifest explicit so a new test cannot silently miss the build.
function(ge_add_runtime_test name source)
    add_executable("ge_${name}_tests" "${source}" ${ARGN})
    target_link_libraries("ge_${name}_tests" PRIVATE ge_engine)
    ge_target_defaults("ge_${name}_tests")
    add_test(NAME "ge.${name}" COMMAND "ge_${name}_tests")
    set_tests_properties("ge.${name}" PROPERTIES LABELS "native;runtime;contract" TIMEOUT 60)
endfunction()

ge_add_runtime_test(batching_audit batching_audit_tests.cpp)
ge_add_runtime_test(render_graph_audit render_graph_audit_tests.cpp)
ge_add_runtime_test(lighting_audit lighting_audit_tests.cpp)
ge_add_runtime_test(postprocess_audit postprocess_audit_tests.cpp)
ge_add_runtime_test(decals_audit decals_audit_tests.cpp)
ge_add_runtime_test(animation_skeleton_audit animation_skeleton_audit_tests.cpp)
ge_add_runtime_test(animation_blend_audit animation_blend_audit_tests.cpp)
ge_add_runtime_test(animation_curves_audit animation_curves_audit_tests.cpp)
ge_add_runtime_test(animation_retarget_audit animation_retarget_audit_tests.cpp)
ge_add_runtime_test(math_from_to_audit math_from_to_audit_tests.cpp)
ge_add_runtime_test(animation_ik_audit animation_ik_audit_tests.cpp)
ge_add_runtime_test(skinning_integration_audit skinning_integration_audit_tests.cpp)
ge_add_runtime_test(ecs_instance_audit ecs_instance_audit_tests.cpp)
ge_add_runtime_test(navigation_navmesh_audit navigation_navmesh_audit_tests.cpp)
ge_add_runtime_test(animation_owner_audit animation_owner_audit_tests.cpp)
ge_add_runtime_test(terrain_heightfield_audit terrain_heightfield_audit_tests.cpp)
ge_add_runtime_test(terrain_vegetation_audit terrain_vegetation_audit_tests.cpp)
ge_add_runtime_test(effects_particles_audit effects_particles_audit_tests.cpp)
ge_add_runtime_test(script_vm_audit script_vm_audit_tests.cpp)
ge_add_runtime_test(config_settings_audit config_settings_audit_tests.cpp)
ge_add_runtime_test(text_localization_audit text_localization_audit_tests.cpp)
ge_add_runtime_test(network_protocol_audit network_protocol_audit_tests.cpp)
ge_add_runtime_test(network_transport_audit network_transport_audit_tests.cpp)
ge_add_runtime_test(network_replication_audit network_replication_audit_tests.cpp)
ge_add_runtime_test(network_prediction_audit network_prediction_audit_tests.cpp)
ge_add_runtime_test(persistence_journal_audit persistence_journal_audit_tests.cpp)
ge_add_runtime_test(ai_behavior_audit ai_behavior_audit_tests.cpp)
ge_add_runtime_test(ai_perception_audit ai_perception_audit_tests.cpp)
ge_add_runtime_test(traffic_roads_audit traffic_roads_audit_tests.cpp)
ge_add_runtime_test(traffic_simulation_audit traffic_simulation_audit_tests.cpp)
ge_add_runtime_test(gameplay_interaction_audit gameplay_interaction_audit_tests.cpp)
ge_add_runtime_test(gameplay_inventory_audit gameplay_inventory_audit_tests.cpp)
ge_add_runtime_test(gameplay_equipment_audit gameplay_equipment_audit_tests.cpp)
ge_add_runtime_test(gameplay_combat_audit gameplay_combat_audit_tests.cpp)
ge_add_runtime_test(gameplay_dialogue_audit gameplay_dialogue_audit_tests.cpp)
ge_add_runtime_test(gameplay_quests_audit gameplay_quests_audit_tests.cpp)
ge_add_runtime_test(editor_selection_audit editor_selection_audit_tests.cpp)
ge_add_runtime_test(jobs_lifecycle_audit jobs_lifecycle_audit_tests.cpp)
ge_add_runtime_test(io_lifecycle_audit io_lifecycle_audit_tests.cpp)
ge_add_runtime_test(binary_limits_audit binary_limits_audit_tests.cpp)
ge_add_runtime_test(signals_lifecycle_audit signals_lifecycle_audit_tests.cpp)
ge_add_runtime_test(asset_catalog_audit asset_catalog_audit_tests.cpp)
ge_add_runtime_test(json_diagnostics_audit json_diagnostics_audit_tests.cpp)
ge_add_runtime_test(gameplay_integration_audit gameplay_integration_audit_tests.cpp)
ge_add_runtime_test(network_integration_audit network_integration_audit_tests.cpp)
ge_add_runtime_test(terrain_integration_audit terrain_integration_audit_tests.cpp)
ge_add_runtime_test(animation_pipeline_audit animation_pipeline_audit_tests.cpp)
ge_add_runtime_test(io_audit io_audit_tests.cpp)
ge_add_runtime_test(input_audit input_audit_tests.cpp)
ge_add_runtime_test(asset_streaming_audit asset_streaming_audit_tests.cpp)
ge_add_runtime_test(collision_audit collision_audit_tests.cpp)
ge_add_runtime_test(dynamics_audit dynamics_audit_tests.cpp)
ge_add_runtime_test(character_audit character_audit_tests.cpp)
ge_add_runtime_test(vehicle_audit vehicle_audit_tests.cpp)
ge_add_runtime_test(visibility_audit visibility_audit_tests.cpp)
ge_add_runtime_test(camera_audit camera_audit_tests.cpp)
ge_add_runtime_test(geometry_audit geometry_audit_tests.cpp)
ge_add_runtime_test(constraints_audit constraints_audit_tests.cpp)
ge_add_runtime_test(runtime_kernel runtime_kernel_tests.cpp)
ge_add_runtime_test(jobs_audit jobs_audit_tests.cpp)
ge_add_runtime_test(math_audit math_audit_tests.cpp)
ge_add_runtime_test(binary_audit binary_audit_tests.cpp)
ge_add_runtime_test(skinning_audit skinning_audit_tests.cpp)
ge_add_runtime_test(ecs_audit ecs_audit_tests.cpp)
ge_add_runtime_test(scene_audit scene_audit_tests.cpp)
ge_add_runtime_test(serialization_audit serialization_audit_tests.cpp)
ge_add_runtime_test(memory_audit memory_audit_tests.cpp)
if(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
    target_compile_options(ge_memory_audit_tests PRIVATE -fno-elide-constructors)
endif()
ge_add_runtime_test(runtime_integration runtime_integration_tests.cpp
    "${PROJECT_SOURCE_DIR}/apps/runtime/modules.cpp")
target_include_directories(ge_runtime_integration_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/apps/runtime")

add_test(NAME ge.runtime_cli
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_runtime_cli.py"
        --binary "$<TARGET_FILE:ge_runtime>"
)
set_tests_properties(ge.runtime_cli PROPERTIES LABELS "python;runtime;integration" TIMEOUT 120)

# One translation unit per installed public header verifies that consumers do
# not need undocumented include ordering or application precompiled headers.
set(GE_HEADER_CHECK_SOURCES)
foreach(module_target IN ITEMS ge_core ge_content ge_world ge_engine)
    get_target_property(module_headers "${module_target}" HEADER_SET)
    foreach(header IN LISTS module_headers)
        string(REGEX REPLACE "^.*[/]include[/]" "" include_name "${header}")
        string(MAKE_C_IDENTIFIER "${include_name}" header_id)
        set(check_source "${CMAKE_CURRENT_BINARY_DIR}/public-headers/${header_id}.cpp")
        file(GENERATE OUTPUT "${check_source}" CONTENT "#include <${include_name}>\n")
        list(APPEND GE_HEADER_CHECK_SOURCES "${check_source}")
    endforeach()
endforeach()
add_library(ge_public_headers OBJECT ${GE_HEADER_CHECK_SOURCES})
target_link_libraries(ge_public_headers PRIVATE ge_engine)
ge_target_defaults(ge_public_headers)
