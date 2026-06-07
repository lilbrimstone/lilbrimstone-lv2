# LilBrimstone LV2 Plugins for Isla Electronics S2400

Designed by Lilbrimstone, mostly coded by AI (Gemini 3.1 Pro for the most part)


*   **SH101:** My attempt at a clone. Includes tempo synced lfo.
*   **Juno:** Another clone
*   **Juno Arp:** A midi arp, inspired by the Juno arp. Put before any synth or midi controlled plugin.
*   **Dexed:** Dexed port, with 512 classic presets loaded. Dexed/MSFA code by Digital Suburban https://asb2m10.github.io/dexed/
*   **Minimoog:** Another clone. Unfortunately hits the S2400 limit of 32 LV2 controls so a couple aren't visible.
*   **Open303:** Port of Open303 by Robin Schmidt https://github.com/maddanio/open303
*   **Braids & Plaits:** Ports of the macro-oscillators Braids and Plaits by Émilie Gillet (Mutable Instruments) https://github.com/pichenettes/eurorack. The 6 op FM models in Plaits are currently not working properly. And Braids comes with no ADSR so it's currently just a drone.
*   **AKWF Single Cycles:** The classic single cycles as a 2 oscillator synth with detuning, mix and ring mod. Poly, mono and unison modes.
*   **Reverb:** Some basic reverb models. Probably not the highest quality reverb ever made...
*   **Phaser/Flanger:** Does what it says, selectable between phaser and flanger.
*   **Stereo Chorus:** Up to 8 voices
*   **Compressor:** Includes the possibility to trigger via MIDI. If you put it on a stereo return bus and route all your other DSP tracks into it you can get a pretty convincing sidechain.
*   **Tape Delay:** Tempo synced with a "warble" type modulation and "age" control

More to come...

# Install

Download / extract the zip and copy to the dspcard/Plugins/lv2 folder on your S2400. Sync DSP Card via Effects menu and power cycle (Shift + Back + Enter)

**Commands:**
Navigate to any plugin folder and run one of the following:

# Build a Windows DLL
make install_win

# Build an Aarch64 .so for the S2400
make install_s2400