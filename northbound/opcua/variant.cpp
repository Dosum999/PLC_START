#include "opcua/variant.h"

#include <cstring>

namespace logict::opcua {

namespace status = sdk::status;
using sdk::DataType;
using sdk::Value;

const UA_DataType* ua_type(DataType t) noexcept {
  switch (t) {
    case DataType::Boolean: return &UA_TYPES[UA_TYPES_BOOLEAN];
    case DataType::Char: return &UA_TYPES[UA_TYPES_SBYTE];
    case DataType::Byte: return &UA_TYPES[UA_TYPES_BYTE];
    case DataType::Short: return &UA_TYPES[UA_TYPES_INT16];
    case DataType::Word:
    case DataType::BCD: return &UA_TYPES[UA_TYPES_UINT16];
    case DataType::Long: return &UA_TYPES[UA_TYPES_INT32];
    case DataType::DWord:
    case DataType::LBCD: return &UA_TYPES[UA_TYPES_UINT32];
    case DataType::LLong: return &UA_TYPES[UA_TYPES_INT64];
    case DataType::QWord: return &UA_TYPES[UA_TYPES_UINT64];
    case DataType::Float: return &UA_TYPES[UA_TYPES_FLOAT];
    case DataType::Double: return &UA_TYPES[UA_TYPES_DOUBLE];
    case DataType::String: return &UA_TYPES[UA_TYPES_STRING];
    case DataType::Date: return &UA_TYPES[UA_TYPES_DATETIME];
    case DataType::Default: break;
  }
  return &UA_TYPES[UA_TYPES_VARIANT];  // BaseDataType
}

UA_DateTime to_ua_time(sdk::Timestamp t) noexcept {
  auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
  return UA_DATETIME_UNIX_EPOCH + ns / 100;
}

namespace {

sdk::Timestamp from_ua_time(UA_DateTime t) {
  return sdk::Timestamp(std::chrono::duration_cast<sdk::Clock::duration>(
      std::chrono::nanoseconds((t - UA_DATETIME_UNIX_EPOCH) * 100)));
}

// Writes one scalar into `dst` (memory of an element of `type`).
UA_StatusCode store_scalar(const Value& v, const UA_DataType* type, void* dst) {
  return std::visit(
      [&](const auto& x) -> UA_StatusCode {
        using T = std::decay_t<decltype(x)>;
        if constexpr (std::is_arithmetic_v<T>) {
          if (type->memSize != sizeof(T)) return UA_STATUSCODE_BADTYPEMISMATCH;
          std::memcpy(dst, &x, sizeof(T));
          return UA_STATUSCODE_GOOD;
        } else if constexpr (std::is_same_v<T, std::string>) {
          if (type != &UA_TYPES[UA_TYPES_STRING]) return UA_STATUSCODE_BADTYPEMISMATCH;
          auto* s = static_cast<UA_String*>(dst);
          UA_String tmp{x.size(), reinterpret_cast<UA_Byte*>(const_cast<char*>(x.data()))};
          return UA_String_copy(&tmp, s);
        } else if constexpr (std::is_same_v<T, sdk::DateTime>) {
          if (type != &UA_TYPES[UA_TYPES_DATETIME]) return UA_STATUSCODE_BADTYPEMISMATCH;
          *static_cast<UA_DateTime*>(dst) = to_ua_time(x.time);
          return UA_STATUSCODE_GOOD;
        } else {
          return UA_STATUSCODE_BADTYPEMISMATCH;
        }
      },
      v.data);
}

sdk::Result<Value> load_scalar(const UA_DataType* type, const void* p) {
  switch (type->typeKind) {
    case UA_DATATYPEKIND_BOOLEAN: return Value(*static_cast<const UA_Boolean*>(p) != 0);
    case UA_DATATYPEKIND_SBYTE: return Value(*static_cast<const UA_SByte*>(p));
    case UA_DATATYPEKIND_BYTE: return Value(*static_cast<const UA_Byte*>(p));
    case UA_DATATYPEKIND_INT16: return Value(*static_cast<const UA_Int16*>(p));
    case UA_DATATYPEKIND_UINT16: return Value(*static_cast<const UA_UInt16*>(p));
    case UA_DATATYPEKIND_INT32: return Value(*static_cast<const UA_Int32*>(p));
    case UA_DATATYPEKIND_UINT32: return Value(*static_cast<const UA_UInt32*>(p));
    case UA_DATATYPEKIND_INT64: return Value(static_cast<std::int64_t>(*static_cast<const UA_Int64*>(p)));
    case UA_DATATYPEKIND_UINT64: return Value(static_cast<std::uint64_t>(*static_cast<const UA_UInt64*>(p)));
    case UA_DATATYPEKIND_FLOAT: return Value(*static_cast<const UA_Float*>(p));
    case UA_DATATYPEKIND_DOUBLE: return Value(*static_cast<const UA_Double*>(p));
    case UA_DATATYPEKIND_STRING: {
      const auto* s = static_cast<const UA_String*>(p);
      return Value(std::string(reinterpret_cast<const char*>(s->data), s->length));
    }
    case UA_DATATYPEKIND_DATETIME: return Value(sdk::DateTime{from_ua_time(*static_cast<const UA_DateTime*>(p))});
    default: return sdk::make_error(status::BadTypeMismatch, "unsupported OPC UA data type");
  }
}

}  // namespace

UA_StatusCode to_variant(const Value& v, const sdk::TagType& shape, UA_Variant* out) {
  if (v.empty()) return UA_STATUSCODE_GOOD;  // empty variant
  if (const auto* arr = v.get_if<sdk::Array>()) {
    DataType elem = arr->empty() ? shape.scalar : sdk::type_of(*arr);
    const UA_DataType* type = ua_type(elem);
    if (type == &UA_TYPES[UA_TYPES_VARIANT]) return UA_STATUSCODE_BADTYPEMISMATCH;
    void* data = UA_Array_new(arr->size(), type);
    if (!data && !arr->empty()) return UA_STATUSCODE_BADOUTOFMEMORY;
    for (std::size_t i = 0; i < arr->size(); ++i) {
      auto* dst = static_cast<char*>(data) + i * type->memSize;
      if (UA_StatusCode rc = store_scalar((*arr)[i], type, dst); rc != UA_STATUSCODE_GOOD) {
        UA_Array_delete(data, arr->size(), type);
        return rc;
      }
    }
    UA_Variant_setArray(out, data, arr->size(), type);
    if (shape.rows > 0 && shape.cols > 0 && shape.rows * shape.cols == arr->size()) {
      out->arrayDimensions = static_cast<UA_UInt32*>(UA_Array_new(2, &UA_TYPES[UA_TYPES_UINT32]));
      out->arrayDimensionsSize = 2;
      out->arrayDimensions[0] = shape.rows;
      out->arrayDimensions[1] = shape.cols;
    }
    return UA_STATUSCODE_GOOD;
  }
  const UA_DataType* type = ua_type(sdk::type_of(v));
  void* data = UA_new(type);
  if (!data) return UA_STATUSCODE_BADOUTOFMEMORY;
  if (UA_StatusCode rc = store_scalar(v, type, data); rc != UA_STATUSCODE_GOOD) {
    UA_delete(data, type);
    return rc;
  }
  UA_Variant_setScalar(out, data, type);
  return UA_STATUSCODE_GOOD;
}

sdk::Result<Value> from_variant(const UA_Variant& v) {
  if (UA_Variant_isEmpty(&v)) return sdk::make_error(status::BadTypeMismatch, "empty value");
  if (UA_Variant_isScalar(&v)) return load_scalar(v.type, v.data);
  sdk::Array out;
  out.reserve(v.arrayLength);
  for (std::size_t i = 0; i < v.arrayLength; ++i) {
    auto e = load_scalar(v.type, static_cast<const char*>(v.data) + i * v.type->memSize);
    if (!e) return e;
    out.push_back(std::move(*e));
  }
  return Value(std::move(out));
}

}  // namespace logict::opcua
