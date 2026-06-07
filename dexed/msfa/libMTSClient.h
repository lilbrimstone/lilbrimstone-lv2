#pragma once
#include <math.h>

class MTSClient {};

inline bool MTS_HasMaster(MTSClient* client) { return false; }

inline double MTS_NoteToFrequency(MTSClient* client, int midinote, int channel) {
    return 440.0 * pow(2.0, (midinote - 69.0) / 12.0);
}
