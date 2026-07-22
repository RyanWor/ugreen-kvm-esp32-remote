# Logic-analyzer captures

Bus captures from a UGREEN 4-port HDMI KVM's wired remote, used to reverse-
engineer the control protocol (see [../PROTOCOL.md](../PROTOCOL.md)).

Captured with a 24 MHz 8-channel logic analyzer in Saleae Logic 2, SPI decode:
`CPOL=1, CPHA=1, LSB-first, 8-bit, Enable/STB active-low` — Ch0=DIO, Ch1=CLK,
Ch2=STB.

## Rendered waveforms
The `.svg` diagrams are rendered directly from the raw captured samples (not
mocked), each showing one `0x42` key-read transaction at identical scale:

- `comparison.svg` — all five DIO responses stacked; the one changing byte per
  button is highlighted.
- `idle.svg`, `input1.svg` … `input4.svg` — individual transactions with the
  decoded bytes labeled.

| Capture  | 0x42 response         |
|----------|-----------------------|
| idle     | `42 00 00 00 00 00`   |
| input 1  | `42 00 00 01 00 00`   |
| input 2  | `42 00 00 08 00 00`   |
| input 3  | `42 00 00 00 01 00`   |
| input 4  | `42 00 00 00 08 00`   |

## Raw captures
The `.sal` files open in Saleae Logic 2; the `.csv` files are the exported
raw samples (`Time, Ch0/DIO, Ch1/CLK, Ch2/STB`) if you want to decode them
yourself. Add your own `.sal`/`.csv` here when you commit.
