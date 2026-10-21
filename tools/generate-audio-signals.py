"""Regenerate the project's CC0 audio fixtures (standard library only).

Run from any directory. Kenney originals and their mono derivative are separate
assets; see their READMEs for provenance and the conversion used.
"""
import hashlib
import math
from pathlib import Path
import random
import struct
import wave


def main():
    folder = Path(__file__).resolve().parents[1] / "samples/audio/audio/generated"
    folder.mkdir(parents=True, exist_ok=True)
    rng = random.Random(271828)
    for name, channels, seconds in (("noise.wav", 1, 2), ("stereo.wav", 2, 12)):
        data = bytearray()
        for i in range(seconds * 48000):
            samples = ([int(rng.uniform(-1, 1) * 2000)] if channels == 1 else
                       [int(2000 * math.sin(2 * math.pi * f * i / 48000)) for f in (440, 660)])
            data.extend(struct.pack("<" + "h" * channels, *samples))
        with wave.open(str(folder / name), "wb") as output:
            output.setnchannels(channels)
            output.setsampwidth(2)
            output.setframerate(48000)
            output.writeframes(data)
    hashes = "".join(f"- {name}: SHA-256 `{hashlib.sha256((folder / name).read_bytes()).hexdigest()}`\n"
                     for name in ("noise.wav", "stereo.wav"))
    (folder / "README.md").write_text(
        "# Generated test signals\n\n"
        "Created for Ogre Throne, dedicated to CC0-1.0. No third-party recordings.\n"
        "48 kHz PCM16. noise.wav: 2 s mono white noise, Python Random seed 271828, peak 2000/32768.\n"
        "stereo.wav: 12 s stereo, 440 Hz left / 660 Hz right, peak 2000/32768.\n\n" + hashes,
        encoding="utf-8")


if __name__ == "__main__":
    main()
