/* Apache License, Version 2.0 */

#include "testing/testing.h"

#include "BLI_serialize.hh"

/* -------------------------------------------------------------------- */
/* tests */

namespace blender::io::serialize::json::testing {

TEST(serialize, string_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value test_value(std::string("Hello JSON"));
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "\"Hello JSON\"");
}

TEST(serialize, int_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value test_value(42);
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "42");
}

}  // namespace blender::io::serialize::json::testing
