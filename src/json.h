#pragma once

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>

#include "types.h"

namespace sdfhook {

/// One JSON object per row, built straight into a string rather than as a document: a row is
/// written per mesh and per packet, so nothing here allocates a tree.
///
/// Naming each field beside its value is the point. The printf this replaces carried sixty
/// specifiers and seventy positional arguments, where one insertion silently shifts every field
/// after it into the wrong column.
class JsonObject {
public:
    JsonObject() { mText = "{"; }

    JsonObject& addUint(const char* key, u64 value) {
        key_(key);
        mText += std::to_string(value);
        return *this;
    }


    JsonObject& addBool(const char* key, bool value) {
        key_(key);
        mText += value ? "true" : "false";
        return *this;
    }

    /// Rounded rather than written at full precision: these read as measurements, and a reader
    /// diffing two traces should not see a difference that is only float noise.
    JsonObject& addReal(const char* key, double value, int decimals = 6) {
        key_(key);
        mText += real(value, decimals);
        return *this;
    }

    JsonObject& addString(const char* key, const char* value) {
        key_(key);
        quote(value ? value : "");
        return *this;
    }

    JsonObject& addString(const char* key, const std::string& value) {
        return addString(key, value.c_str());
    }


    JsonObject& addHexDigits(const char* key, u64 value, int digits) {
        return addString(key, format("%0*llX", digits, value).c_str());
    }

    /// A 128-bit register reads as one 32-character field, not two halves a reader has to rejoin.
    JsonObject& addHex128(const char* key, u64 hi, u64 lo) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%016llX%016llX", (unsigned long long)hi,
                      (unsigned long long)lo);
        return addString(key, buf);
    }


    JsonObject& addReals(const char* key, std::initializer_list<double> values, int decimals = 6) {
        beginArray(key);
        for (double v : values) pushReal(v, decimals);
        return endArray();
    }

    /// Arrays and nested objects are built in place rather than assembled as a string first, so a
    /// loop that writes elements cannot leave the row half-punctuated.
    JsonObject& beginArray(const char* key) {
        key_(key);
        mText += '[';
        mFirst = true;
        return *this;
    }

    JsonObject& endArray() {
        mText += ']';
        mFirst = false;
        return *this;
    }

    JsonObject& beginObject(const char* key) {
        key_(key);
        mText += '{';
        mFirst = true;
        return *this;
    }

    JsonObject& beginObject() {
        comma();
        mText += '{';
        mFirst = true;
        return *this;
    }

    JsonObject& endObject() {
        mText += '}';
        mFirst = false;
        return *this;
    }


    JsonObject& pushReal(double value, int decimals = 6) {
        comma();
        mText += real(value, decimals);
        return *this;
    }

    JsonObject& pushString(const char* value) {
        comma();
        quote(value ? value : "");
        return *this;
    }

    JsonObject& pushHexDigits(u64 value, int digits) {
        return pushString(format("%0*llX", digits, value).c_str());
    }

    /// Closes the object and hands over the row, newline included: every consumer reads JSONL.
    std::string take() {
        mText += "}\n";
        return std::move(mText);
    }

private:
    void comma() {
        if (!mFirst) mText += ',';
        mFirst = false;
    }

    void key_(const char* key) {
        comma();
        quote(key);
        mText += ':';
    }

    void quote(const char* text) {
        mText += '"';
        for (const char* p = text; *p; p++) {
            switch (*p) {
                case '"': mText += "\\\""; break;
                case '\\': mText += "\\\\"; break;
                case '\n': mText += "\\n"; break;
                case '\r': mText += "\\r"; break;
                case '\t': mText += "\\t"; break;
                default:
                    // Control characters have no literal spelling in JSON.
                    if (u8(*p) < 0x20) {
                        char esc[8];
                        std::snprintf(esc, sizeof(esc), "\\u%04X", u8(*p));
                        mText += esc;
                    } else {
                        mText += *p;
                    }
            }
        }
        mText += '"';
    }

    static std::string format(const char* spec, int digits, u64 value) {
        char buf[40];
        std::snprintf(buf, sizeof(buf), spec, digits, (unsigned long long)value);
        return buf;
    }

    static std::string real(double value, int decimals) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*g", decimals, value);
        return buf;
    }

    std::string mText;
    bool mFirst = true;
};

}  // namespace sdfhook
