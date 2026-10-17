#include "variant_pack.hpp"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_float64_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <cstring>

using namespace godot;

namespace weft_godot {
    namespace {
        constexpr int max_depth = 64;

        class Reader {
        public:
            Reader(const std::uint8_t* at, const std::uint8_t* end) : at_(at), end_(end) {
            }

            Variant value(int depth) {
                if (depth > max_depth || !need(1)) {
                    return fail();
                }
                const std::uint8_t tag = *at_++;
                if (tag <= 0x7f) {
                    return std::int64_t(tag);
                }
                if (tag >= 0xe0) {
                    return std::int64_t(std::int8_t(tag));
                }
                if ((tag & 0xf0) == 0x80) {
                    return map(tag & 0x0f, depth);
                }
                if ((tag & 0xf0) == 0x90) {
                    return array(tag & 0x0f, depth);
                }
                if ((tag & 0xe0) == 0xa0) {
                    return text(tag & 0x1f);
                }
                switch (tag) {
                    case 0xc0:
                        return Variant();
                    case 0xc2:
                        return false;
                    case 0xc3:
                        return true;
                    case 0xc4:
                        return bytes(read(1));
                    case 0xc5:
                        return bytes(read(2));
                    case 0xc6:
                        return bytes(read(4));
                    case 0xca: {
                        const auto bits = std::uint32_t(read(4));
                        float number;
                        std::memcpy(&number, &bits, sizeof(number));
                        return double(number);
                    }
                    case 0xcb: {
                        const auto bits = read(8);
                        double number;
                        std::memcpy(&number, &bits, sizeof(number));
                        return number;
                    }
                    case 0xcc:
                        return std::int64_t(read(1));
                    case 0xcd:
                        return std::int64_t(read(2));
                    case 0xce:
                        return std::int64_t(read(4));
                    case 0xcf: {
                        // Beyond int64 a number is a float, as JavaScript has it.
                        const auto whole = read(8);
                        return whole > std::uint64_t(INT64_MAX) ? Variant(double(whole)) : Variant(std::int64_t(whole));
                    }
                    case 0xd0:
                        return std::int64_t(std::int8_t(read(1)));
                    case 0xd1:
                        return std::int64_t(std::int16_t(read(2)));
                    case 0xd2:
                        return std::int64_t(std::int32_t(read(4)));
                    case 0xd3:
                        return std::int64_t(read(8));
                    case 0xd9:
                        return text(read(1));
                    case 0xda:
                        return text(read(2));
                    case 0xdb:
                        return text(read(4));
                    case 0xdc:
                        return array(read(2), depth);
                    case 0xdd:
                        return array(read(4), depth);
                    case 0xde:
                        return map(read(2), depth);
                    case 0xdf:
                        return map(read(4), depth);
                    default:
                        // Extension types: nothing weft sends.
                        return fail();
                }
            }

        private:
            bool need(std::uint64_t count) {
                if (bad_ || std::uint64_t(end_ - at_) < count) {
                    bad_ = true;
                }
                return !bad_;
            }

            Variant fail() {
                bad_ = true;
                return Variant();
            }

            std::uint64_t read(int count) {
                std::uint64_t value = 0;
                if (need(std::uint64_t(count))) {
                    for (int i = 0; i < count; ++i) {
                        value = (value << 8) | *at_++;
                    }
                }
                return value;
            }

            Variant text(std::uint64_t size) {
                if (!need(size)) {
                    return Variant();
                }
                const String result = String::utf8(reinterpret_cast<const char*>(at_), std::int64_t(size));
                at_ += size;
                return result;
            }

            Variant bytes(std::uint64_t size) {
                if (!need(size)) {
                    return Variant();
                }
                PackedByteArray result;
                result.resize(std::int64_t(size));
                if (size) {
                    std::memcpy(result.ptrw(), at_, size);
                }
                at_ += size;
                return result;
            }

            Variant array(std::uint64_t count, int depth) {
                Array result;
                for (std::uint64_t i = 0; i < count && !bad_; ++i) {
                    result.push_back(value(depth + 1));
                }
                return result;
            }

            Variant map(std::uint64_t count, int depth) {
                Dictionary result;
                for (std::uint64_t i = 0; i < count && !bad_; ++i) {
                    const Variant key = value(depth + 1);
                    result[key] = value(depth + 1);
                }
                return result;
            }

            const std::uint8_t* at_;
            const std::uint8_t* end_;
            bool bad_ = false;
        };

        class Writer {
        public:
            std::string out;

