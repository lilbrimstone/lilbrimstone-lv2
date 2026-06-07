#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/time/time.h>
#include <lv2/urid/urid.h>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <new>
#include <cstdlib>

#include "Saw.h"
#include "Square.h"
#include "Filter.h"
#include "ADSR.h"
#include "LFO.h"

#define PLUGIN_URI "https://github.com/lilbrimstone/sh101"
#define NUM_VOICES 1 

const float LN2 = 0.69314718056f;

enum PortIndex {
    PORT_EVENTS_IN = 0,
    PORT_EVENTS_OUT = 1,
    PORT_OUT_L = 2,
    PORT_OUT_R = 3,
    
    PORT_LFO_RATE = 4,
    PORT_LFO_SYNC = 5,
    PORT_LFO_SYNC_RATE = 6,
    PORT_LFO_WAVE = 7,
    
    PORT_MOD_VCO = 8,
    PORT_PWM_SOURCE = 9,
    PORT_PULSE_WIDTH = 10,
    
    PORT_SQUARE_LEVEL = 11,
    PORT_SAW_LEVEL = 12,
    PORT_SUB_LEVEL = 13,
    PORT_SUB_MODE = 14,
    PORT_NOISE_LEVEL = 15,
    
    PORT_CUTOFF = 16,
    PORT_RESONANCE = 17,
    PORT_FILTER_ENV = 18,
    PORT_MOD_VCF = 19,
    PORT_KEY_TRACK = 20,
    
    PORT_AMP_MODE = 21,
    
    PORT_ENV_TRIGGER = 22,
    PORT_ENV_ATTACK = 23,
    PORT_ENV_DECAY = 24,
    PORT_ENV_SUSTAIN = 25,
    PORT_ENV_RELEASE = 26,
    
    PORT_PORTAMENTO = 27,
    PORT_PORTAMENTO_MODE = 28,
    
    PORT_BEND_RANGE = 29,
    PORT_BEND_VCF = 30,
    PORT_WHEEL_AMOUNT = 31
};

struct Voice {
    bool active = false;    
    bool keyHeld = false;   
    int note = -1;
    
    float phase = 0.0f;
    float phaseInc = 0.0f;
    
    float targetFreq = 440.0f;  
    float currentFreq = 440.0f; 
    
    int cycleCount = 0;
    
    uint32_t rngState = 0xCAFEBABE;

    PolyBLEPSaw saw;
    SquareVoice square;
    SquareVoice sub1;
    SquareVoice sub2;
    SH101Filter filter;
    SH101ADSR env;

    inline float nextNoise() {
        rngState = rngState * 1664525 + 1013904223;
        union { float f; uint32_t i; } pun;
        pun.i = (rngState & 0x007FFFFF) | 0x3F800000; 
        return (pun.f - 1.5f) * 2.0f; 
    }
};

struct SH101Osc {
    float* outL = nullptr;
    float* outR = nullptr;
    const LV2_Atom_Sequence* events = nullptr;
    LV2_Atom_Sequence* eventsOut = nullptr;
    
    float* lfoRate = nullptr;
    float* lfoSync = nullptr;
    float* lfoSyncRate = nullptr;
    float* lfoWave = nullptr;
    float* portamento = nullptr; 
    float* portamentoMode = nullptr;
    
    float* modVco = nullptr;
    float* pwmSource = nullptr;
    float* pulseWidth = nullptr;
    
    float* squareLevel = nullptr;
    float* sawLevel = nullptr;
    float* subLevel = nullptr;
    float* subMode = nullptr;
    float* noiseLevel = nullptr;
    
    float* cutoff = nullptr;
    float* resonance = nullptr;
    float* filterEnvAmt = nullptr;
    float* modVcf = nullptr;
    float* keyTrack = nullptr;
    
    float* ampMode = nullptr;
    
    float* envTrigger = nullptr;
    float* envA = nullptr;
    float* envD = nullptr;
    float* envS = nullptr;
    float* envR = nullptr;
    
    float* bendRange = nullptr;
    float* bendVcf = nullptr;
    float* wheelAmount = nullptr;

