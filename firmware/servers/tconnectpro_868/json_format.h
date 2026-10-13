#pragma once
#include <stddef.h>

// Formats the generated JSON without another whole-response allocation.
// Quoted strings and their escapes pass through unchanged.
template<class Emit> void wspdPrettyJson(const char *json, size_t size, Emit emit) {
  unsigned depth = 0; bool quoted = false, escaped = false;
  auto newline = [&]() { emit('\n'); for (unsigned i = 0; i < depth * 2; ++i) emit(' '); };
  for (size_t i = 0; i < size; ++i) {
    const char c = json[i];
    if (quoted) {
      emit(c); if (escaped) escaped = false; else if (c == '\\') escaped = true; else if (c == '"') quoted = false;
    } else if (c == '"') { quoted = true; emit(c); }
    else if (c == '{' || c == '[') {
      emit(c); ++depth;
      if (i + 1 < size && json[i + 1] != '}' && json[i + 1] != ']') newline();
    } else if (c == '}' || c == ']') {
      if (depth) --depth;
      if (i && json[i - 1] != '{' && json[i - 1] != '[') newline();
      emit(c);
    } else if (c == ',') { emit(c); newline(); }
    else if (c == ':') { emit(c); emit(' '); }
    else if (c != ' ' && c != '\n' && c != '\r' && c != '\t') emit(c);
  }
  emit('\n');
}
