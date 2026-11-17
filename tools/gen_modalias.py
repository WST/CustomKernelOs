#!/usr/bin/env python3
"""Собрать /boot/modules/aliases из MYOS_MODULE и MYOS_PCI_ID в исходнике модуля."""
import re, sys
text = open(sys.argv[1], encoding="utf-8").read()
name = re.search(r"MYOS_MODULE\(([A-Za-z0-9_]+)\)", text)
if not name:
    sys.exit("no MYOS_MODULE")
mod = name.group(1) + ".ko"
for ven, dev in re.findall(r"MYOS_PCI_ID\(\s*0x([0-9A-Fa-f]+)\s*,\s*0x([0-9A-Fa-f]+)\s*\)", text):
    print(f"{int(ven,16):04x} {int(dev,16):04x} {mod}")
