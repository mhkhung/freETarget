"""The subset of esp-emu co-simulation protocol v1 used by the target board.

Wire contract: https://github.com/espressif/esp-emulator/blob/main/proto/esp_cosim.proto
Length-prefixed protobuf, with no generated bindings or third-party runtime.
The emulator's registration response checks protocol compatibility.
"""
import struct

MAX_FRAME = 256 * 1024


def varint(value):
    if value < 0:
        raise ValueError("negative protobuf integer")
    data = bytearray()
    while value > 127:
        data.append((value & 127) | 128)
        value >>= 7
    data.append(value)
    return bytes(data)


def number(key, value):
    return varint(key << 3) + varint(value)


def blob(key, data):
    return varint(key << 3 | 2) + varint(len(data)) + data


def fields(data):
    result, at = {}, 0

    def read():
        nonlocal at
        value, shift = 0, 0
        while at < len(data) and shift < 70:
            byte = data[at]
            at += 1
            value |= (byte & 127) << shift
            if byte < 128:
                return value
            shift += 7
        raise ValueError("truncated/oversized protobuf varint")

    while at < len(data):
        tag = read()
        key, wire = tag >> 3, tag & 7
        if not key:
            raise ValueError("protobuf field zero")
        if wire == 0:
            value = read()
        else:
            size = read() if wire == 2 else {1: 8, 5: 4}.get(wire)
            if size is None or at + size > len(data):
                raise ValueError("invalid protobuf field length/type")
            value = data[at:at + size]
            at += size
        result[key] = value
    return result


def message(kind, payload, at_ns=0):
    return number(1, at_ns) + blob(kind, payload)


def register(drive_pins, watch_pins, analog_pins=(), i2c_addresses=(), clock=True):
    claims = [blob(1, number(1, drive_pins) + number(2, watch_pins))]
    claims += [blob(5, number(1, pin)) for pin in analog_pins]
    claims += [blob(2, number(1, addr) + number(2, 13) + number(3, 14)) for addr in i2c_addresses]
    if clock:
        claims.append(blob(23, b""))
    return message(4, number(1, 1) + blob(2, b"freetarget-board") + b"".join(blob(3, c) for c in claims))


def gpio(pin, level, at_ns):
    return message(6, number(1, pin) + number(2, int(level)), at_ns)


def analog(pin, millivolts, at_ns=0):
    return message(13, number(1, pin) + number(2, millivolts), at_ns)


def grant(duration_ns):
    return message(40, number(1, duration_ns))


def pulses(pin, at_ns, widths_ns, first_level=False):
    if not 1 <= len(widths_ns) <= 512 or any(w <= 0 for w in widths_ns):
        raise ValueError("waveforms need 1..512 positive widths")
    packed = b"".join(varint(w) for w in widths_ns)
    return message(14, number(1, pin) + number(2, int(first_level)) + blob(3, packed), at_ns)


def reply(correlation_id, data=b"", ack=True):
    return message(8, number(1, correlation_id) + number(2, int(ack)) + blob(3, data) + number(4, 1))


def send(sock, data):
    if len(data) > MAX_FRAME:
        raise ValueError("co-simulation frame too large")
    sock.sendall(struct.pack("<I", len(data)) + data)


def exact(sock, size):
    result = bytearray()
    while len(result) < size:
        chunk = sock.recv(size - len(result))
        if not chunk:
            raise EOFError("emulator connection closed")
        result.extend(chunk)
    return bytes(result)


def receive(sock):
    size, = struct.unpack("<I", exact(sock, 4))
    if not 2 <= size <= MAX_FRAME:
        raise ValueError("invalid co-simulation frame size")
    frame = exact(sock, size)
    topic_length, = struct.unpack("<H", frame[:2])
    if topic_length > size - 2:
        raise ValueError("invalid topic length")
    return frame[2:2 + topic_length].decode(), fields(frame[2 + topic_length:])

