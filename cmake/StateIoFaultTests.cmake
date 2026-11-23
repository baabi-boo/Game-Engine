include_guard(GLOBAL)

# Test-only interposition: supported on Linux ELF with a GNU-compatible driver,
# libstdc++'s C++11 filesystem ABI, and a linker implementing --wrap. Other
# platforms keep the portable ge.state tests and explicitly skip this suite.
# These are synchronous I/O error tests, not crash or power-loss durability tests.
function(ge_add_state_io_fault_tests)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR
       NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
        message(STATUS
            "ge.state_io_faults not registered: requires Linux ELF, GCC/Clang, "
            "libstdc++ C++11 ABI, and linker --wrap support."
        )
        return()
    endif()

    set(ge_state_wrap_options
        "LINKER:--wrap=fwrite"
        "LINKER:--wrap=fflush"
        "LINKER:--wrap=fclose"
        "LINKER:--wrap=_ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code"
    )

    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED YES)
    set(CMAKE_CXX_EXTENSIONS NO)
    # This probe must link, including when a cross-toolchain normally probes
    # static libraries. The exact __real symbols require functioning --wrap.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set(CMAKE_REQUIRED_LINK_OPTIONS ${ge_state_wrap_options})
    check_cxx_source_compiles([=[
        #include <cstdio>
        #include <filesystem>
        #include <system_error>
        #if !defined(__linux__) || !defined(__ELF__) || !defined(__GLIBCXX__)
        #error Unsupported platform or standard library ABI
        #endif
        #if !defined(_GLIBCXX_USE_CXX11_ABI) || !_GLIBCXX_USE_CXX11_ABI
        #error Unsupported platform or standard library ABI
        #endif
        extern "C" std::size_t __real_fwrite(const void*, std::size_t, std::size_t, std::FILE*);
        extern "C" int __real_fflush(std::FILE*);
        extern "C" int __real_fclose(std::FILE*);
        extern "C" void real_rename(const std::filesystem::path&, const std::filesystem::path&,
                                    std::error_code&) noexcept
            asm("__real__ZNSt10filesystem6renameERKNS_7__cxx114pathES3_RSt10error_code");
        int main() {
            // Compile/link only: no probe I/O is executed at configure time.
            (void)__real_fwrite(nullptr, 0U, 0U, nullptr);
            (void)__real_fflush(nullptr);
            (void)__real_fclose(nullptr);
            const std::filesystem::path path("ge-state-wrap-probe");
            std::error_code error;
            real_rename(path, path, error);
            return 0;
        }
    ]=] GE_HAS_STATE_IO_FAULT_WRAPPING)
    cmake_pop_check_state()
    if(NOT GE_HAS_STATE_IO_FAULT_WRAPPING)
        message(STATUS
            "ge.state_io_faults not registered: the Linux ELF/libstdc++ ABI and "
            "linker --wrap capability probe failed."
        )
        return()
    endif()

    add_executable(ge_state_io_fault_tests "${PROJECT_SOURCE_DIR}/tests/state_io_fault_tests.cpp")
    target_link_libraries(ge_state_io_fault_tests PRIVATE ge_world)
    ge_target_defaults(ge_state_io_fault_tests)
    target_link_options(ge_state_io_fault_tests PRIVATE ${ge_state_wrap_options})
    add_test(NAME ge.state_io_faults COMMAND ge_state_io_fault_tests)
    set_tests_properties(ge.state_io_faults PROPERTIES
        LABELS "native;contract;linux;fault-injection"
        TIMEOUT 60
    )
    message(STATUS "Registered ge.state_io_faults (Linux ELF/libstdc++ linker --wrap).")
endfunction()

ge_add_state_io_fault_tests()
