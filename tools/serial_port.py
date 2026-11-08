"""Serial port helper that avoids resetting native-USB ESP32 boards."""

from __future__ import annotations

import serial

try:
    import termios
except ImportError:  # pragma: no cover - Windows
    termios = None


def open_port(path: str, baud: int = 115200) -> serial.Serial:
    # Configure DTR/RTS before open: pyserial otherwise asserts them while
    # opening the port, which can reset an ESP32-C3 USB-CDC board.
    ser = serial.Serial()
    ser.port = path
    ser.baudrate = baud
    ser.timeout = 0.1
    ser.write_timeout = 1.0
    ser.dsrdtr = False
    ser.rtscts = False
    ser.dtr = False
    ser.rts = False
    ser.open()

    # HUPCL drops DTR on close; clear it so closing the tool does not reset
    # the dongle or simulator.
    if termios is not None:
        try:
            attrs = termios.tcgetattr(ser.fd)
            attrs[2] &= ~termios.HUPCL
            termios.tcsetattr(ser.fd, termios.TCSANOW, attrs)
        except Exception:
            pass
    return ser
