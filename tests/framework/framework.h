#pragma once

// Minimal test framework. Deliberately dependency-free: Ceres-C has no package manager and its
// build is CMake alone, so pulling in Catch2 or doctest would cost more in build plumbing than
// these ninety lines cost to maintain. Copied from Ceres/tests/framework/framework.h (§12 of the
// architecture plan) rather than reinvented - same TEST()/CHECK_EQ() pattern.
//
// Usage:
//     TEST(lexer, maximal_munch_prefers_shl_eq_over_shl)
//     {
//         CHECK_EQ(token.kind(), TokenKind::ShlEq);
//     }
//
// A failing CHECK records the failure and keeps going, so one run reports every problem in a
// test instead of only the first.

#include <ceresc/support/types.h>

#include <cstdio>
#include <format>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace ceresc::testing
{
	struct TestCase
	{
		std::string_view suite;
		std::string_view name;
		std::function<void()> body;

		// Set for a test that pins a bug that is still open. It is expected to fail; the run
		// only goes red if it starts passing, which is the signal to delete the marker.
		bool expectedToFail = false;
		std::string_view reason;
	};

	class Registry
	{
	private:
		std::vector<TestCase> _tests;
		std::vector<std::string> _failures;   // failures of the test currently running
		usize _checks = 0;

	public:
		static Registry& instance()
		{
			static Registry registry;
			return registry;
		}

		int add(TestCase&& testCase)
		{
			_tests.push_back(std::move(testCase));
			return 0;
		}

		void recordCheck() noexcept { ++_checks; }
		void recordFailure(std::string message) { _failures.push_back(std::move(message)); }

		// Returns the process exit code: 0 when everything passed.
		int runAll(std::string_view suiteFilter = {})
		{
			usize passed = 0;
			usize failed = 0;
			usize skipped = 0;
			usize known = 0;

			for (const auto& test : _tests)
			{
				if (!suiteFilter.empty() && test.suite != suiteFilter)
				{
					++skipped;
					continue;
				}

				_failures.clear();
				const usize checksBefore = _checks;

				try
				{
					test.body();
				}
				catch (const std::exception& e)
				{
					_failures.push_back(std::format("threw std::exception: {}", e.what()));
				}
				catch (...)
				{
					_failures.push_back("threw an unknown exception");
				}

				const bool testFailed = !_failures.empty();

				if (test.expectedToFail)
				{
					if (testFailed)
					{
						++known;
						std::printf("  known %s / %s  (%.*s)\n",
							std::string(test.suite).c_str(), std::string(test.name).c_str(),
							static_cast<int>(test.reason.size()), test.reason.data());
					}
					else
					{
						// The bug it documents is gone. That is good news, but the marker now lies.
						++failed;
						std::printf("  FAIL  %s / %s\n",
							std::string(test.suite).c_str(), std::string(test.name).c_str());
						std::printf("          unexpectedly passed: %.*s appears fixed, drop TEST_KNOWN_FAILURE\n",
							static_cast<int>(test.reason.size()), test.reason.data());
					}
					continue;
				}

				if (!testFailed)
				{
					++passed;
					std::printf("  ok    %s / %s (%zu checks)\n",
						std::string(test.suite).c_str(), std::string(test.name).c_str(), _checks - checksBefore);
				}
				else
				{
					++failed;
					std::printf("  FAIL  %s / %s\n",
						std::string(test.suite).c_str(), std::string(test.name).c_str());
					for (const auto& failure : _failures)
						std::printf("          %s\n", failure.c_str());
				}
			}

			std::printf("\n%zu passed, %zu failed", passed, failed);
			if (known > 0)
				std::printf(", %zu known-failing", known);
			if (skipped > 0)
				std::printf(", %zu skipped", skipped);
			std::printf(" (%zu checks)\n", _checks);

			return failed == 0 ? 0 : 1;
		}
	};

	inline void check(bool condition, std::string_view expression, std::string_view file, int line, std::string detail = {})
	{
		Registry::instance().recordCheck();
		if (condition)
			return;

		std::string message = std::format("{}:{}: {}", file, line, expression);
		if (!detail.empty())
			message += std::format("\n            {}", detail);
		Registry::instance().recordFailure(std::move(message));
	}
}

#define CERESC_TEST_CONCAT_(a, b) a##b
#define CERESC_TEST_CONCAT(a, b) CERESC_TEST_CONCAT_(a, b)

#define TEST(suiteName, testName)                                                             \
	static void CERESC_TEST_CONCAT(ceresc_test_, __LINE__)();                                 \
	static const int CERESC_TEST_CONCAT(ceresc_test_reg_, __LINE__) =                         \
		::ceresc::testing::Registry::instance().add(                                          \
			{ #suiteName, #testName, &CERESC_TEST_CONCAT(ceresc_test_, __LINE__) });          \
	static void CERESC_TEST_CONCAT(ceresc_test_, __LINE__)()

// Pins a bug that is still open. The body asserts the *correct* behaviour, so the test fails
// today by design; when the bug is fixed the run goes red and the marker gets deleted.
#define TEST_KNOWN_FAILURE(suiteName, testName, whyText)                                      \
	static void CERESC_TEST_CONCAT(ceresc_test_, __LINE__)();                                 \
	static const int CERESC_TEST_CONCAT(ceresc_test_reg_, __LINE__) =                         \
		::ceresc::testing::Registry::instance().add(                                          \
			{ #suiteName, #testName, &CERESC_TEST_CONCAT(ceresc_test_, __LINE__), true, whyText }); \
	static void CERESC_TEST_CONCAT(ceresc_test_, __LINE__)()

#define CHECK(cond) ::ceresc::testing::check((cond), #cond, __FILE__, __LINE__)

#define CHECK_EQ(actual, expected)                                                             \
	do {                                                                                       \
		const auto cerescActual_ = (actual);                                                   \
		const auto cerescExpected_ = (expected);                                               \
		::ceresc::testing::check(cerescActual_ == cerescExpected_,                             \
			#actual " == " #expected, __FILE__, __LINE__,                                      \
			std::format("actual: {}\n            expected: {}", cerescActual_, cerescExpected_)); \
	} while (false)

// Same as CHECK_EQ but renders both sides with the caller's own formatting function, so e.g. a
// Token can be compared while shown as its kind name instead of as a raw enum value.
#define CHECK_EQ_FMT(actual, expected, renderer)                                              \
	do {                                                                                       \
		const auto cerescActual_ = (actual);                                                   \
		const auto cerescExpected_ = (expected);                                               \
		::ceresc::testing::check(cerescActual_ == cerescExpected_,                             \
			#actual " == " #expected, __FILE__, __LINE__,                                      \
			std::format("actual:   {}\n            expected: {}",                              \
				renderer(cerescActual_), renderer(cerescExpected_)));                          \
	} while (false)
