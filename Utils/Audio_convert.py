import os
import subprocess

# ============================================================
# CONFIGURATION
# ============================================================

import argparse
# 5000 Hz sample rate:
# - Fits within LittleFS partition (~700 KB for 143 seconds of audio)
# - Provides 2500 Hz Nyquist frequency, capturing speech, lead synths, and percussions
SAMPLE_RATE = 5000

# Advanced DSP Audio Processing Filter Chain:
# 1. highpass=f=90:
#    Cuts sub-bass rumble (<90Hz) that small Bluetooth speakers cannot produce
#    and that would otherwise consume 80% of the 8-bit dynamic range.
# 2. equalizer=f=1800:width_type=h:width=1000:g=6:
#    Boosts mid/high presence (1300Hz - 2300Hz) by +6dB so vocals, lead synths,
#    and sound effects remain clear and punchy.
# 3. lowpass=f=2350:
#    Sharp anti-aliasing cutoff right before the 2500Hz Nyquist limit.
# 4. dynaudnorm=f=75:g=21:p=0.95:m=10.0:
#    Dynamic Audio Normalizer. Normalizes quiet and loud passages across
#    the track so the audio consistently utilizes the FULL 8-bit resolution (0..255)
#    instead of sitting in the bottom 3-4 bits (which caused screeching bitcrush noise).
# 5. alimiter=limit=0.98:
#    Brickwall peak limiter to ensure zero clipping distortion.
AUDIO_FILTERS = (
    "highpass=f=90,"
    "equalizer=f=1800:width_type=h:width=1000:g=6,"
    "lowpass=f=2350,"
    "dynaudnorm=f=75:g=21:p=0.95:m=10.0,"
    "alimiter=limit=0.98"
)

def convert_audio():
    parser = argparse.ArgumentParser(description="AUDIO CONVERTER")
    parser.add_argument("input_video", help="Input video file")
    parser.add_argument("-o", "--output", help="Output file", required=True)
    args = parser.parse_args()

    INPUT_VIDEO = args.input_video
    OUTPUT_FILE = args.output
    if not OUTPUT_FILE.endswith('.pcm'):
        OUTPUT_FILE += '.pcm'
    print(f"Extracting & enhancing audio to {OUTPUT_FILE} at {SAMPLE_RATE} Hz...")
    print("Applying DSP filters: High-pass (90Hz), Presence Boost (+6dB @ 1.8kHz), Low-pass (2.35kHz), Dynamic Normalizer & Limiter...")

    command = [
        "ffmpeg",
        "-y",
        "-i", INPUT_VIDEO,
        "-vn",                   # Strip video track
        "-ac", "1",              # Convert to mono
        "-ar", str(SAMPLE_RATE), # 5000 Hz sample rate
        "-af", AUDIO_FILTERS,    # DSP enhancement filters
        "-f", "u8",              # Raw unsigned 8-bit PCM
        "-acodec", "pcm_u8",
        OUTPUT_FILE
    ]

    result = subprocess.run(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )

    if result.returncode != 0:
        print("FFmpeg error:")
        print(result.stderr)
        return

    size = os.path.getsize(OUTPUT_FILE)
    print(f"\n[SUCCESS] Created {OUTPUT_FILE}")
    print(f"File size: {size / 1024:.1f} KB ({size} bytes)")
    print(f"Ready to copy to data/ folder and flash to ESP32!")

if __name__ == "__main__":
    convert_audio()