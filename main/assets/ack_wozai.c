#include "assets/ack_wozai.h"

/*
 * TODO: Replace this placeholder PCM clip with a real 16 kHz mono recording of "我在".
 * The current asset is a short acknowledgement marker so the local routing path can be tested end-to-end.
 */
const int16_t g_ack_wozai_pcm[] = {
    0, 3211, 6392, 9512, 12539, 15446, 18204, 20787,
    23170, 25329, 27245, 28898, 30273, 31357, 32137, 32609,
    32767, 32609, 32137, 31357, 30273, 28898, 27245, 25329,
    23170, 20787, 18204, 15446, 12539, 9512, 6392, 3211,
    0, -3211, -6392, -9512, -12539, -15446, -18204, -20787,
    -23170, -25329, -27245, -28898, -30273, -31357, -32137, -32609,
    -32767, -32609, -32137, -31357, -30273, -28898, -27245, -25329,
    -23170, -20787, -18204, -15446, -12539, -9512, -6392, -3211,
};

const size_t g_ack_wozai_pcm_samples = sizeof(g_ack_wozai_pcm) / sizeof(g_ack_wozai_pcm[0]);
