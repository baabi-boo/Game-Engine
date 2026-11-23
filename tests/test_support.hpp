#pragma once

#include <cstddef>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace ge::test {

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline std::size_t checks_run = 0;

[[noreturn]] inline void fail(
    const char* expression, const char* file, int line,
    std::string_view context = {}) {
    std::ostringstream message;
    message << file << ':' << line << ": CHECK(" << expression << ") failed";
    if (!context.empty()) {
        message << " [" << context << ']';
    }
    throw CheckFailure(message.str());
}

inline void check(
    bool condition, const char* expression, const char* file, int line,
    std::string_view context = {}) {
    ++checks_run;
    if (!condition) {
        fail(expression, file, line, context);
    }
}

template <typename Exception, typename Function>
void check_throws_as(
    Function&& function, const char* expression, const char* exception_name,
    const char* file, int line) {
    ++checks_run;
    try {
        std::forward<Function>(function)();
    } catch (const Exception&) {
        return;
    } catch (const std::exception& error) {
        std::ostringstream context;
        context << "expected " << exception_name << "; caught: " << error.what();
        fail(expression, file, line, context.str());
    } catch (...) {
        fail(expression, file, line, "caught an unexpected exception type");
    }
    std::ostringstream context;
    context << "expected " << exception_name << "; no exception was thrown";
    fail(expression, file, line, context.str());
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

inline int run(std::string_view suite, std::initializer_list<TestCase> cases) {
    std::size_t failed = 0;
    const auto initial_checks = checks_run;
    for (const auto& test : cases) {
        try {
            test.function();
            std::cout << "[PASS] " << suite << '/' << test.name << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cerr << "[FAIL] " << suite << '/' << test.name << ": "
                      << error.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << suite << '/' << test.name
                      << ": unexpected non-standard exception\n";
        }
    }
    std::cout << suite << ": " << cases.size() - failed << '/' << cases.size()
              << " cases passed; " << checks_run - initial_checks << " checks\n";
    return failed == 0 ? 0 : 1;
}

} // namespace ge::test

// These checks deliberately do not use assert(): expressions are evaluated and
// failures remain fatal to the test case when Release builds define NDEBUG.
#define CHECK(...) \
    ::ge::test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __FILE__, __LINE__)

#define CHECK_MSG(expression, context) \
    ::ge::test::check(static_cast<bool>((expression)), #expression, __FILE__, __LINE__, (context))

#define CHECK_THROWS_AS(expression, exception_type) \
    ::ge::test::check_throws_as<exception_type>( \
        [&]() { static_cast<void>(expression); }, #expression, #exception_type, __FILE__, __LINE__)
