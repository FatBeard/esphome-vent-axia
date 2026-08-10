// Proves the harness itself works before any component code exists.
#include "test_framework.h"

TEST_CASE(harness_runs) {
  CHECK(true);
  CHECK_EQ(2 + 2, 4);
  CHECK_EQ(std::string("Diagnostic  27"), std::string("Diagnostic  27"));
}
