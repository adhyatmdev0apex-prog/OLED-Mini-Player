import os
import subprocess
import shutil
import json
import re

def modify_scripts(video_name, out_bin, out_pcm):
    # Edit convert.py
    with open("convert.py", "r") as f:
        content = f.read()
    content = re.sub(r'INPUT_VIDEO = ".*?"', f'INPUT_VIDEO = "{video_name}"', content)
    content = re.sub(r'OUTPUT_FILE = ".*?"', f'OUTPUT_FILE = "{out_bin}"', content)
    with open("convert.py", "w") as f:
        f.write(content)

    # Edit Audio_convert.py
    with open("Audio_convert.py", "r") as f:
        content2 = f.read()
    content2 = re.sub(r'INPUT_VIDEO = ".*?"', f'INPUT_VIDEO = "{video_name}"', content2)
    content2 = re.sub(r'OUTPUT_FILE = ".*?"', f'OUTPUT_FILE = "{out_pcm}"', content2)
    with open("Audio_convert.py", "w") as f:
        f.write(content2)

def convert_and_move(video_name, folder_name):
    print(f"\n--- Converting {video_name} ---")
    out_bin = f"{folder_name}_video.bin"
    out_pcm = f"{folder_name}_audio.bin"
    modify_scripts(video_name, out_bin, out_pcm)

    subprocess.run(["python", "convert.py"], check=True)
    subprocess.run(["python", "Audio_convert.py"], check=True)

    dest_dir = os.path.join("WEB", "media", folder_name)
    os.makedirs(dest_dir, exist_ok=True)
    
    shutil.move(out_bin, os.path.join(dest_dir, "video.bin"))
    shutil.move(out_pcm, os.path.join(dest_dir, "audio.bin"))
    print(f"Moved files to {dest_dir}")

convert_and_move("Animation vs. Geometry Dash - Alan Becker (360p, h264).mp4", "v0001")
convert_and_move("Animation vs. Coding - Alan Becker (360p, h264).mp4", "v0005")

# Update catalogue
cat_path = os.path.join("WEB", "data", "catalogue.json")
with open(cat_path, "r") as f:
    cat = json.load(f)

for v in cat.get("videos", []):
    if v["id"] in ["v0001", "v0005"]:
        if "metadata" not in v:
            v["metadata"] = {}
        v["metadata"]["fps"] = 18

with open(cat_path, "w") as f:
    json.dump(cat, f, indent=2)

print("Catalogue updated!")
