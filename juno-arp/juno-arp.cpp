/*
 * LilBrimstone Juno Arp - Final (No Drift Correction)
 * Free-running accumulator. Proven on S2400.
 */

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/urid/urid.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdlib.h>

#define PLUGIN_URI "https://github.com/lilbrimstone/juno-arp"
#define MAX_NOTES 128

enum PortIndex {
    P_MIDI_IN = 0,
    P_MIDI_OUT,
    P_RATE,
    P_RANGE,
    P_MODE,
    P_GATE,
    P_HOLD,
    P_IN_L,
    P_IN_R,
    P_OUT_L,
    P_OUT_R
};

struct Note { uint8_t key, vel; };

typedef struct {
    double sampleRate;

    const LV2_Atom_Sequence* mIn;
    LV2_Atom_Sequence* mOut;
    const float* pRate;
    const float* pRange;
    const float* pMode;
    const float* pGate;
    const float* pHold;
    const float* inL;
    const float* inR;
    float* outL;
    float* outR;

    LV2_URID midi_Event;
    LV2_URID atom_Blank, atom_Object;
    LV2_URID atom_Float, atom_Double, atom_Int, atom_Long;
    LV2_URID time_Pos, time_Bpm, time_Speed, time_Bar;

    double bpm, phase, gateOff;
    float speed;
    int isPlaying, firstStep;

    uint8_t heldKeys[128];
    Note activeNotes[MAX_NOTES];
    int noteCount, playingKey, arpIndex, octIndex, dir;
} Arp;

static double rd(Arp* s, const LV2_Atom* a) {
    if (a->type == s->atom_Float)  return ((const LV2_Atom_Float*)a)->body;
    if (a->type == s->atom_Double) return ((const LV2_Atom_Double*)a)->body;
    if (a->type == s->atom_Int)    return ((const LV2_Atom_Int*)a)->body;
    if (a->type == s->atom_Long)   return ((const LV2_Atom_Long*)a)->body;
    return 0.0;
}

static void emit(Arp* s, uint32_t cap, int64_t t, uint8_t st, uint8_t d1, uint8_t d2) {
    struct Ev { LV2_Atom_Event e; uint8_t m[3]; };
    Ev ev;
    ev.e.time.frames = t;
    ev.e.body.type = s->midi_Event;
    ev.e.body.size = 3;
    ev.m[0] = st; ev.m[1] = d1; ev.m[2] = d2;
    lv2_atom_sequence_append_event(s->mOut, cap, &ev.e);
}

static void kill(Arp* s, uint32_t cap, uint32_t t) {
    if (s->playingKey >= 0) {
        emit(s, cap, t, 0x80, (uint8_t)s->playingKey, 0);
        s->playingKey = -1;
    }
}

static void sort_notes(Arp* s) {
    s->noteCount = 0;
    for (int i = 0; i < 128; i++) {
        if (s->heldKeys[i] > 0 && s->noteCount < MAX_NOTES) {
            s->activeNotes[s->noteCount].key = (uint8_t)i;
            s->activeNotes[s->noteCount].vel = s->heldKeys[i];
            s->noteCount++;
        }
    }
}

static double get_spb(const float* pRate) {
    int r = (int)(*pRate + 0.1f);
    switch(r) {
        case 0: return 0.5;   case 1: return 1.0;
        case 2: return 2.0;   case 3: return 3.0;
        case 4: return 4.0;   case 5: return 6.0;
        case 6: return 8.0;   case 7: return 12.0;
        case 8: return 16.0;  case 9: return 24.0;
        default: return 4.0;
    }
}

