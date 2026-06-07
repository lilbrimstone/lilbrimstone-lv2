import sys
import os
import glob

# LV2 Configuration
URI = "https://github.com/lilbrimstone/dexed"
NAME = "LilBrimstone Dexed"
BINARY = "dexed.so"

def clean_name(raw_bytes):
    valid = ""
    for b in raw_bytes:
        if 32 <= b <= 126:
            valid += chr(b)
    return valid.strip()

def unpack_dx7_patch(packed):
    if len(packed) != 128: return None
    unpacked = bytearray(156)
    for op in range(6):
        src_start = op * 17
        src = packed[src_start : src_start + 17]
        dst_start = op * 21
        unpacked[dst_start + 0] = src[0]
        unpacked[dst_start + 1] = src[1]
        unpacked[dst_start + 2] = src[2]
        unpacked[dst_start + 3] = src[3]
        unpacked[dst_start + 4] = src[4]
        unpacked[dst_start + 5] = src[5]
        unpacked[dst_start + 6] = src[6]
        unpacked[dst_start + 7] = src[7]
        unpacked[dst_start + 8] = src[8]
        unpacked[dst_start + 9] = src[9]
        unpacked[dst_start + 10] = src[10]
        unpacked[dst_start + 11] = (src[11] & 0x0C) >> 2
        unpacked[dst_start + 12] = (src[11] & 0x03)
        unpacked[dst_start + 13] = src[12] & 0x07
        unpacked[dst_start + 14] = src[13] & 0x03
        unpacked[dst_start + 15] = (src[13] >> 2) & 0x07
        unpacked[dst_start + 16] = src[14]
        unpacked[dst_start + 17] = src[15] & 0x01
        unpacked[dst_start + 18] = (src[15] >> 1) & 0x1F
        unpacked[dst_start + 19] = src[16]
        unpacked[dst_start + 20] = (src[12] >> 3) & 0x0F
    g_src = packed[102:]
    g_dst = 126
    unpacked[g_dst : g_dst + 9] = g_src[0 : 9]
    unpacked[g_dst + 9] = g_src[9] & 0x07
    unpacked[g_dst + 10] = (g_src[9] >> 3) & 0x01
    unpacked[g_dst + 11] = g_src[10]
    unpacked[g_dst + 12] = g_src[11]
    unpacked[g_dst + 13] = g_src[12]
    unpacked[g_dst + 14] = g_src[13]
    unpacked[g_dst + 15] = g_src[14] & 0x01
    unpacked[g_dst + 16] = (g_src[14] >> 1) & 0x07
    unpacked[g_dst + 17] = (g_src[14] >> 4) & 0x07
    unpacked[g_dst + 18] = g_src[15]
    for i in range(10): unpacked[g_dst + 19 + i] = g_src[16 + i]
    return unpacked

