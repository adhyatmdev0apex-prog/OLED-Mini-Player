import argparse
import os
from moviepy import VideoFileClip

def main():
    # Set up the command line argument parser
    parser = argparse.ArgumentParser(description="Automatically cut a video in half.")
    parser.add_argument("input_video", help="The original video file to cut")
    parser.add_argument("-o", "--output", default="split_video", help="The base name for the output files")
    
    args = parser.parse_args()
    input_path = args.input_video
    base_name = args.output

    if not os.path.exists(input_path):
        print(f"Error: Could not find the file '{input_path}'")
        return

    print(f"Loading '{input_path}'...")
    video = VideoFileClip(input_path)
    
    midpoint = video.duration / 2
    print(f"Total duration: {video.duration} seconds. Splitting at {midpoint} seconds...")

    # Cut the video into two halves
    part1 = video.subclipped(0, midpoint)
    part2 = video.subclipped(midpoint, video.duration)

    # Automatically add the extensions
    out1 = f"{base_name}_part1.mp4"
    out2 = f"{base_name}_part2.mp4"

    # Export the two halves
    print(f"\nExporting {out1}...")
    part1.write_videofile(out1, codec="libx264")

    print(f"\nExporting {out2}...")
    part2.write_videofile(out2, codec="libx264")

    # Clean up memory
    video.close()
    part1.close()
    part2.close()

    print(f"\nDone! Saved as {out1} and {out2}")

if __name__ == "__main__":
    main()