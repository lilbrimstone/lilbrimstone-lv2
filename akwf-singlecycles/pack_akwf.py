import os, glob
import numpy as np
from scipy.io import wavfile
import re

def clean_var(s):
    return re.sub(r'[^a-zA-Z0-9_]', '_', s)

def build():
    base_dir = "akwf"
    if not os.path.exists(base_dir):
        os.makedirs(base_dir)
        print(f"Created '{base_dir}/'. Drop subfolders full of .wav files here!")
        return

    folders = sorted([f for f in os.listdir(base_dir) if os.path.isdir(os.path.join(base_dir, f))])
    if not folders:
        print(f"No subfolders found inside '{base_dir}/'.")
        return

    print(f"Found {len(folders)} AKWF categories. Packing...")

    h_file = open("AkwfWaves.h", "w")
    h_file.write("#ifndef AKWF_WAVES_H\n#define AKWF_WAVES_H\n\n")
    h_file.write("#define AKWF_LENGTH 600\n\n")
    h_file.write("struct AkwfFolder {\n    const float** waves;\n    int num_waves;\n};\n\n")

    folder_data = []
    max_waves_global = 0
    ttl_folder_dropdown = ""

    for i, fol in enumerate(folders):
        wav_files = sorted(glob.glob(os.path.join(base_dir, fol, "*.wav")))
        if not wav_files: continue
        
        if len(wav_files) > max_waves_global:
            max_waves_global = len(wav_files)

        fol_label = fol[5:] if fol.upper().startswith("AKWF_") else fol
        fol_label = fol_label[:14] 
        fol_c_name = clean_var(fol)

        wave_array_names = []
        
        for w in wav_files:
            w_name = os.path.splitext(os.path.basename(w))[0]
            w_c_name = clean_var(w_name)
            
            sr, data = wavfile.read(w)
            
            if data.dtype == np.int16: data = data.astype(np.float32) / 32768.0
            elif data.dtype == np.int32: data = data.astype(np.float32) / 2147483648.0
            elif data.dtype == np.float64: data = data.astype(np.float32)
            if len(data.shape) > 1: data = data[:, 0] 
            
            if len(data) > 600: data = data[:600]
            elif len(data) < 600: data = np.pad(data, (0, 600 - len(data)), 'constant')
                
            wave_arr_id = f"wave_{fol_c_name}_{w_c_name}"
            wave_array_names.append(wave_arr_id)
            
            h_file.write(f"static const float {wave_arr_id}[AKWF_LENGTH] = {{\n")
            for j in range(0, 600, 8):
                chunk = data[j:j+8]
                h_file.write("    " + ", ".join([f"{x:.6f}f" for x in chunk]) + ",\n")
            h_file.write("};\n\n")

        ptr_array_name = f"ptr_{fol_c_name}"
        h_file.write(f"static const float* const {ptr_array_name}[{len(wave_array_names)}] = {{\n")
        for wa in wave_array_names:
            h_file.write(f"    {wa},\n")
        h_file.write("};\n\n")
        
        folder_data.append({
            "label": fol_label,
            "ptr": ptr_array_name,
            "count": len(wave_array_names)
        })
        
        ttl_folder_dropdown += f"        lv2:scalePoint [ rdfs:label \"{fol_label}\" ; rdf:value {i} ] ;\n"

    h_file.write(f"#define NUM_FOLDERS {len(folder_data)}\n\n")
    h_file.write("static const AkwfFolder g_folders[NUM_FOLDERS] = {\n")
    for fol in folder_data:
        h_file.write(f"    {{ (const float**){fol['ptr']}, {fol['count']} }},\n")
    h_file.write("};\n\n#endif // AKWF_WAVES_H\n")
    h_file.close()
    
    # --- GENERATE FULL TTL FILE ---
    ttl_content = f"""@prefix atom:  <http://lv2plug.in/ns/ext/atom#> .
@prefix doap:  <http://usefulinc.com/ns/doap#> .
@prefix lv2:   <http://lv2plug.in/ns/lv2core#> .
@prefix rdf:   <http://www.w3.org/1999/02/22-rdf-syntax-ns#> .
@prefix rdfs:  <http://www.w3.org/2000/01/rdf-schema#> .
@prefix urid:  <http://lv2plug.in/ns/ext/urid#> .
@prefix time:  <http://lv2plug.in/ns/ext/time#> .
@prefix midi:  <http://lv2plug.in/ns/ext/midi#> .
@prefix pprop: <http://lv2plug.in/ns/ext/port-props#> .

<https://github.com/lilbrimstone/akwf-singlecycles>
    a lv2:Plugin ;
    a lv2:InstrumentPlugin ;
    doap:name "LilBrimstone AKWF Single Cycles" ;
    lv2:requiredFeature urid:map ;
    
    lv2:port [
        a lv2:InputPort ;
        a atom:AtomPort ;
        lv2:index 0 ;
        lv2:symbol "midi_in" ;
        lv2:name "MIDI In" ;
        atom:bufferType atom:Sequence ;
        atom:supports midi:MidiEvent , time:Position ;
    ] , [
        a lv2:OutputPort ;
        a atom:AtomPort ;
        lv2:index 1 ;
        lv2:symbol "midi_out" ;
        lv2:name "MIDI Out" ;
        atom:bufferType atom:Sequence ;
        atom:supports midi:MidiEvent ;
    ] , [
        a lv2:AudioPort ;
        a lv2:OutputPort ;
        lv2:index 2 ;
        lv2:symbol "out_l" ;
        lv2:name "Out L" ;
    ] , [
        a lv2:AudioPort ;
        a lv2:OutputPort ;
        lv2:index 3 ;
        lv2:symbol "out_r" ;
        lv2:name "Out R" ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 4 ;
        lv2:symbol "folder_a" ;
        lv2:name "Folder A" ;
        lv2:portProperty lv2:integer ;
        lv2:portProperty lv2:enumeration ;
        lv2:default 0 ;
        lv2:minimum 0 ;
        lv2:maximum {len(folder_data)-1} ;
{ttl_folder_dropdown}    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 5 ;
        lv2:symbol "wave_a" ;
        lv2:name "Wave Num A" ;
        lv2:portProperty lv2:integer ;
        lv2:default 0 ;
        lv2:minimum 0 ;
        lv2:maximum {max_waves_global-1} ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 6 ;
        lv2:symbol "coarse_a" ;
        lv2:name "Coarse A" ;
        lv2:portProperty lv2:integer ;
        lv2:default 0 ;
        lv2:minimum -24 ;
        lv2:maximum 24 ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 7 ;
        lv2:symbol "fine_a" ;
        lv2:name "Fine A" ;
        lv2:default 0.0 ;
        lv2:minimum -100.0 ;
        lv2:maximum 100.0 ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 8 ;
        lv2:symbol "folder_b" ;
        lv2:name "Folder B" ;
        lv2:portProperty lv2:integer ;
        lv2:portProperty lv2:enumeration ;
        lv2:default 0 ;
        lv2:minimum 0 ;
        lv2:maximum {len(folder_data)-1} ;
{ttl_folder_dropdown}    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 9 ;
        lv2:symbol "wave_b" ;
        lv2:name "Wave Num B" ;
        lv2:portProperty lv2:integer ;
        lv2:default 0 ;
        lv2:minimum 0 ;
        lv2:maximum {max_waves_global-1} ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 10 ;
        lv2:symbol "coarse_b" ;
        lv2:name "Coarse B" ;
        lv2:portProperty lv2:integer ;
        lv2:default 0 ;
        lv2:minimum -24 ;
        lv2:maximum 24 ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 11 ;
        lv2:symbol "fine_b" ;
        lv2:name "Fine B" ;
        lv2:default 0.0 ;
        lv2:minimum -100.0 ;
        lv2:maximum 100.0 ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 12 ;
        lv2:symbol "crossfade" ;
        lv2:name "A/B Mix" ;
        lv2:default 0.5 ;
        lv2:minimum 0.0 ;
        lv2:maximum 1.0 ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 13 ;
        lv2:symbol "ringmod" ;
        lv2:name "Ring Mod Amount" ;
        lv2:default 0.0 ;
        lv2:minimum 0.0 ;
        lv2:maximum 1.0 ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 14 ;
        lv2:symbol "voice_mode" ;
        lv2:name "Voice Mode" ;
        lv2:portProperty lv2:integer ;
        lv2:portProperty lv2:enumeration ;
        lv2:default 0 ;
        lv2:minimum 0 ;
        lv2:maximum 2 ;
        lv2:scalePoint [ rdfs:label "Poly" ; rdf:value 0 ] ;
        lv2:scalePoint [ rdfs:label "Mono" ; rdf:value 1 ] ;
        lv2:scalePoint [ rdfs:label "Unison" ; rdf:value 2 ] ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 15 ;
        lv2:symbol "detune" ;
        lv2:name "Detune/Slop" ;
        lv2:default 10.0 ;
        lv2:minimum 0.0 ;
        lv2:maximum 100.0 ;
    ] , [
        a lv2:InputPort ;
        a lv2:ControlPort ;
        lv2:index 16 ;
        lv2:symbol "spread" ;
        lv2:name "Spread" ;
        lv2:default 0.5 ;
        lv2:minimum 0.0 ;
        lv2:maximum 1.0 ;
    ] .
"""
    with open("akwf-singlecycles.ttl", "w") as f:
        f.write(ttl_content)
    
    print("Done! Wrote AkwfWaves.h and akwf-singlecycles.ttl")

if __name__ == "__main__":
    build()