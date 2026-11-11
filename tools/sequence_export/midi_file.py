# SPDX-License-Identifier: MIT

"""Reads the Standard MIDI Files that the converters write, for the converters' checks and research tools.

tracks_of(midi) returns the header chunk and each track as its name and its events. An event is (tick, status, data),
with the tick counted from the track's start:

- a channel event has its status byte, with running status resolved, and its one or two data bytes;
- a meta event has the status 0xFF, and its data is its type, its length as a variable-length number, then its bytes;
- a system exclusive event has the status 0xF0 or 0xF7, and its data is the bytes after its length.

event_bytes(status, data) returns an event's bytes as a file stores them, with its status byte. tick_seconds() converts
ticks to seconds through a file's tempo changes.
"""

import bisect

DEFAULT_TEMPO = 500000  # microseconds per quarter note, MIDI's default


def read_number(data, pos):
    """The variable-length number at `data[pos]`, and the position after it."""
    value = 0
    while True:
        byte = data[pos]
        pos += 1
        value = value << 7 | byte & 0x7F
        if not byte & 0x80:
            return value, pos


def number_bytes(value):
    """`value` as a variable-length number."""
    out = bytearray([value & 0x7F])
    while value > 0x7F:
        value >>= 7
        out.insert(0, value & 0x7F | 0x80)
    return bytes(out)


def division_of(midi):
    """The ticks a quarter note of the file `midi`, or of its header chunk."""
    return int.from_bytes(midi[12:14], "big")


def events_of(body):
    """The events of the track chunk whose bytes after the chunk's size are `body`. Each event is (tick, status,
    data)."""
    events = []
    tick = 0
    pos = 0
    status = 0
    while pos < len(body):
        delta, pos = read_number(body, pos)
        tick += delta
        if body[pos] == 0xFF:
            size, start = read_number(body, pos + 2)
            events.append((tick, 0xFF, bytes(body[pos + 1:start + size])))
            pos = start + size
        elif body[pos] in (0xF0, 0xF7):
            size, start = read_number(body, pos + 1)
            events.append((tick, body[pos], bytes(body[start:start + size])))
            pos = start + size
        else:
            if body[pos] & 0x80:
                status = body[pos]
                pos += 1
            size = 1 if status & 0xF0 in (0xC0, 0xD0) else 2
            events.append((tick, status, bytes(body[pos:pos + size])))
            pos += size
    return events


def meta_text(data):
    """The text of a meta event whose data is `data`, such as a track's name."""
    size, start = read_number(data, 1)
    return data[start:start + size].decode("utf-8", "replace")


def tracks_of(midi):
    """The header chunk of the file `midi`, and each track chunk as its name and its events (events_of). A track's
    name is its first name event, or "" without one. Chunks of other kinds are left out."""
    header_size = int.from_bytes(midi[4:8], "big")
    pos = 8 + header_size
    tracks = []
    while pos + 8 <= len(midi):
        size = int.from_bytes(midi[pos + 4:pos + 8], "big")
        body = midi[pos + 8:pos + 8 + size]
        is_track = midi[pos:pos + 4] == b"MTrk"
        pos += 8 + size
        if not is_track:
            continue

        events = events_of(body)
        name = next((meta_text(data) for _, status, data in events if status == 0xFF and data[:1] == b"\x03"), "")
        tracks.append((name, events))
    return midi[:8 + header_size], tracks


def event_bytes(status, data):
    """The bytes of the event (status, data) of events_of as a file stores them, with its status byte."""
    if status in (0xF0, 0xF7):
        return bytes([status]) + number_bytes(len(data)) + data
    return bytes([status]) + data


def tempo_changes(tracks):
    """Each tempo change of `tracks` (tracks_of): (tick, microseconds per quarter note), in the order of the ticks.
    Changes at the same tick stay in the order of the tracks and the events."""
    changes = [(tick, int.from_bytes(data[2:5], "big")) for _, events in tracks for tick, status, data in events
               if status == 0xFF and data[:1] == b"\x51"]
    return sorted(changes, key=lambda change: change[0])


def tick_seconds(division, changes):
    """A function that converts a tick to seconds, for a file of `division` ticks a quarter note and the tempo changes
    `changes` (tempo_changes). The last change at a tick applies from that tick on."""
    ticks, starts, tempos = [0], [0.0], [DEFAULT_TEMPO]
    for tick, tempo in changes:
        starts.append(starts[-1] + (tick - ticks[-1]) * tempos[-1] / division / 1e6)
        ticks.append(tick)
        tempos.append(tempo)

    def seconds(tick):
        i = bisect.bisect_right(ticks, tick) - 1
        return starts[i] + (tick - ticks[i]) * tempos[i] / division / 1e6
    return seconds


def length_seconds(midi):
    """The length of the file `midi` in seconds: the tick of its last event, through its tempo changes."""
    header, tracks = tracks_of(midi)
    end = max((events[-1][0] for _, events in tracks if events), default=0)
    return tick_seconds(division_of(header), tempo_changes(tracks))(end)
