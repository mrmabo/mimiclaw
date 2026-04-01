#include "assets/ack_im_here.h"

/*
 * TODO: Replace this placeholder PCM clip with a real 16 kHz mono recording of "I'm here".
 * The current asset is a short acknowledgement marker so the local routing path can be tested end-to-end.
 */
const int16_t g_ack_im_here_pcm[] = {
    0, 6392, 12539, 18204, 23170, 27245, 30273, 32137,
    32767, 32137, 30273, 27245, 23170, 18204, 12539, 6392,
    0, -6392, -12539, -18204, -23170, -27245, -30273, -32137,
    -32767, -32137, -30273, -27245, -23170, -18204, -12539, -6392,
    0, 3211, 6392, 9512, 12539, 15446, 18204, 20787,
    23170, 25329, 27245, 28898, 30273, 31357, 32137, 32609,
    32767, 32609, 32137, 31357, 30273, 28898, 27245, 25329,
    23170, 20787, 18204, 15446, 12539, 9512, 6392, 3211,
};

const size_t g_ack_im_here_pcm_samples = sizeof(g_ack_im_here_pcm) / sizeof(g_ack_im_here_pcm[0]);
