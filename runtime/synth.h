/* synth.h - Standard MIDI File parser + small software synthesiser (no soundfont needed) */
#ifndef SYNTH_H
#define SYNTH_H
#include <stdint.h>
typedef struct MidiSong MidiSong;
typedef struct MidiPlayer MidiPlayer;
MidiSong   *midi_parse(const uint8_t *data, int len, int rate);       /* NULL if not a valid SMF */
void        midi_song_free(MidiSong *s);
double      midi_song_seconds(const MidiSong *s);
MidiPlayer *midi_player_new(const MidiSong *s, int rate);
void        midi_player_free(MidiPlayer *p);
void        midi_rewind(MidiPlayer *p);                                /* silence + back to start */
void        midi_release_all(MidiPlayer *p);                           /* note-offs (used by stop) */
/* Adds `frames` stereo float frames into out. Returns 1 while sounding/pending, 0 once finished. */
int         midi_render(MidiPlayer *p, float *out, int frames);
#endif