def generate_files(files):
    all_patches = []
    for filename in files:
        print(f"Reading {filename}...")
        try:
            with open(filename, 'rb') as f:
                data = f.read()
            start_offset = 0
            header_idx = data.find(b'\xF0\x43\x00\x09\x20\x00')
            if header_idx != -1: start_offset = header_idx + 6
            elif len(data) == 4104: start_offset = 6
            elif len(data) == 4096: start_offset = 0
            for i in range(32):
                offset = start_offset + (i * 128)
                if offset + 128 > len(data): break
                packed = data[offset : offset + 128]
                unpacked = unpack_dx7_patch(packed)
                name_raw = unpacked[145:155]
                name = clean_name(name_raw)
                if not name: name = f"Patch {i+1}"
                all_patches.append((name, unpacked))
        except Exception as e:
            print(f"Error reading {filename}: {e}")

    if not all_patches:
        sys.exit(1)

    print(f"Generating patches.h with {len(all_patches)} presets...")
    with open("patches.h", "w") as h:
        h.write("#pragma once\n#include <cstdint>\n\n")
        h.write(f"static const int NUM_PATCHES = {len(all_patches)};\n\n")
        h.write("static const uint8_t FACTORY_BANK[][156] = {\n")
        for idx, (name, data) in enumerate(all_patches):
            h.write(f"    {{ /* {idx}: {name} */ ")
            h.write(", ".join(str(b) for b in data))
            h.write(" },\n")
        h.write("};\n")

    print("Generating dexed.ttl...")
    with open("dexed.ttl", "w") as t:
        t.write("@prefix doap:  <http://usefulinc.com/ns/doap#> .\n")
        t.write("@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .\n")
        t.write("@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .\n")
        t.write("@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .\n")
        t.write("@prefix units: <http://lv2plug.in/ns/extensions/units#> .\n")
        t.write("@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .\n")
        t.write("@prefix midi:  <http://lv2plug.in/ns/ext/midi#> .\n")
        t.write("@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .\n\n")
        t.write(f"<{URI}>\n")
        t.write("    a lv2:Plugin, lv2:InstrumentPlugin ;\n")
        t.write(f"    lv2:binary <{BINARY}> ;\n")
        t.write(f"    doap:name \"{NAME}\" ;\n")
        t.write("    lv2:optionalFeature lv2:hardRTCapable ;\n")
        t.write("    lv2:requiredFeature urid:map ;\n")
        t.write("""
    lv2:port [
        a lv2:OutputPort, lv2:AudioPort ;
        lv2:index 0 ;
        lv2:symbol "out_l" ;
        lv2:name "Out L" ;
    ] , [
        a lv2:OutputPort, lv2:AudioPort ;
        lv2:index 1 ;
        lv2:symbol "out_r" ;
        lv2:name "Out R" ;
    ] , [
        a lv2:InputPort, atom:AtomPort ;
        atom:bufferType atom:Sequence ;
        atom:supports midi:MidiEvent ;
        lv2:index 2 ;
        lv2:symbol "midi_in" ;
        lv2:name "MIDI In" ;
    ] , [
        a lv2:InputPort, lv2:ControlPort ;
        lv2:index 3 ;
        lv2:symbol "gain" ;
        lv2:name "Gain" ;
        lv2:default 1.0 ;
        lv2:minimum 0.0 ;
        lv2:maximum 2.0 ;
    ] , [
        a lv2:InputPort, lv2:ControlPort ;
        lv2:index 4 ;
        lv2:symbol "octave" ;
        lv2:name "Octave" ;
        lv2:default 0 ;
        lv2:minimum -3 ;
        lv2:maximum 3 ;
        lv2:portProperty lv2:integer ;
    ] , [
""")
        # PRESET (5)
        t.write("        a lv2:InputPort, lv2:ControlPort ;\n")
        t.write("        lv2:index 5 ;\n")
        t.write("        lv2:symbol \"preset\" ;\n")
        t.write("        lv2:name \"Preset\" ;\n")
        t.write("        lv2:default 0 ;\n")
        t.write("        lv2:minimum 0 ;\n")
        t.write(f"        lv2:maximum {len(all_patches) - 1} ;\n")
        t.write("        lv2:portProperty lv2:integer, lv2:enumeration ;\n")
        for i, (name, _) in enumerate(all_patches):
            t.write("        lv2:scalePoint [\n")
            t.write(f"            rdfs:label \"{i}: {name}\" ;\n")
            t.write(f"            rdf:value {i}\n")
            t.write("        ] ;\n")
        t.write("    ] , [\n")
        
        # ALGO (6)
        t.write("        a lv2:InputPort, lv2:ControlPort ;\n")
        t.write("        lv2:index 6 ;\n")
        t.write("        lv2:symbol \"algo\" ;\n")
        t.write("        lv2:name \"Algorithm\" ;\n")
        t.write("        lv2:default 0 ;\n")
        t.write("        lv2:minimum 0 ;\n")
        t.write("        lv2:maximum 32 ;\n")
        t.write("        lv2:portProperty lv2:integer, lv2:enumeration ;\n")
        t.write("        lv2:scalePoint [\n")
        t.write("            rdfs:label \"0: Preset\" ;\n")
        t.write("            rdf:value 0\n")
        t.write("        ] ;\n")
        for k in range(1, 33):
            t.write("        lv2:scalePoint [\n")
            t.write(f"            rdfs:label \"{k}\" ;\n")
            t.write(f"            rdf:value {k}\n")
            t.write("        ] ;\n")
        
        # Correctly close the final bracket instead of comma
        t.write("    ] .\n")
        
    print(f"Done! Combined {len(all_patches)} presets.")

if __name__ == "__main__":
    files = []
    if len(sys.argv) > 1:
        files = sys.argv[1:]
    else:
        files = sorted(glob.glob("*.syx"))
    if not files:
        sys.exit(1)
    generate_files(files)