include_guard(GLOBAL)

option(GE_STRICT_WARNINGS "Enable strict compiler warnings and treat them as errors" ON)
option(GE_ENABLE_SANITIZERS "Instrument native code with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(GE_ENABLE_PACKAGING "Enable CPack ZIP and TGZ binary archives" ON)

if(GE_ENABLE_SANITIZERS)
    if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        message(FATAL_ERROR
            "GE_ENABLE_SANITIZERS requires a GNU-compatible GCC or Clang driver with both "
            "AddressSanitizer and UndefinedBehaviorSanitizer. MSVC/clang-cl does not support "
            "this combined option. Use the dev or release preset for MSVC builds."
        )
    endif()

    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined -fno-omit-frame-pointer")
    set(CMAKE_REQUIRED_LINK_OPTIONS "-fsanitize=address,undefined")
    check_cxx_source_compiles("int main() { return 0; }" GE_HAS_ASAN_UBSAN)
    cmake_pop_check_state()
    if(NOT GE_HAS_ASAN_UBSAN)
        message(FATAL_ERROR
            "The selected C++ toolchain cannot compile and link with ASan and UBSan. "
            "Install its sanitizer runtimes or configure with GE_ENABLE_SANITIZERS=OFF."
        )
    endif()
endif()

# Apply implementation policy only to repository-owned compilations. Public
# include paths and C++20 requirements are declared separately by each module.
# Tests can call this helper to receive the same warnings and instrumentation.
function(ge_target_defaults target_name)
    if(NOT TARGET "${target_name}")
        message(FATAL_ERROR "ge_target_defaults received an unknown target: ${target_name}")
    endif()
    get_target_property(target_type "${target_name}" TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY" OR target_type STREQUAL "UTILITY")
        message(FATAL_ERROR "ge_target_defaults requires a compiled target: ${target_name}")
    endif()

    target_compile_features("${target_name}" PRIVATE cxx_std_20)
    set_target_properties("${target_name}" PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
    )

    if(MSVC)
        target_compile_options("${target_name}" PRIVATE
            /permissive- /Zc:__cplusplus /utf-8
        )
        if(GE_STRICT_WARNINGS)
            target_compile_options("${target_name}" PRIVATE /W4 /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        if(GE_STRICT_WARNINGS)
            target_compile_options("${target_name}" PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror
            )
        endif()
    elseif(GE_STRICT_WARNINGS)
        message(WARNING
            "No strict warning policy is defined for ${CMAKE_CXX_COMPILER_ID}; "
            "C++20 is still required for ${target_name}."
        )
    endif()

    if(GE_ENABLE_SANITIZERS)
        target_compile_options("${target_name}" PRIVATE
            -fsanitize=address,undefined -fno-omit-frame-pointer
        )
        if(target_type STREQUAL "STATIC_LIBRARY" OR target_type STREQUAL "SHARED_LIBRARY")
            # Instrumented static objects need sanitizer runtimes in the final
            # executable. PUBLIC link options also preserve this requirement in
            # installed targets; private compile warnings never leak to users.
            target_link_options("${target_name}" PUBLIC -fsanitize=address,undefined)
        else()
            target_link_options("${target_name}" PRIVATE -fsanitize=address,undefined)
        endif()
    endif()
endfunction()
