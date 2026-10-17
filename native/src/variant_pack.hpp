#pragma once

#include <godot_cpp/variant/variant.hpp>

#include <cstddef>
#include <string>

namespace weft_godot {
    // Values between Godot and a page, as MessagePack (weft's WEFT_VALUE_MSGPACK): a page's value becomes a Variant (null,
    // bool, int, float, String, PackedByteArray for binary, Array, Dictionary), and a Variant becomes a value of the page
    // (vectors, colors and rectangles as objects of their fields, other types as their text). Integers stay integers both
    // ways, which JSON would not keep.
    godot::Variant unpack(const void* data, std::size_t size);
    std::string pack(const godot::Variant& value);
} // namespace weft_godot
