"""Run the large-volume test on a disposable sparse image on Linux storage.
Using /tmp avoids the cost of hundreds of thousands of tiny DrvFS operations.
"""
import pathlib
import subprocess
import sys
import tempfile

binary = pathlib.Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="myos-large-") as directory:
    image = pathlib.Path(directory) / "test.img"
    with image.open("xb") as stream:
        stream.truncate(1073741824)
    subprocess.run([str(binary), str(image)], check=True)
