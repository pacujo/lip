#!/usr/bin/env python

import sys

def main():
    try:
        variable, cpath, hpath = sys.argv[1:]
    except ValueError:
        sys.stderr.write(f"Usage: {sys.argv[0]} variable cpath hpath\n")
        sys.exit(1)
    special = {
        ord('"'): '\\"',
        ord("\\"): "\\\\",
        ord("\f"): "\\f",
        ord("\n"): "\\n",
        ord("\r"): "\\r",
        ord("\t"): "\\t",
        ord("\v"): "\\v",
        ord("?"): "\\?",        # trigraph avoidance
    }
    printable = set(range(ord(" "), ord("~") + 1)) - special.keys()
    cout = sys.stdout if cpath == "-" else open(cpath, "w")
    hout = sys.stdout if cpath == "-" else open(hpath, "w")
    hout.write(f"extern const char {variable}[];\n")
    cout.write(f'const char {variable}[] = ""')
    while True:
        data = sys.stdin.buffer.read(18)
        if not data:
            break
        cout.write('\n    "')
        for b in data:
            if b in printable:
                cout.write(f"{b:c}")
            else:
                cout.write(special.get(b, f"\\x{b:02x}"))
        cout.write('"')
    cout.write(";\n")

if __name__ == "__main__":
    main()
