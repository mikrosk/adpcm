#!/usr/bin/env python3
"""
Convert an audio file into an APM (IMA ADPCM) file for the Falcon.

The input is resampled to one of the Falcon's internal-clock frequencies
and encoded by ffmpeg's IMA ADPCM WAV encoder with a trellis search.
The WAV blocks are then repacked into the APM layout, which is decodable
block by block with Jack Jansen's adpcm_decoder() (state set from the
block header).

APM layout (all multi-byte values are big-endian):

  File header, 32 bytes:
    0   char[4]  magic "APCM"
    4   u16      version (1)
    6   u16      number of channels (1 or 2)
    8   u32      sample rate in Hz
    12  u16      block size in bytes (power of two, 32..8192)
    14  u16      reserved (0)
    16  u32      number of samples per channel
    20  u8[12]   reserved (0)

  Blocks, each exactly <block size> bytes (the last one zero-padded):
    for each channel: s16 valprev, u8 index (0..88), u8 reserved (0)
    then the ADPCM codes: each byte holds two consecutive samples of one
    channel, the first one in the high nibble; for stereo the bytes
    alternate left, right, left, right, ...

  The header valprev is the first sample of the block; the codes follow it,
  i.e. one block holds 1 + 2 * (block size - 4 * channels) / channels
  samples per channel.
"""

import argparse
import os
import struct
import subprocess
import sys
import tempfile

FALCON_RATES = (49170, 32780, 24585, 19668, 16390, 12292, 9834, 8195)

APM_MAGIC = b'APCM'
APM_VERSION = 1
APM_HEADER_SIZE = 32


def read_wav_ima(path):
    with open(path, 'rb') as f:
        data = f.read()

    if data[0:4] != b'RIFF' or data[8:12] != b'WAVE':
        sys.exit(f'{path}: not a WAV file')

    fmt = None
    samples = None
    payload = None
    pos = 12
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        chunk_size = struct.unpack('<I', data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + chunk_size]
        if chunk_id == b'fmt ':
            fmt = struct.unpack('<HHIIHH', body[:16])
        elif chunk_id == b'fact':
            samples = struct.unpack('<I', body[:4])[0]
        elif chunk_id == b'data':
            payload = body
        pos += 8 + chunk_size + (chunk_size & 1)

    if fmt is None or payload is None or samples is None:
        sys.exit(f'{path}: missing fmt, fact or data chunk')

    format_tag, channels, rate, _, block_align, bits = fmt
    if format_tag != 0x11 or bits != 4:
        sys.exit(f'{path}: not IMA ADPCM')

    return channels, rate, block_align, samples, payload


def repack_block(block, channels, block_size):
    out = bytearray()

    for ch in range(channels):
        valprev, index = struct.unpack('<hB', block[ch * 4:ch * 4 + 3])
        out += struct.pack('>hBB', valprev, index, 0)

    # WAV IMA: groups of 4 bytes (8 samples) per channel, first sample of
    # each byte in the low nibble
    body = block[channels * 4:]
    per_channel = [bytearray() for _ in range(channels)]
    for group in range(0, len(body), 4 * channels):
        for ch in range(channels):
            for b in body[group + ch * 4:group + ch * 4 + 4]:
                per_channel[ch].append(((b & 0x0f) << 4) | (b >> 4))

    for i in range(len(per_channel[0])):
        for ch in range(channels):
            out.append(per_channel[ch][i])

    out += bytes(block_size - len(out))
    return out


def main():
    parser = argparse.ArgumentParser(
        description='Encode an audio file into Falcon APM (IMA ADPCM).')
    parser.add_argument('input', help='input audio file (any format ffmpeg reads)')
    parser.add_argument('output', nargs='?', help='output .apm file (default: input with .apm suffix)')
    parser.add_argument('-r', '--rate', type=int, default=24585, choices=FALCON_RATES,
                        help='output sample rate (default: %(default)s)')
    parser.add_argument('-b', '--block-size', type=int, default=4096,
                        help='block size in bytes, power of two 32..8192 (default: %(default)s)')
    parser.add_argument('-t', '--trellis', type=int, default=8,
                        help='ffmpeg trellis size, 0..16, 0 = greedy (default: %(default)s)')
    args = parser.parse_args()

    bs = args.block_size
    if bs < 32 or bs > 8192 or bs & (bs - 1):
        parser.error('block size must be a power of two between 32 and 8192')
    if not 0 <= args.trellis <= 16:
        parser.error('trellis must be between 0 and 16')

    output = args.output or os.path.splitext(args.input)[0] + '.apm'

    with tempfile.TemporaryDirectory() as tmp:
        wav = os.path.join(tmp, 'ima.wav')
        subprocess.run([
            'ffmpeg', '-hide_banner', '-loglevel', 'error', '-nostdin', '-y',
            '-i', args.input,
            '-vn',
            '-af', f'aresample={args.rate}:resampler=soxr:precision=28',
            '-c:a', 'adpcm_ima_wav',
            '-trellis', str(args.trellis),
            '-block_size', str(bs),
            wav], check=True)

        channels, rate, block_align, samples, payload = read_wav_ima(wav)

    if channels not in (1, 2):
        sys.exit(f'unsupported number of channels: {channels}')

    with open(output, 'wb') as f:
        header = struct.pack('>4sHHIHHI', APM_MAGIC, APM_VERSION, channels,
                             rate, block_align, 0, samples)
        f.write(header + bytes(APM_HEADER_SIZE - len(header)))

        for pos in range(0, len(payload), block_align):
            f.write(repack_block(payload[pos:pos + block_align], channels, block_align))

    print(f'{output}: {channels} ch, {rate} Hz, {samples} samples/ch, '
          f'block {block_align} B, trellis {args.trellis}')


if __name__ == '__main__':
    main()
