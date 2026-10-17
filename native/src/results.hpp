#pragma once

#include "weft/weft.h"

#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/string.hpp>

namespace weft_godot {
    // A weft result as Godot's Error.
    inline godot::Error godot_error(weft_result result) {
        switch (result) {
            case WEFT_OK:
                return godot::OK;
            case WEFT_INVALID_ARGUMENT:
                return godot::ERR_INVALID_PARAMETER;
            case WEFT_UNSUPPORTED_ABI:
                return godot::ERR_UNAVAILABLE;
            case WEFT_OUT_OF_MEMORY:
            case WEFT_CAPACITY_EXCEEDED:
                return godot::ERR_OUT_OF_MEMORY;
            case WEFT_BUSY:
                return godot::ERR_BUSY;
            case WEFT_STALE_HANDLE:
                return godot::ERR_DOES_NOT_EXIST;
            case WEFT_SCRIPT_ERROR:
                return godot::ERR_SCRIPT_FAILED;
            default:
                return godot::FAILED;
        }
    }

    // "weft: <what> failed: <weft's text of the result>".
    inline godot::String failure(const char* what, weft_result result) {
        return godot::String("weft: ") + what + " failed: " + weft_result_string(result);
    }
} // namespace weft_godot
