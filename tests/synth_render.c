/* synth_render.c - renders a .mid to raw float32 stereo (44.1 kHz) for offline checks.  usage: synth_render in.mid out.raw seconds */
#include "../runtime/synth.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc < 4) return 2;
    FILE *f = fopen(argv[1], "rb"); if (!f) return 1; static uint8_t buf[1 << 20]; int n = (int)fread(buf, 1, sizeof buf, f); fclose(f);
    MidiSong *s = midi_parse(buf, n, 44100); if (!s) { fprintf(stderr, "parse failed\n"); return 1; }
    MidiPlayer *p = midi_player_new(s, 44100); int frames = (int)(atof(argv[3]) * 44100);
    float *out = calloc((size_t)frames * 2, sizeof(float)); int done = 0;
    while (done < frames) { int k = frames - done > 441 ? 441 : frames - done; midi_render(p, out + 2 * done, k); done += k; }
    f = fopen(argv[2], "wb"); fwrite(out, sizeof(float), (size_t)frames * 2, f); fclose(f);
    printf("song %.2fs\n", midi_song_seconds(s)); return 0;
}
