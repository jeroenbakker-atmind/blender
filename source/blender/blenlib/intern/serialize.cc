#include "BLI_serialize.hh"

#include "json.hpp"

namespace blender::io::serialize {

static void convert_to_json(nlohmann::json &j, const Value &value)
{
  switch (value.type()) {
    case ValueType::String: {
      j = value.string_value();
      break;
    }
    case ValueType::Int: {
      j = value.int_value();
      break;
    }
  }
}

void JsonFormatter::serialize(std::ostream &os, Value &value)
{
  nlohmann::json j;
  convert_to_json(j, value);
  os << j.dump(2);
}

}  // namespace blender::io::serialize
