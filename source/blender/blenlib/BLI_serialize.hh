/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#pragma once

/** \file
 * \ingroup bli
 *
 * An abstraction layer between serialization formats. Currently only
 * support JSON.
 */

#include "BLI_map.hh"
#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include <optional>
#include <ostream>

#pragma once

namespace blender::io::serialize {

enum class ValueType {
  String,
  Int,
  Array,
  Null,
  Boolean,
  Float,
  Object,
};

// TODO: Should we use inheritance or the current flat structure. The current implementation was
// done to keep the structure as flat as possible. Adding inheritance might lead to more
// allocations. but keeps the code readability more clear...
class Value;
class StringValue;
template<typename T, ValueType V> class PrimitiveValue;
using IntValue = PrimitiveValue<uint64_t, ValueType::Int>;
using FloatValue = PrimitiveValue<double, ValueType::Float>;
using BooleanValue = PrimitiveValue<bool, ValueType::Boolean>;

class Value {
 private:
  ValueType _type;
  Vector<Value *> _array_items;
  Map<std::string, Value *> _attributes;

 public:
  Value(ValueType type) : _type(type)
  {
  }

  ~Value()
  {
    while (!_array_items.is_empty()) {
      Value *value = _array_items.pop_last();
      delete value;
    }
    for (Map<std::string, Value *>::Item item : _attributes.items()) {
      delete item.value;
      // item.value = nullptr;
    }
    _attributes.clear();
  }

  const ValueType type() const
  {
    return _type;
  }

  const Vector<Value *> &array_items() const
  {
    BLI_assert(_type == ValueType::Array);
    return _array_items;
  }
  Vector<Value *> &array_items()
  {
    BLI_assert(_type == ValueType::Array);
    return _array_items;
  }

  const Map<std::string, Value *> &attributes() const
  {
    BLI_assert(_type == ValueType::Object);
    return _attributes;
  }

  Map<std::string, Value *> &attributes()
  {
    BLI_assert(_type == ValueType::Object);
    return _attributes;
  }

  const StringValue *as_string_value() const;
  const IntValue *as_int_value() const;
  const FloatValue *as_float_value() const;
  const BooleanValue *as_boolean_value() const;
};

template<typename T, ValueType V> class PrimitiveValue : public Value {
 private:
  T _inner_value;

 public:
  PrimitiveValue(const T value) : Value(V), _inner_value(value)
  {
  }

  const T value() const
  {
    return _inner_value;
  }
};

class StringValue : public Value {
 private:
  std::string _string;

 public:
  StringValue(const StringRef string) : Value(ValueType::String), _string(string)
  {
  }

  const std::string &string_value() const
  {
    return _string;
  }
};

class JsonFormatter {
 public:
  uint8_t indentation_len = 0;

 public:
  void serialize(std::ostream &os, Value &value);
};

}  // namespace blender::io::serialize