static void advance(Arp* s) {
    if (s->noteCount == 0) return;
    int range = (int)(*s->pRange + 0.5f);
    int mode = (int)(*s->pMode + 0.5f);

    if (mode == 3) {
        s->arpIndex = rand() % s->noteCount;
        s->octIndex = rand() % range;
        return;
    }
    if (mode == 0) {
        s->arpIndex++;
        if (s->arpIndex >= s->noteCount) { s->arpIndex = 0; s->octIndex++; }
        if (s->octIndex >= range) s->octIndex = 0;
    } else if (mode == 1) {
        s->arpIndex--;
        if (s->arpIndex < 0) { s->arpIndex = s->noteCount - 1; s->octIndex--; }
        if (s->octIndex < 0) s->octIndex = range - 1;
    } else if (mode == 2) {
        if (s->dir == 1) {
            s->arpIndex++;
            if (s->arpIndex >= s->noteCount) {
                if (s->octIndex < range - 1) { s->octIndex++; s->arpIndex = 0; }
                else { s->dir = -1; s->arpIndex = (s->noteCount > 1) ? s->noteCount - 2 : 0; }
            }
        } else {
            s->arpIndex--;
            if (s->arpIndex < 0) {
                if (s->octIndex > 0) { s->octIndex--; s->arpIndex = s->noteCount - 1; }
                else { s->dir = 1; s->arpIndex = (s->noteCount > 1) ? 1 : 0; }
            }
        }
    }
}

