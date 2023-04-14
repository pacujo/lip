#!/usr/bin/env python

import sys

def main():
    try:
        variable, lenconst, cpath, hpath = sys.argv[1:]
    except ValueError:
        sys.stderr.write(
            f"Usage: {sys.argv[0]} variable lenconst cpath hpath\n")
        sys.exit(1)
    enctab = { b: f"\\x{b:02x}" for b in range(256) }
    enctab.update({ b: chr(b) for b in range(ord(" "), ord("~") + 1) })
    enctab.update({
        ord('"'): '\\"',
        ord("\\"): "\\\\",
        ord("\f"): "\\f",
        ord("\n"): "\\n",
        ord("\r"): "\\r",
        ord("\t"): "\\t",
        ord("\v"): "\\v",
        ord("?"): "\\?",        # trigraph avoidance
    })
    data = sys.stdin.buffer.read()
    with sys.stdout if cpath == "-" else open(hpath, "w") as hout:
        hout.write(f"""#pragma once
enum {{ {lenconst} = {len(data)} }};
extern const char {variable}[];
""")
    with sys.stdout if cpath == "-" else open(cpath, "w") as cout:
        cout.write(f'const char {variable}[] =\n"')
        col = 1
        for b in data:
            enc = enctab[b]
            col += len(enc)
            if col >= 79:
                cout.write('"\n"')
                col = 1 + len(enc)
            cout.write(enc)
        cout.write(f'"\n; /* {variable} */\n')

if __name__ == "__main__":
    main()