    // URIDs
    LV2_URID midi_Event = 0;
    LV2_URID time_Position = 0;
    LV2_URID time_bpm = 0;
    LV2_URID time_speed = 0;
    
    LV2_URID atom_Blank = 0;
    LV2_URID atom_Object = 0;
    LV2_URID atom_Float = 0;
    LV2_URID atom_Double = 0;
    LV2_URID atom_Int = 0;
    LV2_URID atom_Long = 0;

    float sampleRate = 48000.0f;
    double bpm = 120.0;
    float hostSpeed = 0.0f;
    bool wasPlaying = false;
    
    float pitchBend = 0.0f;
    float modWheel = 0.0f;
    
    Voice voice;
    int currentNote = -1;
    
    SH101LFO lfo;
};

// Type-agnostic atom reader
static double read_atom(SH101Osc* self, const LV2_Atom* atom) {
    if (atom->type == self->atom_Float)
        return (double)((const LV2_Atom_Float*)atom)->body;
    if (atom->type == self->atom_Double)
        return ((const LV2_Atom_Double*)atom)->body;
    if (atom->type == self->atom_Int)
        return (double)((const LV2_Atom_Int*)atom)->body;
    if (atom->type == self->atom_Long)
        return (double)((const LV2_Atom_Long*)atom)->body;
    return 0.0;
}

static inline float midiNoteToHz(int note) {
    return 440.0f * powf(2.0f, (note - 69.0f) / 12.0f);
}

static double syncBeatsFromIdx(int index) {
    switch (index) {
        case 0:  return 16.0;     
        case 1:  return 8.0;      
        case 2:  return 4.0;      
        case 3:  return 2.0;      
        case 4:  return 1.0;      
        case 5:  return 1.5;      
        case 6:  return 2.0/3.0;  
        case 7:  return 0.5;      
        case 8:  return 0.75;     
        case 9:  return 1.0/3.0;  
        case 10: return 0.25;     
        case 11: return 0.375;    
        case 12: return 1.0/6.0;  
        case 13: return 0.125;    
        case 14: return 0.0625;   
        default: return 1.0;
    }
}

static void noteOn(SH101Osc* self, int note, int vel) {
    Voice& v = self->voice;
    const int trigMode = self->envTrigger ? (int)(*self->envTrigger) : 1; 
    const int glideMode = self->portamentoMode ? (int)(*self->portamentoMode) : 0; 
    const bool wasLegato = v.keyHeld;
    const bool wasInitialized = (v.note != -1);

    self->currentNote = note;
    v.active = true;
    v.keyHeld = true;
    v.note = note;
    
    float target = midiNoteToHz(note);
    v.targetFreq = target;
    
    float glideKnob = self->portamento ? *self->portamento : 0.0f;
    bool doGlide = false;

    if (!wasInitialized) doGlide = false;
    else if (glideKnob > 0.01f) {
        if (glideMode == 0) doGlide = wasLegato;
        else if (glideMode == 1) doGlide = false;
        else if (glideMode == 2) doGlide = true;
    }

    if (!doGlide) v.currentFreq = target; 

    v.square.velocity = 1.0f;
    v.sub1.velocity = 1.0f;
    v.sub2.velocity = 1.0f;

    if (trigMode == 2) v.env.noteOn(); 
    else if (trigMode == 1) { if (!wasLegato) v.env.noteOn(); }
}

static void noteOff(SH101Osc* self, int note) {
    if (self->currentNote == note) {
        self->voice.keyHeld = false;
        self->voice.env.noteOff();
        self->currentNote = -1;
    }
}

