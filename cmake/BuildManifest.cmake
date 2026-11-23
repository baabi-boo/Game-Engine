include_guard(GLOBAL)

# Each configuration hashes its own final executable. An ALL target deliberately
# reruns on every normal build so a changed Git working tree is also observed.
# It depends on the executable and fixture; neither depends on the manifest.
set(GE_BUILD_MANIFEST "${PROJECT_BINARY_DIR}/build-manifest/$<CONFIG>/build-manifest.json")
add_custom_target(ge_build_manifest ALL
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/build_manifest.py"
        --source-dir "${PROJECT_SOURCE_DIR}"
        --binary "$<TARGET_FILE:ge_headless>"
        --fixture "${GE_COOKED_FIXTURE}"
        --output "${GE_BUILD_MANIFEST}"
        --project-name "${PROJECT_NAME}"
        --project-version "${PROJECT_VERSION}"
        --platform "${CMAKE_SYSTEM_NAME}"
        --system-processor "${CMAKE_SYSTEM_PROCESSOR}"
        --compiler-id "${CMAKE_CXX_COMPILER_ID}"
        --compiler-version "${CMAKE_CXX_COMPILER_VERSION}"
        --configuration "$<CONFIG>"
        --strict-warnings "$<BOOL:${GE_STRICT_WARNINGS}>"
        --sanitizers "$<BOOL:${GE_ENABLE_SANITIZERS}>"
    DEPENDS ge_headless ge_cook_fixture
    BYPRODUCTS "${GE_BUILD_MANIFEST}"
    COMMENT "Recording build settings and hashes of the completed artifacts"
    VERBATIM
)

install(FILES "${GE_BUILD_MANIFEST}"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/game-engine"
    COMPONENT Runtime
)
