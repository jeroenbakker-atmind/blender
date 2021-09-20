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

    case ValueType::Array: {
      const Vector<Value *> &items = value.array_items();
      for (const Value *item_value : items) {
        nlohmann::json json_item;
        convert_to_json(json_item, *item_value);
        j.push_back(json_item);
      }
      break;
    }

    case ValueType::Null: {
      j = nullptr;
      break;
    }

    case ValueType::Boolean: {
      j = value.boolean_value();
      break;
    }
  }
}

void JsonFormatter::serialize(std::ostream &os, Value &value)
{
  nlohmann::json j;
  convert_to_json(j, value);
  if (indentation_len) {
    os << j.dump(indentation_len);
  }
  else {
    os << j.dump();
  }
}

}  // namespace blender::io::serialize
