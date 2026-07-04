#!/usr/bin/env python3

from pathlib import Path
import sys


def main() -> None:
    if len(sys.argv) != 4:
        print("Usage: bin2c.py input.bin output.h symbol_name")
        raise SystemExit(1)

    input_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])
    symbol_name = sys.argv[3]

    data = input_path.read_bytes()

    with output_path.open("w", encoding="utf-8") as file:
        file.write("#pragma once\n\n")
        file.write("#include <stddef.h>\n")
        file.write("#include <stdint.h>\n\n")

        file.write(
            f"static const uint8_t {symbol_name}[] "
            "__attribute__((aligned(4))) = {\n"
        )

        for offset in range(0, len(data), 12):
            chunk = data[offset:offset + 12]
            values = ", ".join(f"0x{value:02X}" for value in chunk)
            file.write(f"    {values},\n")

        file.write("};\n\n")
        file.write(
            f"static const size_t {symbol_name}_size = "
            f"sizeof({symbol_name});\n"
        )


if __name__ == "__main__":
    main()
