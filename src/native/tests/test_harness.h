// A framework-free assert harness for photoncore_tests.
//
// Deliberately not GoogleTest or Catch: the repo's rule is "no test framework
// dependency we have to vendor", and the same plain-`main()` shape is what makes
// this the OFFLINE REPRODUCTION HARNESS for anything misbehaving in the patchers —
// the pattern that found the ImGui `CmdListsCount` bug in a minute after two sim
// restarts found nothing. Add a case, build, run, read the assertion.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace photontest {

struct Case {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<Case>& Registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& FailureCount() {
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        Registry().push_back({name, std::move(fn)});
    }
};

// Thrown by a failed assertion so one bad case does not take the run down.
struct Failure {
    std::string message;
};

inline void Fail(const char* file, int line, const std::string& what) {
    std::ostringstream msg;
    msg << file << ":" << line << ": " << what;
    throw Failure{msg.str()};
}

// Rendering a value for the failure message is best-effort: comparing containers
// is worth doing even when they have no `operator<<`, and requiring one would push
// every test toward comparing sizes instead of contents.
template <typename T, typename = void>
struct Streamable : std::false_type {};
template <typename T>
struct Streamable<T, decltype(void(std::declval<std::ostream&>()
                                   << std::declval<const T&>()))>
    : std::true_type {};

template <typename T>
typename std::enable_if<Streamable<T>::value, std::string>::type Show(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

template <typename T>
typename std::enable_if<!Streamable<T>::value, std::string>::type Show(const T&) {
    return "<value of a type with no operator<<>";
}

template <typename T>
std::string Show(const std::vector<T>& v) {
    std::ostringstream os;
    os << "[";
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i) os << ", ";
        os << Show(v[i]);
    }
    os << "]";
    return os.str();
}

template <typename A, typename B>
void ExpectEq(const char* file, int line, const A& got, const B& want,
              const char* gotExpr, const char* wantExpr) {
    if (!(got == want)) {
        std::ostringstream msg;
        msg << "expected " << gotExpr << " == " << wantExpr << "\n"
            << "      got: " << Show(got) << "\n"
            << "     want: " << Show(want);
        Fail(file, line, msg.str());
    }
}

inline int RunAll(int argc, char** argv) {
    const std::string filter = (argc > 1) ? argv[1] : std::string();
    int ran = 0;
    for (const Case& c : Registry()) {
        if (!filter.empty() && c.name.find(filter) == std::string::npos) continue;
        ++ran;
        try {
            c.fn();
            std::cout << "  ok   " << c.name << "\n";
        } catch (const Failure& f) {
            ++FailureCount();
            std::cout << "  FAIL " << c.name << "\n         " << f.message << "\n";
        } catch (const std::exception& e) {
            ++FailureCount();
            std::cout << "  FAIL " << c.name
                      << "\n         unexpected exception: " << e.what() << "\n";
        } catch (...) {
            ++FailureCount();
            std::cout << "  FAIL " << c.name << "\n         unknown exception\n";
        }
    }
    std::cout << "\nran " << ran << " test(s), " << FailureCount()
              << " failure(s)\n";
    return FailureCount() == 0 ? 0 : 1;
}

}  // namespace photontest

#define PHOTON_CONCAT_INNER(a, b) a##b
#define PHOTON_CONCAT(a, b) PHOTON_CONCAT_INNER(a, b)

#define TEST(name)                                                     \
    static void PHOTON_CONCAT(photon_test_fn_, __LINE__)();            \
    static ::photontest::Registrar PHOTON_CONCAT(photon_test_reg_,     \
                                                 __LINE__)(            \
        name, &PHOTON_CONCAT(photon_test_fn_, __LINE__));              \
    static void PHOTON_CONCAT(photon_test_fn_, __LINE__)()

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) ::photontest::Fail(__FILE__, __LINE__,            \
                                        "expected: " #cond);           \
    } while (0)

#define CHECK_EQ(got, want)                                            \
    ::photontest::ExpectEq(__FILE__, __LINE__, (got), (want), #got, #want)

#define CHECK_THROWS(expr, ExceptionType)                              \
    do {                                                               \
        bool photon_threw = false;                                     \
        try {                                                          \
            (void)(expr);                                              \
        } catch (const ExceptionType&) {                               \
            photon_threw = true;                                       \
        }                                                              \
        if (!photon_threw) {                                           \
            ::photontest::Fail(__FILE__, __LINE__,                     \
                               "expected " #expr " to throw " #ExceptionType); \
        }                                                              \
    } while (0)
