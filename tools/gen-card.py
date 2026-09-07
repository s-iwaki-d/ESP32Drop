#!/usr/bin/env python3
"""Regenerate the GreetingCard example's card_png.h from a PNG file.

    python3 tools/gen-card.py ESP32Drop.png > ESP32Drop/examples/GreetingCard/card_png.h

The image is embedded verbatim and sent verbatim: AirDrop carries the file, so what lands
on the receiving device is byte-identical to the array this writes.

Size is a matter of patience, not of protocol. An established connection outlives the
twenty-five seconds an iPhone's listener accepts NEW connections for: a measured transfer
took 28 seconds end to end and completed. What must land inside that window are the three
connects -- /Discover, /Ask and /Upload each open their own -- and the human's Accept sits
between the second and the third. So the thing that breaks a transfer is a slow person, not
a large file. The link runs at a measured 5.7-26 KB/s, so size only decides how long the
last connection is held open.
"""
import struct, sys

def main() -> int:
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    path = sys.argv[1]
    with open(path, 'rb') as f:
        d = f.read()
    if d[:8] != b'\x89PNG\r\n\x1a\n':
        sys.stderr.write(f"{path}: not a PNG. The sender refuses anything else: the file type\n"
                         f"is decided from the leading bytes, and only PNG is measured.\n")
        return 1
    w, h = struct.unpack('>II', d[16:24])
    if len(d) > 250000:
        sys.stderr.write(f"note: {len(d):,} bytes is roughly {len(d)/5700:.0f}-{len(d)/26000:.0f} s on this link.\n"
                         f"That completes -- an established connection is not bound by the wake\n"
                         f"window -- but the receiver watches a progress bar for that long.\n")

    out = sys.stdout.write
    out('/* card_png.h -- the image the GreetingCard example hands to whoever comes near.\n'
        ' *\n'
        f' * Generated from {path} by tools/gen-card.py; do not edit by hand.\n'
        f' * {w}x{h}, {len(d):,} bytes of PNG. Flat colour, so it compresses hard: a photo of the\n'
        ' * same dimensions would be an order of magnitude larger and would not cross the link\n'
        " * inside the twenty-five seconds an iPhone's AirDrop listener stays open.\n"
        ' *\n'
        ' * It is sent verbatim -- AirDrop carries the file, not a re-encoding of it, so what\n'
        ' * lands on the other device is byte-identical to this array.\n'
        ' */\n'
        '#pragma once\n'
        '#include <stdint.h>\n\n'
        f'#define CARD_PNG_LEN {len(d)}\n\n'
        'static const uint8_t CARD_PNG[CARD_PNG_LEN] = {\n')
    for i in range(0, len(d), 16):
        out('  ' + ' '.join(f'0x{b:02x},' for b in d[i:i+16]) + '\n')
    out('};\n')
    return 0

if __name__ == '__main__':
    raise SystemExit(main())