            void value(const Variant& v, int depth) {
                if (depth > max_depth) {
                    nil();
                    return;
                }
                switch (v.get_type()) {
                    case Variant::NIL:
                        nil();
                        break;
                    case Variant::BOOL:
                        byte(bool(v) ? 0xc3 : 0xc2);
                        break;
                    case Variant::INT:
                        integer(std::int64_t(v));
                        break;
                    case Variant::FLOAT:
                        real(double(v));
                        break;
                    case Variant::STRING:
                    case Variant::STRING_NAME:
                    case Variant::NODE_PATH:
                        text(String(v));
                        break;
                    case Variant::VECTOR2: {
                        const Vector2 p = v;
                        fields({{"x", p.x}, {"y", p.y}});
                        break;
                    }
                    case Variant::VECTOR2I: {
                        const Vector2i p = v;
                        fields({{"x", double(p.x)}, {"y", double(p.y)}});
                        break;
                    }
                    case Variant::VECTOR3: {
                        const Vector3 p = v;
                        fields({{"x", p.x}, {"y", p.y}, {"z", p.z}});
                        break;
                    }
                    case Variant::VECTOR3I: {
                        const Vector3i p = v;
                        fields({{"x", double(p.x)}, {"y", double(p.y)}, {"z", double(p.z)}});
                        break;
                    }
                    case Variant::RECT2: {
                        const Rect2 r = v;
                        fields({{"x", r.position.x}, {"y", r.position.y}, {"width", r.size.x}, {"height", r.size.y}});
                        break;
                    }
                    case Variant::RECT2I: {
                        const Rect2i r = v;
                        fields({{"x", double(r.position.x)}, {"y", double(r.position.y)}, {"width", double(r.size.x)},
                                {"height", double(r.size.y)}});
                        break;
                    }
                    case Variant::COLOR: {
                        const Color c = v;
                        fields({{"r", c.r}, {"g", c.g}, {"b", c.b}, {"a", c.a}});
                        break;
                    }
                    case Variant::DICTIONARY: {
                        const Dictionary d = v;
                        const Array keys = d.keys();
                        header(keys.size(), 0x80, 0xde, 0xdf);
                        for (std::int64_t i = 0; i < keys.size(); ++i) {
                            // A page's object keys are text.
                            text(String(keys[i]));
                            value(d[keys[i]], depth + 1);
                        }
                        break;
                    }
                    case Variant::ARRAY:
                        list(Array(v), depth);
                        break;
                    case Variant::PACKED_BYTE_ARRAY: {
                        const PackedByteArray b = v;
                        binary(b);
                        break;
                    }
                    case Variant::PACKED_INT32_ARRAY:
                        list(Array(PackedInt32Array(v)), depth);
                        break;
                    case Variant::PACKED_INT64_ARRAY:
                        list(Array(PackedInt64Array(v)), depth);
                        break;
                    case Variant::PACKED_FLOAT32_ARRAY:
                        list(Array(PackedFloat32Array(v)), depth);
                        break;
                    case Variant::PACKED_FLOAT64_ARRAY:
                        list(Array(PackedFloat64Array(v)), depth);
                        break;
                    case Variant::PACKED_STRING_ARRAY:
                        list(Array(PackedStringArray(v)), depth);
                        break;
                    case Variant::PACKED_VECTOR2_ARRAY:
                        list(Array(PackedVector2Array(v)), depth);
                        break;
                    case Variant::PACKED_VECTOR3_ARRAY:
                        list(Array(PackedVector3Array(v)), depth);
                        break;
                    case Variant::PACKED_COLOR_ARRAY:
                        list(Array(PackedColorArray(v)), depth);
                        break;
                    default:
                        text(v.stringify());
                        break;
                }
            }

        private:
            struct Field {
                const char* name;
                double number;
            };

            void byte(std::uint8_t b) {
                out.push_back(char(b));
            }

            void big(std::uint64_t v, int count) {
                for (int i = count - 1; i >= 0; --i) {
                    byte(std::uint8_t(v >> (8 * i)));
                }
            }

            void nil() {
                byte(0xc0);
            }

            void integer(std::int64_t v) {
                if (v >= 0 && v <= 0x7f) {
                    byte(std::uint8_t(v));
                } else if (v < 0 && v >= -32) {
                    byte(std::uint8_t(std::int8_t(v)));
                } else if (v >= INT32_MIN && v <= INT32_MAX) {
                    byte(0xd2);
                    big(std::uint32_t(std::int32_t(v)), 4);
                } else {
                    byte(0xd3);
                    big(std::uint64_t(v), 8);
                }
            }

            void real(double v) {
                std::uint64_t bits;
                std::memcpy(&bits, &v, sizeof(bits));
                byte(0xcb);
                big(bits, 8);
            }

            void header(std::int64_t count, std::uint8_t fix, std::uint8_t two, std::uint8_t four) {
                if (count < 16) {
                    byte(std::uint8_t(fix | count));
                } else if (count <= 0xffff) {
                    byte(two);
                    big(std::uint64_t(count), 2);
                } else {
                    byte(four);
                    big(std::uint64_t(count), 4);
                }
            }

            void text(const String& s) {
                const CharString utf8 = s.utf8();
                const auto size = std::uint64_t(utf8.length());
                if (size < 32) {
                    byte(std::uint8_t(0xa0 | size));
                } else if (size <= 0xff) {
                    byte(0xd9);
                    big(size, 1);
                } else if (size <= 0xffff) {
                    byte(0xda);
                    big(size, 2);
                } else {
                    byte(0xdb);
                    big(size, 4);
                }
                out.append(utf8.get_data(), size);
            }

            void binary(const PackedByteArray& b) {
                const auto size = std::uint64_t(b.size());
                if (size <= 0xff) {
                    byte(0xc4);
                    big(size, 1);
                } else if (size <= 0xffff) {
                    byte(0xc5);
                    big(size, 2);
                } else {
                    byte(0xc6);
                    big(size, 4);
                }
                out.append(reinterpret_cast<const char*>(b.ptr()), size);
            }

            void fields(std::initializer_list<Field> list) {
                byte(std::uint8_t(0x80 | list.size()));
                for (const auto& field : list) {
                    text(String(field.name));
                    real(field.number);
                }
            }

            void list(const Array& a, int depth) {
                header(a.size(), 0x90, 0xdc, 0xdd);
                for (std::int64_t i = 0; i < a.size(); ++i) {
                    value(a[i], depth + 1);
                }
            }
        };
    } // namespace

    Variant unpack(const void* data, std::size_t size) {
        if (!data || !size) {
            return Variant();
        }
        const auto* at = static_cast<const std::uint8_t*>(data);
        Reader reader(at, at + size);
        return reader.value(0);
    }

    std::string pack(const Variant& value) {
        Writer writer;
        writer.value(value, 0);
        return std::move(writer.out);
    }
} // namespace weft_godot
