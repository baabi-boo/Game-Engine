# Archives contain the install tree: a runnable host, its fixture, public
# headers, static libraries, CMake exports, and project notices. Version 0.1.0
# identifies this foundation build; it does not imply a production release.
set(CPACK_PACKAGE_NAME "GameEngine")
set(CPACK_PACKAGE_VENDOR "Game Engine contributors")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Independent headless engine foundation (pre-alpha)")
set(CPACK_GENERATOR "ZIP;TGZ")
set(CPACK_SOURCE_GENERATOR "")
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}"
)
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/packages")
set(CPACK_PACKAGE_CHECKSUM SHA256)
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY ON)
set(CPACK_MONOLITHIC_INSTALL ON)
set(CPACK_PACKAGING_INSTALL_PREFIX "/")
include(CPack)
