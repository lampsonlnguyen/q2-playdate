#!/usr/bin/env python3
"""Embed a Lua source file as a C string-literal header.

Usage: python3 tools/embed_lua.py src/json.lua json_lua build/json_lua.h
"""
import sys


def main():
    src, symbol, out = sys.argv[1], sys.argv[2], sys.argv[3]
    with open(src, "r", encoding="utf-8") as handle:
        lines = handle.read().splitlines(keepends=True)
    guard = symbol.upper() + "_H"
    with open(out, "w", encoding="utf-8") as handle:
        handle.write(f"/* Generated from {src} by tools/embed_lua.py; do not edit. */\n")
        handle.write(f"#ifndef {guard}\n#define {guard}\n\n")
        handle.write(f"static const char {symbol}[] =\n")
        for line in lines:
            escaped = (line.replace("\\", "\\\\")
                       .replace('"', '\\"')
                       .replace("\n", "\\n")
                       .replace("\r", "\\r"))
            handle.write(f'    "{escaped}"\n')
        handle.write("    ;\n\n#endif\n")


main()
