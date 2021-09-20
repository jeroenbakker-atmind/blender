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

#include <ostream>

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
class Value {
 private:
  ValueType _type;
  union {
    std::string _string_value = "";
    int64_t _int_value;
    double _double;
    bool _boolean;
  };
  Vector<Value *> _array_items;
  Map<std::string, Value *> _attributes;

 public:
  Value(ValueType type, ...) : _type(type)
  {
    switch (_type) {
      case ValueType::String: {
        va_list va;
        va_start(va, type);
        _string_value = va_arg(va, std::string);
        va_end(va);
        break;
      }

      case ValueType::Int: {
        va_list va;
        va_start(va, type);
        _int_value = va_arg(va, uint64_t);
        va_end(va);
        break;
      }

      case ValueType::Float: {
        va_list va;
        va_start(va, type);
        _double = va_arg(va, double);
        va_end(va);
        break;
      }

      case ValueType::Boolean: {
        va_list va;
        va_start(va, type);
        /* Compiler promotes bools to ints. */
        _boolean = va_arg(va, int);
        va_end(va);
        break;
      }
      case ValueType::Null: {
        break;
      }

      case ValueType::Array: {
        break;
      }

      case ValueType::Object: {
        break;
      }
    }
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

  const std::string &string_value() const
  {
    BLI_assert(_type == ValueType::String);
    return _string_value;
  }

  const int64_t int_value() const
  {
    BLI_assert(_type == ValueType::Int);
    return _int_value;
  }

  const bool boolean_value() const
  {
    BLI_assert(_type == ValueType::Boolean);
    return _boolean;
  }

  const double float_value() const
  {
    BLI_assert(_type == ValueType::Float);
    return _double;
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
};

class JsonFormatter {
 public:
  uint8_t indentation_len = 0;

 public:
  void serialize(std::ostream &os, Value &value);
};

}  // namespace blender::io::serialize
