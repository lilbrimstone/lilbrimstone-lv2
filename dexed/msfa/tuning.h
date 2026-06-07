#pragma once
#include <stdint.h>

// Dummy forward declaration to satisfy Dexed's MTS hooks
class MTSClient; 

// The bare-minimum Tuning base class (Implemented in our dexed.cpp)
class TuningState {
public:
    virtual ~TuningState() {}
    virtual int32_t midinote_to_logfreq(int midinote) = 0;
    virtual bool is_standard_tuning() = 0;
};
