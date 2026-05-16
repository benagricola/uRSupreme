#!/usr/bin/env python3
"""Raw KISS sniffer — opens the RNode at /dev/ttyACM0, flips it into TNC
mode by sending CMD_DETECT, configures the radio, and then dumps every
CMD_DATA / CMD_LOG / CMD_RADIO_STATE / CMD_ERROR frame it receives until
Ctrl-C. Sidesteps the python RNS stack so we can see what the firmware
is actually forwarding over the serial link.
"""
import sys
import time
import serial

FEND  = 0xC0
FESC  = 0xDB
TFEND = 0xDC
TFESC = 0xDD

CMD_DATA        = 0x00
CMD_FREQUENCY   = 0x01
CMD_BANDWIDTH   = 0x02
CMD_TXPOWER     = 0x03
CMD_SF          = 0x04
CMD_CR          = 0x05
CMD_RADIO_STATE = 0x06
CMD_DETECT      = 0x08
CMD_STAT_RX     = 0x21
CMD_STAT_TX     = 0x22
CMD_LOG         = 0x80
CMD_ERROR       = 0x90
DETECT_REQ      = 0x73

CMD_NAMES = {0x00: "DATA", 0x01: "FREQ", 0x02: "BW", 0x03: "TXP",
             0x04: "SF",  0x05: "CR",   0x06: "RADIO_STATE", 0x08: "DETECT",
             0x21: "STAT_RX", 0x22: "STAT_TX", 0x80: "LOG",   0x90: "ERROR"}


def kiss_frame(cmd, payload):
    out = bytearray([FEND, cmd])
    for b in payload:
        if b == FEND: out += bytes([FESC, TFEND])
        elif b == FESC: out += bytes([FESC, TFESC])
        else: out += bytes([b])
    out += bytes([FEND])
    return bytes(out)


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyACM0"
    s = serial.Serial(port, 115200, timeout=0.2)
    print(f"opened {port}")

    # Wake the RNode + flip to TNC mode.
    s.write(kiss_frame(CMD_DETECT, [DETECT_REQ]))
    time.sleep(0.5)

    # Configure radio: 868 MHz, BW 125 kHz, SF11, CR4/5, +14 dBm.
    def u32(v): return [(v >> 24) & 0xFF, (v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF]
    s.write(kiss_frame(CMD_FREQUENCY,  u32(868_000_000)))
    s.write(kiss_frame(CMD_BANDWIDTH,  u32(125_000)))
    s.write(kiss_frame(CMD_SF,         [11]))
    s.write(kiss_frame(CMD_CR,         [5]))
    s.write(kiss_frame(CMD_TXPOWER,    [14]))
    s.write(kiss_frame(CMD_RADIO_STATE, [0x01]))  # bring radio online
    time.sleep(0.5)
    print("config sent — listening for incoming frames (Ctrl-C to stop)")

    buf = bytearray()
    in_frame = False
    escape = False
    cmd = None
    payload = bytearray()
    while True:
        chunk = s.read(256)
        if not chunk:
            continue
        for b in chunk:
            if b == FEND:
                if in_frame and len(payload) > 0:
                    name = CMD_NAMES.get(cmd, f"0x{cmd:02x}")
                    hex_view = " ".join(f"{x:02x}" for x in payload[:48])
                    extra = " …" if len(payload) > 48 else ""
                    print(f"[{time.strftime('%H:%M:%S')}] {name:11s} len={len(payload):3d} | {hex_view}{extra}")
                in_frame = True; escape = False; cmd = None; payload = bytearray()
            elif in_frame and cmd is None:
                cmd = b
            elif in_frame:
                if escape:
                    if b == TFEND: payload.append(FEND)
                    elif b == TFESC: payload.append(FESC)
                    else: payload.append(b)
                    escape = False
                elif b == FESC:
                    escape = True
                else:
                    payload.append(b)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nbye")