extern "C" {

static LV2_Handle instantiate(const LV2_Descriptor*, double rate, const char*, const LV2_Feature* const* features) {
    LV2_URID_Map* map = nullptr;
    for (int i=0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            map = (LV2_URID_Map*)features[i]->data;
            break;
        }
    }
    
    void* ptr = calloc(1, sizeof(SH101Osc));
    if (!ptr) return nullptr;
    SH101Osc* self = new(ptr) SH101Osc();
    
    self->sampleRate = (float)rate;
    
    if (map) {
        self->midi_Event    = map->map(map->handle, LV2_MIDI__MidiEvent);
        self->time_Position = map->map(map->handle, LV2_TIME__Position);
        self->time_bpm      = map->map(map->handle, LV2_TIME__beatsPerMinute);
        self->time_speed    = map->map(map->handle, LV2_TIME__speed);
        
        self->atom_Blank  = map->map(map->handle, LV2_ATOM__Blank);
        self->atom_Object = map->map(map->handle, LV2_ATOM__Object);
        self->atom_Float  = map->map(map->handle, LV2_ATOM__Float);
        self->atom_Double = map->map(map->handle, LV2_ATOM__Double);
        self->atom_Int    = map->map(map->handle, LV2_ATOM__Int);
        self->atom_Long   = map->map(map->handle, LV2_ATOM__Long);
    }

    self->voice.active = false;
    self->voice.keyHeld = false;
    self->voice.saw.init(rate);
    self->voice.square.init(rate);
    self->voice.sub1.init(rate);
    self->voice.sub2.init(rate);
    self->voice.filter.init(rate);
    self->voice.env.init((float)rate);
    
    self->voice.targetFreq = 261.63f;
    self->voice.currentFreq = 261.63f;
    self->voice.rngState = (uint32_t)rate;
    self->currentNote = -1;
    self->pitchBend = 0.0f;
    self->modWheel = 0.0f;
    
    self->bpm = 120.0;
    self->hostSpeed = 0.0f;
    self->wasPlaying = false;
    
    self->lfo.init((float)rate);
    
    return (LV2_Handle)self;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    SH101Osc* self = (SH101Osc*)instance;
    switch (port) {
        case PORT_EVENTS_IN:   self->events = (const LV2_Atom_Sequence*)data; break;
        case PORT_EVENTS_OUT:  self->eventsOut = (LV2_Atom_Sequence*)data; break;
        case PORT_OUT_L:       self->outL = (float*)data; break;
        case PORT_OUT_R:       self->outR = (float*)data; break;
        
        case PORT_LFO_RATE:    self->lfoRate = (float*)data; break;
        case PORT_LFO_SYNC:    self->lfoSync = (float*)data; break;
        case PORT_LFO_SYNC_RATE:self->lfoSyncRate = (float*)data; break;
        case PORT_LFO_WAVE:    self->lfoWave = (float*)data; break;
        
        case PORT_MOD_VCO:     self->modVco = (float*)data; break;
        case PORT_PWM_SOURCE:  self->pwmSource = (float*)data; break;
        case PORT_PULSE_WIDTH: self->pulseWidth = (float*)data; break;
        
        case PORT_SQUARE_LEVEL:self->squareLevel = (float*)data; break;
        case PORT_SAW_LEVEL:   self->sawLevel = (float*)data; break;
        case PORT_SUB_LEVEL:   self->subLevel = (float*)data; break;
        case PORT_SUB_MODE:    self->subMode = (float*)data; break;
        case PORT_NOISE_LEVEL: self->noiseLevel = (float*)data; break;
        
        case PORT_CUTOFF:      self->cutoff = (float*)data; break;
        case PORT_RESONANCE:   self->resonance = (float*)data; break;
        case PORT_FILTER_ENV:  self->filterEnvAmt = (float*)data; break;
        case PORT_MOD_VCF:     self->modVcf = (float*)data; break;
        case PORT_KEY_TRACK:   self->keyTrack = (float*)data; break;
        
        case PORT_AMP_MODE:    self->ampMode = (float*)data; break;
        
        case PORT_ENV_TRIGGER: self->envTrigger = (float*)data; break;
        case PORT_ENV_ATTACK:  self->envA = (float*)data; break;
        case PORT_ENV_DECAY:   self->envD = (float*)data; break;
        case PORT_ENV_SUSTAIN: self->envS = (float*)data; break;
        case PORT_ENV_RELEASE: self->envR = (float*)data; break;
        
        case PORT_PORTAMENTO:  self->portamento = (float*)data; break;
        case PORT_PORTAMENTO_MODE: self->portamentoMode = (float*)data; break;
        
        case PORT_BEND_RANGE:  self->bendRange = (float*)data; break;
        case PORT_BEND_VCF:    self->bendVcf = (float*)data; break;
        case PORT_WHEEL_AMOUNT:self->wheelAmount = (float*)data; break;
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    SH101Osc* self = (SH101Osc*)instance;

    // --- OSC LEVELS ---
    float sawLvl = self->sawLevel ? (*self->sawLevel * 0.1f) : 0.0f;
    float sqLvl = self->squareLevel ? (*self->squareLevel * 0.1f) : 1.0f;
    float subLvl = self->subLevel ? (*self->subLevel * 0.1f) : 0.0f;
    int subMd = self->subMode ? (int)(*self->subMode) : 0;
    float noiseLvl = self->noiseLevel ? (*self->noiseLevel * 0.1f) : 0.0f;
    
    // --- FILTER ---
    float cutoffKnob = self->cutoff ? *self->cutoff : 10.0f;
    float baseCutoffHz = 20.0f * powf(2000.0f, cutoffKnob/10.0f);
    float resoKnob = self->resonance ? *self->resonance : 0.0f;
    float reso01 = resoKnob / 10.0f;
    
    float envAmtKnob = self->filterEnvAmt ? *self->filterEnvAmt : 0.0f;
    float envAmt = envAmtKnob / 10.0f;
    if (envAmt < 0.0f) envAmt = 0.0f; 
    if (envAmt > 1.0f) envAmt = 1.0f;
    
    int ampMode = self->ampMode ? (int)(*self->ampMode) : 0;
    int trigMode = self->envTrigger ? (int)(*self->envTrigger) : 1;

    // --- ENVELOPE ---
    float eA = self->envA ? *self->envA : 0.0f;
    float eD = self->envD ? *self->envD : 2.0f;
    float eS = self->envS ? *self->envS : 10.0f;
    float eR = self->envR ? *self->envR : 2.0f;
    
    // --- PORTAMENTO ---
    float glideKnob = self->portamento ? *self->portamento : 0.0f;
    float glideCoeff = 1.0f;
    if (glideKnob > 0.01f) {
        float norm = glideKnob * 0.1f; 
        float sec = 5.0f * (norm * norm); 
        if (sec < 0.001f) sec = 0.001f;
        glideCoeff = 1.0f / (sec * self->sampleRate);
        if (glideCoeff > 1.0f) glideCoeff = 1.0f;
    }

    Voice& v = self->voice;
    int lfoWave = self->lfoWave ? (int)(*self->lfoWave) : 0;
    
    // --- LFO FREQUENCY ---
    bool syncOn = self->lfoSync ? (*self->lfoSync > 0.5f) : false;
    float lfoHz = 1.0f;

    if (syncOn) {
        int syncIdx = self->lfoSyncRate ? (int)(*self->lfoSyncRate) : 6;
        double stepBeats = syncBeatsFromIdx(syncIdx);
        double currentBpm = (self->bpm < 20.0) ? 120.0 : self->bpm;
        lfoHz = (float)(currentBpm / (60.0 * stepBeats));
    } else {
        float lfoRateKnob = self->lfoRate ? *self->lfoRate : 5.0f;
        lfoHz = 0.1f * powf(300.0f, lfoRateKnob / 10.0f);
    }

    // --- MIDI OUTPUT RESET ---
    uint32_t outCapacity = 0;
    if (self->eventsOut) {
        outCapacity = self->eventsOut->atom.size;
        lv2_atom_sequence_clear(self->eventsOut);
        self->eventsOut->atom.type = self->midi_Event;
        if (self->events) self->eventsOut->atom.type = self->events->atom.type;
    }

    // --- EVENT PARSING ---
    if (self->events) {
        LV2_ATOM_SEQUENCE_FOREACH(self->events, ev) {
            
            if (ev->body.type == self->atom_Object || ev->body.type == self->atom_Blank) {
                const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
                if (obj->body.otype == self->time_Position) {
                    const LV2_Atom* bpmAtom   = NULL;
                    const LV2_Atom* speedAtom = NULL;
                    lv2_atom_object_get(obj,
                        self->time_bpm,   &bpmAtom,
                        self->time_speed, &speedAtom,
                        NULL);
                    if (bpmAtom) {
                        double val = read_atom(self, bpmAtom);
                        if (val > 0.0) self->bpm = val;
                    }
                    if (speedAtom) {
                        self->hostSpeed = (float)read_atom(self, speedAtom);
                    }
                }
            }
            else if (ev->body.type == self->midi_Event) {
                const uint8_t* msg = (const uint8_t*)(ev+1);
                uint8_t status = msg[0] & 0xF0;
                if (status == 0x90 && msg[2] > 0) noteOn(self, msg[1], msg[2]);
                else if (status == 0x80 || (status == 0x90 && msg[2] == 0)) noteOff(self, msg[1]);
                else if (status == 0xE0) { 
                    int val = msg[1] | (msg[2] << 7);
                    self->pitchBend = (val - 8192) / 8192.0f;
                }
                else if (status == 0xB0 && msg[1] == 1) { 
                    self->modWheel = msg[2] / 127.0f;
                }
            }
            if (self->eventsOut) {
                lv2_atom_sequence_append_event(self->eventsOut, outCapacity, ev);
            }
        }
    }

    // --- TRANSPORT STATE MACHINE ---
    bool isPlaying = (self->hostSpeed > 0.0f);
    
    // PLAY edge: reset LFO phase to lock to grid
    if (isPlaying && !self->wasPlaying) {
        if (syncOn) self->lfo.phase = 0.0f;
    }
    self->wasPlaying = isPlaying;

    // --- MOD DEPTHS ---
    float wheelAmtKnob = self->wheelAmount ? *self->wheelAmount : 10.0f;
    float mwNorm = self->modWheel * (wheelAmtKnob / 10.0f);
    float panelModVco = self->modVco ? (*self->modVco * 0.1f) : 0.0f;
    float combinedVco = panelModVco + mwNorm; 
    float depthOctaves = (combinedVco * combinedVco) * 3.0f; 
    
    float compensationScaler = 1.0f;
    if (depthOctaves > 0.001f) {
        float k = depthOctaves * LN2; 
        switch (lfoWave) {
            case 0: if (k > 0.01f) compensationScaler = k / sinhf(k); break;
            case 1: compensationScaler = 1.0f / coshf(k); break;
            case 2: if (k > 0.01f) compensationScaler = k / sinhf(k); break;
            case 3: if (k > 0.01f) compensationScaler = k / sinhf(k); break;
        }
    }

    float panelModVcf = self->modVcf ? (*self->modVcf * 0.1f) : 0.0f;
    float combinedVcf = panelModVcf + mwNorm;
    float pwKnob = self->pulseWidth ? (*self->pulseWidth * 0.1f) : 0.0f;
    int pwmSource = self->pwmSource ? (int)(*self->pwmSource) : 0;
    float ktKnob = self->keyTrack ? *self->keyTrack : 10.0f;
    float ktRatio = ktKnob / 10.0f; 
    float maxCutoffHz = self->sampleRate * 0.45f;
    float bendRangeVal = self->bendRange ? *self->bendRange : 2.0f;
    float oscBendFactor = powf(2.0f, (self->pitchBend * bendRangeVal) / 12.0f);
    float bendVcfKnob = self->bendVcf ? *self->bendVcf : 0.0f;
    float vcfBendOctaves = self->pitchBend * (bendVcfKnob / 10.0f) * 2.0f;

    // --- AUDIO LOOP ---
    memset(self->outL, 0, n_samples*sizeof(float));
    memset(self->outR, 0, n_samples*sizeof(float));

    if (v.active) {
        v.env.setAttackKnob(eA);
        v.env.setDecayKnob(eD);
        v.env.setSustainKnob(eS);
        v.env.setReleaseKnob(eR);
        v.filter.setResonance(reso01);

        float ktOctaves = 0.0f;
        if (v.note >= 0) ktOctaves = ((float)v.note - 60.0f) / 12.0f * ktRatio;
        
        for (uint32_t i=0; i<n_samples; ++i) {
            
            bool trig = self->lfo.step(lfoHz);
            
            // LFO TRIGGER for ENV
            if (trigMode == 0 && trig && v.keyHeld) v.env.noteOn();

            float lfoOut = self->lfo.getSample(lfoWave);

            if (glideKnob > 0.01f && fabsf(v.targetFreq - v.currentFreq) > 0.001f) {
                v.currentFreq += (v.targetFreq - v.currentFreq) * glideCoeff;
            } else {
                v.currentFreq = v.targetFreq;
            }

            // --- OSC FREQ ---
            float rawMod = powf(2.0f, lfoOut * depthOctaves);
            float freqMod = rawMod * compensationScaler;
            
            float oscFreq = v.currentFreq * freqMod * oscBendFactor;
            if (oscFreq < 10.0f) oscFreq = 10.0f;
            if (oscFreq > 22000.0f) oscFreq = 22000.0f; 

            v.phaseInc = oscFreq / self->sampleRate;

            v.saw.setFrequency(oscFreq); 
            v.square.setFrequency(oscFreq);
            v.sub1.setFrequency(oscFreq * 0.5f);
            v.sub2.setFrequency(oscFreq * 0.25f);
            
            float envVal = v.env.processEnv();
            float currentPW = (pwmSource == 0) ? pwKnob :
                              (pwmSource == 1) ? pwKnob * ((lfoOut + 1.0f) * 0.5f) :
                              (pwmSource == 2) ? pwKnob * ((self->lfo.getTriangle() + 1.0f) * 0.5f) :
                              pwKnob * envVal;
            
            float sig = 0.0f;
            float sqPhase = v.phase + 0.5f; if (sqPhase>=1.0f) sqPhase-=1.0f;
            v.square.setPhase(sqPhase);
            float sub1Phase = v.phase*0.5f + (v.cycleCount&1)*0.5f; if(sub1Phase>=1.0f)sub1Phase-=1.0f;
            float sub2Phase = v.phase*0.25f + (v.cycleCount&3)*0.25f; if(sub2Phase>=1.0f)sub2Phase-=1.0f;
            v.sub1.setPhase(sub1Phase);
            v.sub2.setPhase(sub2Phase);
            
            if (sawLvl>0.001f) sig += v.saw.next()*sawLvl;
            if (sqLvl>0.001f) sig += v.square.process(currentPW)*sqLvl;
            
            if (subLvl>0.001f) {
                float subOut = (subMd == 0) ? v.sub1.process(0.0f) :
                               (subMd == 1) ? v.sub2.process(0.0f) :
                               v.sub2.process(0.5f);
                sig += subOut*subLvl*0.75f;
            }
            if (noiseLvl > 0.001f) sig += v.nextNoise() * noiseLvl;
            
            v.phase += v.phaseInc;
            if (v.phase >= 1.0f) { v.phase -= 1.0f; v.cycleCount++; }
            v.saw.setPhase(v.phase);
            
            float ampVal = (ampMode == 0) ? v.env.processAmpFromEnv(envVal) : v.env.processGateAmp();
            
            float cutoffHz = baseCutoffHz * powf(2.0f, (envAmt * envVal * 9.25f) + 
                                                       (lfoOut * combinedVcf * 8.0f) + 
                                                       ktOctaves + 
                                                       vcfBendOctaves);
            
            if (cutoffHz > maxCutoffHz) cutoffHz = maxCutoffHz;
            if (cutoffHz < 5.0f) cutoffHz = 5.0f;
            v.filter.setCutoff(cutoffHz);
            
            float out = v.filter.process(sig);
            out *= ampVal * 0.25f;
            
            self->outL[i] = out;
            self->outR[i] = out;
            
            if (ampVal <= 1e-6f && !v.env.isActive() && !v.keyHeld) v.active = false;
        }
    }
}

static void cleanup(LV2_Handle instance) { 
    SH101Osc* self = (SH101Osc*)instance;
    self->~SH101Osc();
    free(self);
}

static const LV2_Descriptor descriptor = { 
    PLUGIN_URI, instantiate, connect_port, nullptr, run, nullptr, cleanup, nullptr 
};

#ifdef _WIN32
__declspec(dllexport)
#else
__attribute__((visibility("default")))
#endif
const LV2_Descriptor* lv2_descriptor(uint32_t index) { 
    return index == 0 ? &descriptor : nullptr; 
}
} // extern "C"