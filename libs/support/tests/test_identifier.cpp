#include <ceresc/support/identifier.h>

#include "framework.h"

#include <unordered_set>

using namespace ceresc;
using namespace ceresc::support;

namespace
{
	using TestId = GenericIdentifier<u32>;
	using TestIdGenerator = GenericIdentifierGenerator<u32>;
}

TEST(identifier, default_constructed_is_invalid)
{
	TestId id;
	CHECK(!id);
	CHECK(!static_cast<bool>(id));
	CHECK(id == TestId::invalid());
}

TEST(identifier, make_produces_a_valid_nonzero_id)
{
	TestId id = TestId::make(42);
	CHECK(static_cast<bool>(id));
	CHECK_EQ(id.value(), 42u);
}

TEST(identifier, equality_and_ordering_follow_the_underlying_value)
{
	TestId a = TestId::make(1);
	TestId b = TestId::make(2);
	CHECK(a == a);
	CHECK(a != b);
	CHECK(a < b);
}

TEST(identifier, generator_starts_at_one_and_increments)
{
	TestIdGenerator gen;
	TestId first = gen.generate();
	TestId second = gen.generate();
	CHECK_EQ(first.value(), 1u);
	CHECK_EQ(second.value(), 2u);
	CHECK(first != second);
}

TEST(identifier, generator_reset_restarts_the_sequence)
{
	TestIdGenerator gen;
	gen.generate();
	gen.generate();
	gen.reset();
	TestId afterReset = gen.generate();
	CHECK_EQ(afterReset.value(), 1u);
}

TEST(identifier, hash_specialization_agrees_with_equality)
{
	TestId a = TestId::make(7);
	TestId b = TestId::make(7);
	std::hash<TestId> hasher;
	CHECK_EQ(hasher(a), hasher(b));

	std::unordered_set<TestId> ids;
	ids.insert(a);
	CHECK(ids.contains(b));
}