extern "C" {

static LV2_Handle instantiate(const LV2_Descriptor* d, double rate,
    const char* path, const LV2_Feature* const* features)
{
    if (!features) return NULL;
    LV2_URID_Map* map = NULL;
    for (int i = 0; features[i]; i++)
        if (!strcmp(features[i]->URI, LV2_URID__map)) { map = (LV2_URID_Map*)features[i]->data; break; }
    if (!map) return NULL;

    Arp* s = new Arp();
    s->sampleRate  = rate;
    s->midi_Event  = map->map(map->handle, LV2_MIDI__MidiEvent);
    s->atom_Blank  = map->map(map->handle, LV2_ATOM__Blank);
    s->atom_Object = map->map(map->handle, LV2_ATOM__Object);
    s->atom_Float  = map->map(map->handle, LV2_ATOM__Float);
    s->atom_Double = map->map(map->handle, LV2_ATOM__Double);
    s->atom_Int    = map->map(map->handle, LV2_ATOM__Int);
    s->atom_Long   = map->map(map->handle, LV2_ATOM__Long);
    s->time_Pos    = map->map(map->handle, "http://lv2plug.in/ns/ext/time#Position");
    s->time_Bpm    = map->map(map->handle, "http://lv2plug.in/ns/ext/time#beatsPerMinute");
    s->time_Speed  = map->map(map->handle, "http://lv2plug.in/ns/ext/time#speed");
    s->time_Bar    = map->map(map->handle, "http://lv2plug.in/ns/ext/time#barBeat");

    s->bpm = 120.0;
    s->speed = 0.0f;
    s->isPlaying = 0;
    s->firstStep = 1;
    s->phase = 0.0;
    s->gateOff = -1.0;
    s->playingKey = -1;
    s->arpIndex = 0;
    s->octIndex = 0;
    s->dir = 1;
    s->noteCount = 0;
    memset(s->heldKeys, 0, sizeof(s->heldKeys));

    return (LV2_Handle)s;
}

static void connect_port(LV2_Handle h, uint32_t port, void* data) {
    Arp* s = (Arp*)h;
    switch (port) {
        case P_MIDI_IN:  s->mIn = (const LV2_Atom_Sequence*)data; break;
        case P_MIDI_OUT: s->mOut = (LV2_Atom_Sequence*)data; break;
        case P_RATE:     s->pRate = (const float*)data; break;
        case P_RANGE:    s->pRange = (const float*)data; break;
        case P_MODE:     s->pMode = (const float*)data; break;
        case P_GATE:     s->pGate = (const float*)data; break;
        case P_HOLD:     s->pHold = (const float*)data; break;
        case P_IN_L:     s->inL = (const float*)data; break;
        case P_IN_R:     s->inR = (const float*)data; break;
        case P_OUT_L:    s->outL = (float*)data; break;
        case P_OUT_R:    s->outR = (float*)data; break;
    }
}

static void activate(LV2_Handle h) {}

static void run(LV2_Handle h, uint32_t n_samples) {
    Arp* s = (Arp*)h;
    if (!s->mIn || !s->mOut) return;

    // Audio thru
    if (s->outL && s->inL) memcpy(s->outL, s->inL, n_samples * sizeof(float));
    else if (s->outL) memset(s->outL, 0, n_samples * sizeof(float));
    if (s->outR && s->inR) memcpy(s->outR, s->inR, n_samples * sizeof(float));
    else if (s->outR) memset(s->outR, 0, n_samples * sizeof(float));

    const uint32_t cap = s->mOut->atom.size;
    lv2_atom_sequence_clear(s->mOut);
    s->mOut->atom.type = s->mIn->atom.type;

    // Parse input
    LV2_ATOM_SEQUENCE_FOREACH(s->mIn, ev) {
        if (ev->body.type == s->midi_Event) {
            const uint8_t* msg = (const uint8_t*)(ev + 1);
            uint8_t st = msg[0] & 0xF0;
            uint8_t k  = msg[1] & 0x7F;
            uint8_t v  = msg[2] & 0x7F;
            if (st == 0x90 && v > 0) {
                s->heldKeys[k] = v;
                sort_notes(s);
            } else if (st == 0x80 || (st == 0x90 && v == 0)) {
                s->heldKeys[k] = 0;
                if (*s->pHold < 0.5f) sort_notes(s);
            }
        } else if (ev->body.type == s->atom_Object || ev->body.type == s->atom_Blank) {
            const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
            if (obj->body.otype == s->time_Pos) {
                const LV2_Atom* a_bpm = NULL;
                const LV2_Atom* a_spd = NULL;
                lv2_atom_object_get(obj,
                    s->time_Bpm, &a_bpm,
                    s->time_Speed, &a_spd,
                    NULL);
                if (a_bpm) { double v = rd(s, a_bpm); if (v > 0) s->bpm = v; }
                if (a_spd) s->speed = (float)rd(s, a_spd);
            }
        }
    }

    // Transport edges
    int now = (s->speed > 0.0f) ? 1 : 0;
    if (now && !s->isPlaying) {
        kill(s, cap, 0);
        s->phase = 0.0;
        s->gateOff = -1.0;
        s->arpIndex = 0;
        s->octIndex = 0;
        s->dir = 1;
        s->firstStep = 1;
    }
    if (!now && s->isPlaying) {
        kill(s, cap, 0);
        memset(s->heldKeys, 0, sizeof(s->heldKeys));
        s->noteCount = 0;
    }
    s->isPlaying = now;

    // Need notes?
    int active = 0;
    for (int i = 0; i < 128; i++) if (s->heldKeys[i]) { active = 1; break; }
    if (*s->pHold > 0.5f && s->noteCount > 0) active = 1;
    if (!active) { kill(s, cap, 0); s->noteCount = 0; return; }
    if (s->noteCount == 0) sort_notes(s);
    if (s->noteCount == 0) return;

    // Timing
    double safeBpm = (s->bpm < 20.0) ? 120.0 : s->bpm;
    double spb = get_spb(s->pRate);
    double samplesPerStep = (s->sampleRate * 60.0) / (safeBpm * spb);
    float gp = *s->pGate;
    if (gp < 5) gp = 5;
    if (gp > 200) gp = 200;
    double gateLen = samplesPerStep * (gp / 100.0);

    // Sample loop
    for (uint32_t i = 0; i < n_samples; i++) {
        if (s->gateOff > 0.0) {
            s->gateOff -= 1.0;
            if (s->gateOff <= 0.0) {
                kill(s, cap, i);
                s->gateOff = -1.0;
            }
        }

        if (s->phase >= samplesPerStep || s->firstStep) {
            if (!s->firstStep) {
                s->phase -= samplesPerStep;
            } else {
                s->phase = 0.0;
                s->firstStep = 0;
            }

            kill(s, cap, i);

            if (s->arpIndex < 0 || s->arpIndex >= s->noteCount) s->arpIndex = 0;
            int k = s->activeNotes[s->arpIndex].key + (s->octIndex * 12);
            if (k > 127) k = 127;

            emit(s, cap, i, 0x90, (uint8_t)k, s->activeNotes[s->arpIndex].vel);
            s->playingKey = k;
            s->gateOff = gateLen;

            advance(s);
        }

        s->phase += 1.0;
    }
}

static void deactivate(LV2_Handle h) {}
static void cleanup(LV2_Handle h) { delete (Arp*)h; }
static const void* extension_data(const char* uri) { return NULL; }

static const LV2_Descriptor descriptor = {
    PLUGIN_URI, instantiate, connect_port, activate, run, deactivate, cleanup, extension_data
};

__attribute__((visibility("default")))
const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}

} // extern "C"