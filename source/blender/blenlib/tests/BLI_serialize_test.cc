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

TEST(serialize, null_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Value test_value;
  json.serialize(out, test_value);
  EXPECT_EQ(out.str(), "null");
}

TEST(serialize, array_to_json)
{
  JsonFormatter json;
  std::stringstream out;
  Vector<Value *> array;
  array.append_as(new Value(42));
  array.append_as(new Value(std::string("Hello JSON")));
  Value value_array(array);

  json.serialize(out, value_array);
  EXPECT_EQ(out.str(), "[42,\"Hello JSON\"]");
}

}  // namespace blender::io::serialize::json::testing
