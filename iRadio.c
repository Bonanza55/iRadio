/* fsk_wfmd_gapless.c  --  Gapless playback of raw PCM via ffmpeg
 *
 * Cross-platform (macOS & Linux) version streaming raw PCM data to a player process.
 * The player is ffmpeg writing to the platform-appropriate audio device.
 * Audio is written to the player pipe one demodulated block at a time (~27 ms).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <rtl-sdr.h>

/* ---- compile-time defaults ---- */
#define DEFAULT_FREQ_HZ        89300000u
#define DEFAULT_SAMP_RATE      1200000u
#define DEFAULT_GAIN_TENTHS    402
#define DEFAULT_PPM            0
#define DEFAULT_DEV_INDEX      0
#define DEFAULT_DEEMPH_US      75.0
#define DEFAULT_OUTDIR         "."
#define DEFAULT_AUDIO_GAIN     1.0
#define DEFAULT_SQUELCH_DB     -100.0f

#define IF_RATE                240000u
#define AUDIO_RATE             48000u

#define ASYNC_BUF_NUM          12
#define ASYNC_BUF_LEN          (1u << 18)
#define FLUSH_BLOCKS           4
#define PLL_TOL_HZ             1000
#define PLL_RETRIES            5

#define REINIT_ATTEMPTS        5
#define REINIT_BACKOFF_SEC     3

#define SQ_DC_BIAS             127.4f
#define BASE_AUDIO_SCALE       15000.0f

#define MAX_PATH_LEN           512
#define MAX_TAPS               192
#define DEMOD_SLICE_BYTES      (1u << 16)
#define MAX_BLOCK_SAMPS        (DEMOD_SLICE_BYTES / 2)
#define MAX_IF_SAMPS           (MAX_BLOCK_SAMPS / 2 + 8)
#define MAX_AUDIO_SAMPS        (MAX_IF_SAMPS / 5 + 8)
#define IQ_RING_BYTES          (32u * 1024u * 1024u)

/* Warmup settings for NFM startup noise elimination */
#define WARMUP_SAMPLES         4800    /* 100ms at 48kHz */

/* Squelch/Carrier detect settings */
#define CARRIER_HYSTERESIS     0.5f    /* 0.5 dB hysteresis to prevent chattering */
#define CARRIER_DETECT_TIME    0.020f  /* 20ms minimum carrier detection time */
#define CARRIER_LOSS_TIME      0.050f  /* 50ms carrier loss timeout */

/* Demodulation Modes */
typedef enum {
    MODE_NARROW = 0,
    MODE_WIDE   = 1
} demod_mode_t;

#define DEFAULT_DEMOD_MODE     MODE_WIDE
#define NFM_IF_RATE            48000u

_Static_assert(IF_RATE % AUDIO_RATE == 0, "IF must decimate integrally to audio");
_Static_assert(ASYNC_BUF_LEN % 512 == 0, "libusb bulk transfers need 512-B multiples");
_Static_assert(IQ_RING_BYTES > 4u * ASYNC_BUF_LEN, "ring must dwarf one transfer");

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int s) { (void)s; g_stop = 1; }

static char g_outdir[MAX_PATH_LEN] = DEFAULT_OUTDIR;
static int g_verbose = 0;

static void logmsg(const char *fmt, ...) {
    if (!g_verbose) return;

    char ts[32], line[512];
    time_t now = time(NULL);
    struct tm tmv;
    gmtime_r(&now, &tmv);
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tmv);

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s %s\n", ts, line);
    fflush(stderr);
}

/* =============================================================================
 * IQ RING
 * ===========================================================================*/
static uint8_t          s_iq_ring[IQ_RING_BYTES];
static size_t           iq_head = 0, iq_tail = 0;
static unsigned long    iq_overruns = 0;
static pthread_mutex_t  iq_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t   iq_cv = PTHREAD_COND_INITIALIZER;
static int              demod_quit = 0;

static void usb_callback(unsigned char *buf, uint32_t len, void *ctx) {
    rtlsdr_dev_t *dev = (rtlsdr_dev_t *)ctx;
    if (g_stop) { rtlsdr_cancel_async(dev); return; }

    pthread_mutex_lock(&iq_mx);
    const size_t space = (iq_tail > iq_head)
                       ? (iq_tail - iq_head - 1)
                       : (IQ_RING_BYTES - iq_head + iq_tail - 1);
    if (space < len) {
        iq_overruns += len;
    } else {
        size_t first = IQ_RING_BYTES - iq_head;
        if (first > len) first = len;
        memcpy(s_iq_ring + iq_head, buf, first);
        if (len > first) memcpy(s_iq_ring, buf + first, len - first);
        iq_head = (iq_head + len) % IQ_RING_BYTES;
        pthread_cond_signal(&iq_cv);
    }
    pthread_mutex_unlock(&iq_mx);
}

static size_t iq_ring_pull(uint8_t *dst, size_t cap) {
    pthread_mutex_lock(&iq_mx);
    while (iq_head == iq_tail && !demod_quit)
        pthread_cond_wait(&iq_cv, &iq_mx);
    if (demod_quit && iq_head == iq_tail) {
        pthread_mutex_unlock(&iq_mx);
        return 0;
    }
    size_t avail = (iq_head >= iq_tail)
                 ? (iq_head - iq_tail)
                 : (IQ_RING_BYTES - iq_tail + iq_head);
    size_t take = (avail < cap) ? avail : cap;
    take &= ~(size_t)1;
    if (take == 0) { pthread_mutex_unlock(&iq_mx); return 0; }

    size_t first = IQ_RING_BYTES - iq_tail;
    if (first > take) first = take;
    memcpy(dst, s_iq_ring + iq_tail, first);
    if (take > first) memcpy(dst + first, s_iq_ring, take - first);
    iq_tail = (iq_tail + take) % IQ_RING_BYTES;
    pthread_mutex_unlock(&iq_mx);
    return take;
}

/* =============================================================================
 * STREAMING FIR DECIMATOR
 * ===========================================================================*/
typedef struct {
    int   ntaps, dec, hlen, phase;
    float taps[MAX_TAPS];
    float hist[MAX_TAPS];
    float buf[MAX_TAPS + MAX_BLOCK_SAMPS];
} decim_t;

static void decim_design(decim_t *d, int dec, int ntaps, double fc_norm) {
    if (ntaps > MAX_TAPS) ntaps = MAX_TAPS;
    if (!(ntaps & 1)) ntaps--;
    d->ntaps = ntaps;
    d->dec   = dec;
    d->hlen  = ntaps - 1;
    d->phase = 0;

    const int M = ntaps - 1;
    double sum = 0.0;
    for (int n = 0; n < ntaps; n++) {
        double x = n - M / 2.0;
        double s = (x == 0.0) ? 2.0 * fc_norm
                              : sin(2.0 * M_PI * fc_norm * x) / (M_PI * x);
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * n / M);
        d->taps[n] = (float)(s * w);
        sum += s * w;
    }
    for (int n = 0; n < ntaps; n++) d->taps[n] /= (float)sum;
    memset(d->hist, 0, sizeof d->hist);
}

static int decim_run(decim_t *d, const float *in, int n, float *out) {
    memcpy(d->buf, d->hist, (size_t)d->hlen * sizeof(float));
    memcpy(d->buf + d->hlen, in, (size_t)n * sizeof(float));

    const int L = d->hlen + n;
    int p = d->phase, m = 0;
    while (p + d->ntaps <= L) {
        const float *b = d->buf + p;
        float acc = 0.0f;
        for (int k = 0; k < d->ntaps; k++) acc += b[k] * d->taps[k];
        out[m++] = acc;
        p += d->dec;
    }
    const int keep_from = L - d->hlen;
    memcpy(d->hist, d->buf + keep_from, (size_t)d->hlen * sizeof(float));
    d->phase = p - keep_from;
    return m;
}

/* =============================================================================
 * WFM / NFM DEMODULATOR
 * ===========================================================================*/
typedef struct {
    float rms_accum;
    int   sample_count;
    float current_db;
    int   carrier_detected;
    float carrier_db_threshold;
    float carrier_hysteresis;
    float carrier_detect_time;
    float carrier_loss_time;
    float carrier_on_time;
    float carrier_off_time;
    int   audio_muted;
} squelch_state_t;

static decim_t       s_dec1_i, s_dec1_q;
static decim_t       s_dec2;

static float         s_prev_i = 1.0f, s_prev_q = 0.0f;
static float         s_deemph_y = 0.0f, s_deemph_a = 0.0f;
static float         s_dcb_x1 = 0.0f, s_dcb_y1 = 0.0f;
static float         s_audio_scale = BASE_AUDIO_SCALE;
static demod_mode_t  s_current_mode = DEFAULT_DEMOD_MODE;
static float         s_squelch_db = DEFAULT_SQUELCH_DB;
static int           s_squelch_enabled = 1;
static int           s_warmup_samples = 0;
static squelch_state_t s_squelch = {0};

static float         s_fi[MAX_BLOCK_SAMPS],  s_fq[MAX_BLOCK_SAMPS];
static float         s_ifi[MAX_IF_SAMPS],    s_ifq[MAX_IF_SAMPS];
static float         s_disc[MAX_IF_SAMPS];
static float         s_aud[MAX_AUDIO_SAMPS];

static void squelch_init(float threshold_db, float hysteresis, float detect_time, float loss_time) {
    s_squelch.rms_accum = 0.0f;
    s_squelch.sample_count = 0;
    s_squelch.current_db = -100.0f;
    s_squelch.carrier_detected = 0;
    s_squelch.carrier_db_threshold = threshold_db;
    s_squelch.carrier_hysteresis = hysteresis;
    s_squelch.carrier_detect_time = detect_time;
    s_squelch.carrier_loss_time = loss_time;
    s_squelch.carrier_on_time = 0.0f;
    s_squelch.carrier_off_time = 0.0f;
    s_squelch.audio_muted = 1;  /* Start muted */
}

static void wfm_init(uint32_t rate, double deemph_us, double audio_gain, demod_mode_t mode, float squelch_db) {
    s_current_mode = mode;
    s_squelch_db = squelch_db;
    s_warmup_samples = 0;
    
    s_prev_i = 1.0f;
    s_prev_q = 0.0f;
    s_deemph_y = 0.0f;
    s_dcb_x1 = 0.0f;
    s_dcb_y1 = 0.0f;

    squelch_init(squelch_db, CARRIER_HYSTERESIS, CARRIER_DETECT_TIME, CARRIER_LOSS_TIME);

    if (mode == MODE_NARROW) {
        const int dec1 = (int)(rate / NFM_IF_RATE);
        decim_design(&s_dec1_i, dec1, 16 * dec1 + 1, 4000.0 / (double)rate);
        decim_design(&s_dec1_q, dec1, 16 * dec1 + 1, 4000.0 / (double)rate);
        s_audio_scale = (float)(BASE_AUDIO_SCALE * audio_gain * 12.0f);
    } else {
        const int dec1 = (int)(rate / IF_RATE);
        const int dec2 = (int)(IF_RATE / AUDIO_RATE);

        decim_design(&s_dec1_i, dec1, 16 * dec1 + 1, 100000.0 / (double)rate);
        decim_design(&s_dec1_q, dec1, 16 * dec1 + 1, 100000.0 / (double)rate);
        decim_design(&s_dec2, dec2, 161, 14000.0 / (double)IF_RATE);

        s_deemph_a    = 1.0f - (float)exp(-1.0 / (deemph_us * 1e-6 * (double)IF_RATE));
        s_audio_scale = (float)(BASE_AUDIO_SCALE * audio_gain);
    }
}

static void squelch_update(const int16_t *audio, int nsamp) {
    if (!s_squelch_enabled || nsamp == 0) {
        s_squelch.audio_muted = 0;
        return;
    }

    for (int i = 0; i < nsamp; i++) {
        float sample = (float)audio[i] / 32768.0f;
        s_squelch.rms_accum += sample * sample;
        s_squelch.sample_count++;
    }

    const int block_size = (int)(AUDIO_RATE * 0.020f);  /* 20ms at 48kHz */
    if (s_squelch.sample_count >= block_size) {
        float rms = sqrtf(s_squelch.rms_accum / s_squelch.sample_count);
        s_squelch.current_db = 20.0f * log10f(rms + 1e-10f);
        
        s_squelch.rms_accum = 0.0f;
        s_squelch.sample_count = 0;

        float threshold = s_squelch.carrier_detected 
                         ? (s_squelch.carrier_db_threshold - s_squelch.carrier_hysteresis)
                         : s_squelch.carrier_db_threshold;

        float block_time = (float)block_size / AUDIO_RATE;
        
        if (s_squelch.current_db >= threshold) {
            s_squelch.carrier_on_time += block_time;
            s_squelch.carrier_off_time = 0.0f;
            
            if (s_squelch.carrier_on_time >= s_squelch.carrier_detect_time) {
                s_squelch.carrier_detected = 1;
                s_squelch.audio_muted = 0;
            }
        } else {
            s_squelch.carrier_off_time += block_time;
            s_squelch.carrier_on_time = 0.0f;
            
            if (s_squelch.carrier_off_time >= s_squelch.carrier_loss_time) {
                s_squelch.carrier_detected = 0;
                s_squelch.audio_muted = 1;
            }
        }
    }
}

static int wfm_block(const uint8_t *iq, int nbytes, int16_t *out) {
    const int n = nbytes / 2;
    if (n <= 0 || n > (int)MAX_BLOCK_SAMPS) return 0;

    for (int k = 0; k < n; k++) {
        s_fi[k] = ((float)iq[2*k]     - SQ_DC_BIAS) * (1.0f / 128.0f);
        s_fq[k] = ((float)iq[2*k + 1] - SQ_DC_BIAS) * (1.0f / 128.0f);
    }

    int n_out = 0;

    if (s_current_mode == MODE_NARROW) {
        const int n1 = decim_run(&s_dec1_i, s_fi, n, s_ifi);
        (void)decim_run(&s_dec1_q, s_fq, n, s_ifq);

        float pi_ = s_prev_i, pq_ = s_prev_q;
        for (int k = 0; k < n1; k++) {
            const float ci = s_ifi[k], cq = s_ifq[k];
            const float re = ci * pi_ + cq * pq_;
            const float im = cq * pi_ - ci * pq_;
            s_disc[k] = atan2f(im, re);
            pi_ = ci; pq_ = cq;
        }
        s_prev_i = pi_; s_prev_q = pq_;

        float x1 = s_dcb_x1, y1 = s_dcb_y1;
        for (int k = 0; k < n1; k++) {
            const float x  = s_disc[k];
            const float hp = x - x1 + 0.9995f * y1;
            x1 = x; y1 = hp;
            float v = hp * s_audio_scale;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out[k] = (int16_t)lrintf(v);
        }
        s_dcb_x1 = x1; s_dcb_y1 = y1;
        n_out = n1;
    } else {
        const int n1 = decim_run(&s_dec1_i, s_fi, n, s_ifi);
        (void)decim_run(&s_dec1_q, s_fq, n, s_ifq);

        float pi_ = s_prev_i, pq_ = s_prev_q;
        for (int k = 0; k < n1; k++) {
            const float ci = s_ifi[k], cq = s_ifq[k];
            const float re = ci * pi_ + cq * pq_;
            const float im = cq * pi_ - ci * pq_;
            s_disc[k] = atan2f(im, re);
            pi_ = ci; pq_ = cq;
        }
        s_prev_i = pi_; s_prev_q = pq_;

        float y = s_deemph_y;
        const float a = s_deemph_a;
        for (int k = 0; k < n1; k++) {
            y += a * (s_disc[k] - y);
            s_disc[k] = y;
        }
        s_deemph_y = y;

        const int n2 = decim_run(&s_dec2, s_disc, n1, s_aud);

        float x1 = s_dcb_x1, y1 = s_dcb_y1;
        for (int k = 0; k < n2; k++) {
            const float x  = s_aud[k];
            const float hp = x - x1 + 0.9995f * y1;
            x1 = x; y1 = hp;
            float v = hp * s_audio_scale;
            if (v >  32767.0f) v =  32767.0f;
            if (v < -32768.0f) v = -32768.0f;
            out[k] = (int16_t)lrintf(v);
        }
        s_dcb_x1 = x1; s_dcb_y1 = y1;
        n_out = n2;
    }

    if (s_warmup_samples < WARMUP_SAMPLES) {
        int warmup_needed = WARMUP_SAMPLES - s_warmup_samples;
        int zero_count = (n_out < warmup_needed) ? n_out : warmup_needed;
        memset(out, 0, (size_t)zero_count * sizeof(int16_t));
        s_warmup_samples += n_out;
        
        if (s_warmup_samples >= WARMUP_SAMPLES) {
            logmsg("WARMUP complete (%d samples)", WARMUP_SAMPLES);
        }
        squelch_update(out, n_out);
        return n_out;
    }

    squelch_update(out, n_out);

    if (s_squelch.audio_muted) {
        memset(out, 0, (size_t)n_out * sizeof(int16_t));
    }

    return n_out;
}

/* =============================================================================
 * GAPLESS PLAYBACK: Cross-Platform Player Subprocess (macOS & Linux)
 * ===========================================================================*/

static pid_t player_pid = 0;
static int player_pipe = -1;

static int start_player_pipe(void) {
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        logmsg("ERR pipe: %s", strerror(errno));
        return -1;
    }
    
    pid_t pid = fork();
    if (pid < 0) {
        logmsg("ERR fork: %s", strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    
    if (pid == 0) {
        dup2(pipefd[0], STDIN_FILENO);
        if (pipefd[0] != STDIN_FILENO) close(pipefd[0]);
        close(pipefd[1]);
        dup2(STDERR_FILENO, STDOUT_FILENO);

#if defined(__APPLE__)
        /* macOS: Use AudioToolbox device output */
        execlp("ffmpeg", "ffmpeg",
               "-loglevel", "warning",
               "-nostats",
               "-f", "s16le",
               "-ar", "48000",
               "-ch_layout", "mono",
               "-i", "-",
               "-f", "audiotoolbox", "-",
               (char *)NULL);

        fprintf(stderr, "ffmpeg not found or failed. Install with: brew install ffmpeg\n");
#elif defined(__linux__)
        /* Linux: Use ALSA default output device */
        execlp("ffmpeg", "ffmpeg",
               "-loglevel", "warning",
               "-nostats",
               "-f", "s16le",
               "-ar", "48000",
               "-ch_layout", "mono",
               "-i", "-",
               "-f", "alsa", "default",
               (char *)NULL);

        fprintf(stderr, "ffmpeg not found or failed. Install via package manager (e.g., sudo apt install ffmpeg)\n");
#else
        fprintf(stderr, "Unsupported target operating system for automatic audio routing.\n");
#endif
        _exit(127);
    }

    close(pipefd[0]);
    player_pid = pid;
    player_pipe = pipefd[1];

    usleep(250000);
    if (waitpid(pid, NULL, WNOHANG) == pid) {
        logmsg("ERR player exited immediately (ffmpeg missing or bad args)");
        close(pipefd[0]);
        close(pipefd[1]);
        player_pipe = -1;
        player_pid  = 0;
        return -1;
    }
    return pipefd[1];
}

static void stop_player_pipe(void) {
    if (player_pipe >= 0) {
        close(player_pipe);
        player_pipe = -1;
    }
    if (player_pid <= 0) { player_pid = 0; return; }
    {
        int status;
        for (int i = 0; i < 20; i++) {
            if (waitpid(player_pid, &status, WNOHANG) == player_pid) {
                player_pid = 0;
                return;
            }
            usleep(100000);
        }
        kill(player_pid, SIGTERM);
        waitpid(player_pid, &status, 0);
        player_pid = 0;
    }
}

static int write_pcm_to_pipe(const int16_t *pcm, uint32_t nsamp) {
    if (player_pipe < 0) return -1;

    const uint8_t *p = (const uint8_t *)pcm;
    size_t left = (size_t)nsamp * 2u;
    while (left > 0) {
        ssize_t w = write(player_pipe, p, left);
        if (w < 0) {
            if (errno == EINTR) continue;
            logmsg("ERR pipe write audio: %s", strerror(errno));
            return -1;
        }
        p    += (size_t)w;
        left -= (size_t)w;
    }
    return 0;
}

/* =============================================================================
 * RTL-SDR SETUP
 * ===========================================================================*/
static uint8_t s_flush_buf[DEMOD_SLICE_BYTES];

static int snap_gain(rtlsdr_dev_t *dev, int want_tenths) {
    int n = rtlsdr_get_tuner_gains(dev, NULL);
    if (n <= 0) return want_tenths;

    int *gains = malloc((size_t)n * sizeof(int));
    if (!gains) return want_tenths;

    rtlsdr_get_tuner_gains(dev, gains);
    int best = gains[0], bestd = abs(gains[0] - want_tenths);
    for (int i = 1; i < n; i++) {
        int d = abs(gains[i] - want_tenths);
        if (d < bestd) { bestd = d; best = gains[i]; }
    }
    free(gains);
    return best;
}

static int configure_dongle(rtlsdr_dev_t *dev, uint32_t freq, uint32_t rate,
                            int gain_tenths, int ppm) {
    if (rtlsdr_set_sample_rate(dev, rate) < 0) {
        logmsg("ERR set_sample_rate(%u) failed", rate); return -1;
    }
    if (rtlsdr_set_tuner_gain_mode(dev, 1) < 0) {
        logmsg("ERR set_tuner_gain_mode(manual) failed"); return -1;
    }
    int g = snap_gain(dev, gain_tenths);
    if (rtlsdr_set_tuner_gain(dev, g) < 0) {
        logmsg("ERR set_tuner_gain(%d) failed", g); return -1;
    }
    rtlsdr_set_agc_mode(dev, 0);
    if (ppm) rtlsdr_set_freq_correction(dev, ppm);
    if (gain_tenths != g)
        logmsg("gain snapped %.1f -> %.1f dB", gain_tenths / 10.0, g / 10.0);

    int locked = 0;
    for (int i = 0; i < PLL_RETRIES; i++) {
        rtlsdr_set_center_freq(dev, freq);
        usleep(50000);
        uint32_t actual = rtlsdr_get_center_freq(dev);
        long err = (long)actual - (long)freq;
        if (labs(err) <= PLL_TOL_HZ) { locked = 1; break; }
        logmsg("PLL retry %d: want %u got %u (err %ld Hz)", i + 1, freq, actual, err);
    }
    if (!locked)
        logmsg("WARN PLL not confirmed within %d Hz; continuing", PLL_TOL_HZ);

    rtlsdr_reset_buffer(dev);
    for (int i = 0; i < FLUSH_BLOCKS; i++) {
        int nr = 0;
        rtlsdr_read_sync(dev, s_flush_buf, DEMOD_SLICE_BYTES, &nr);
    }
    return 0;
}

static int reinit_device(rtlsdr_dev_t **pdev, int dev_index, uint32_t freq,
                         uint32_t rate, int gain_tenths, int ppm) {
    if (*pdev) { rtlsdr_close(*pdev); *pdev = NULL; }

    for (int a = 1; a <= REINIT_ATTEMPTS && !g_stop; a++) {
        logmsg("device re-init attempt %d/%d (backoff %d s)",
               a, REINIT_ATTEMPTS, REINIT_BACKOFF_SEC);
        sleep(REINIT_BACKOFF_SEC);
        if (rtlsdr_open(pdev, (uint32_t)dev_index) < 0) {
            *pdev = NULL;
            continue;
        }
        if (configure_dongle(*pdev, freq, rate, gain_tenths, ppm) == 0) {
            logmsg("device re-init OK");
            return 0;
        }
        rtlsdr_close(*pdev); *pdev = NULL;
    }
    return -1;
}

/* =============================================================================
 * DEMOD THREAD
 * ===========================================================================*/

static int16_t s_ablk[MAX_AUDIO_SAMPS];
static uint8_t s_slice[DEMOD_SLICE_BYTES];

static int  g_noplay = 0;
static int  g_verbose_squelch = 0;

static void *demod_thread(void *arg) {
    (void)arg;
    unsigned long overruns_seen = 0;

    for (;;) {
        size_t got = iq_ring_pull(s_slice, DEMOD_SLICE_BYTES);
        if (got == 0) break;

        int na = wfm_block(s_slice, (int)got, s_ablk);

        if (g_verbose_squelch && s_squelch_enabled) {
            static int last_carrier_state = -1;
            if (s_squelch.carrier_detected != last_carrier_state) {
                logmsg("SQUELCH: carrier %s (%.1f dB, threshold %.1f dB)",
                       s_squelch.carrier_detected ? "DETECTED" : "LOST",
                       (double)s_squelch.current_db,
                       (double)s_squelch.carrier_db_threshold);
                last_carrier_state = s_squelch.carrier_detected;
            }
        }

        if (!g_noplay && na > 0) {
            if (write_pcm_to_pipe(s_ablk, (uint32_t)na) != 0) {
                logmsg("ERR player pipe closed");
                break;
            }
        }

        pthread_mutex_lock(&iq_mx);
        unsigned long ovr = iq_overruns;
        pthread_mutex_unlock(&iq_mx);

        if (ovr != overruns_seen) {
            logmsg("WARN IQ ring overran: %lu bytes dropped",
                   ovr - overruns_seen);
            overruns_seen = ovr;
        }
    }
    logmsg("demod thread done");
    return NULL;
}

/* =============================================================================
 * MAIN
 * ===========================================================================*/
static uint32_t parse_freq(const char *s) {
    char *end = NULL;
    double v = strtod(s, &end);
    if (end == s || v <= 0.0) {
        fprintf(stderr, "Error: bad frequency '%s'\n", s);
        exit(2);
    }
    if (v < 100000.0) v *= 1e6;
    if (v > 4.294967295e9) {
        fprintf(stderr, "Error: frequency %.0f Hz out of range\n", v);
        exit(2);
    }
    return (uint32_t)llround(v);
}

static void usage(const char *p) {
    fprintf(stderr,
      "usage: %s [-f freq] [-p ppm] [-s samp_rate] [-g gain_tenths] [-d dev_index]\n"
      "          [-e deemph_us] [-a audio_gain] [-q squelch_db] [-b mode] [-o outdir] [-N] [-v] [-S] [-Q]\n"
      "  -f  frequency in MHz (104.5, 151.820) or Hz\n"
      "  -b  demodulation mode: 'n' for narrowband (NFM), 'w' for wideband (WFM)\n"
      "  -q  squelch threshold in dBFS (default %.1f)\n"
      "  -a  audio gain multiplier (default %.1f)\n"
      "  -N  do not launch player (dry run / mute)\n"
      "  -S  write raw PCM to stdout instead of forking a player\n"
      "  -Q  verbose squelch logging (log carrier detect/loss events)\n"
      "  -v  enable verbose logging to stderr\n"
      "  \n"
      "  NOTE: Gapless playback requires ffmpeg with appropriate device output support\n"
      "  defaults: -f %.4f -p %d -s %u -g %d -d %d -e %.0f -q %.1f -b %s -o %s\n",
      p, (double)DEFAULT_SQUELCH_DB, (double)DEFAULT_AUDIO_GAIN,
      (double)DEFAULT_FREQ_HZ / 1e6, DEFAULT_PPM, DEFAULT_SAMP_RATE, DEFAULT_GAIN_TENTHS,
      DEFAULT_DEV_INDEX, DEFAULT_DEEMPH_US, (double)DEFAULT_SQUELCH_DB,
      DEFAULT_DEMOD_MODE == MODE_NARROW ? "n" : "w", DEFAULT_OUTDIR);
}

int main(int argc, char **argv) {
    uint32_t freq = DEFAULT_FREQ_HZ, rate = DEFAULT_SAMP_RATE;
    int gain = DEFAULT_GAIN_TENTHS, ppm = DEFAULT_PPM, dev_index = DEFAULT_DEV_INDEX;
    int noplay = 0;
    double deemph_us = DEFAULT_DEEMPH_US, audio_gain = DEFAULT_AUDIO_GAIN;
    float squelch_db = DEFAULT_SQUELCH_DB;
    demod_mode_t demod_mode = DEFAULT_DEMOD_MODE;
    const char *outdir = DEFAULT_OUTDIR;
    int stdout_pcm = 0;

    int c;
    while ((c = getopt(argc, argv, "f:p:s:g:d:e:a:q:b:o:NvShQ")) != -1) {
        switch (c) {
            case 'f': freq = parse_freq(optarg); break;
            case 'p': ppm  = atoi(optarg); break;
            case 's': rate = (uint32_t)strtoul(optarg, NULL, 10); break;
            case 'g': gain = atoi(optarg); break;
            case 'd': dev_index = atoi(optarg); break;
            case 'e': deemph_us = atof(optarg); break;
            case 'a': audio_gain = atof(optarg); break;
            case 'q': squelch_db = (float)atof(optarg); break;
            case 'Q': g_verbose_squelch = 1; break;
            case 'b':
                if (optarg[0] == 'n' || optarg[0] == 'N') {
                    demod_mode = MODE_NARROW;
                } else if (optarg[0] == 'w' || optarg[0] == 'W') {
                    demod_mode = MODE_WIDE;
                } else {
                    fprintf(stderr, "Error: invalid mode '-b %s'. Use 'n' or 'w'.\n", optarg);
                    return 2;
                }
                break;
            case 'o': outdir = optarg; break;
            case 'N': noplay = 1; break;
            case 'S': stdout_pcm = 1; break;
            case 'v': g_verbose = 1; break;
            case 'h': default: usage(argv[0]); return (c == 'h') ? 0 : 2;
        }
    }

    if (strlen(outdir) >= MAX_PATH_LEN) {
        fprintf(stderr, "Error: outdir path exceeds %d bytes.\n", MAX_PATH_LEN);
        return 1;
    }
    if (rate % IF_RATE != 0 || rate / IF_RATE < 2) {
        fprintf(stderr, "Error: samp_rate %u must be a multiple of %u.\n", rate, IF_RATE);
        return 1;
    }
    if (audio_gain <= 0.0 || audio_gain > 16.0) {
        fprintf(stderr, "Error: audio_gain must be in (0, 16].\n");
        return 1;
    }
    snprintf(g_outdir, sizeof g_outdir, "%s", outdir);
    g_noplay = noplay;

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    wfm_init(rate, deemph_us, audio_gain, demod_mode, squelch_db);

    pthread_t demod_tid;
    
    if (!noplay && stdout_pcm) {
        player_pipe = STDOUT_FILENO;
        player_pid  = 0;
        logmsg("streaming raw PCM to stdout (48000 Hz, mono, s16le)");
    } else if (!noplay) {
        if (start_player_pipe() < 0) {
            fprintf(stderr, "Error: cannot start player pipe.\n");
#if defined(__APPLE__)
            fprintf(stderr, "Install ffmpeg: brew install ffmpeg\n");
#elif defined(__linux__)
            fprintf(stderr, "Install ffmpeg: sudo apt install ffmpeg (or equivalent package manager)\n");
#endif
            fprintf(stderr, "Or use -S to pipe PCM to your own player\n");
            return 1;
        }
#if defined(__APPLE__)
        logmsg("player pipe started (gapless playback via ffmpeg -> audiotoolbox)");
#elif defined(__linux__)
        logmsg("player pipe started (gapless playback via ffmpeg -> alsa)");
#endif
    }

    if (pthread_create(&demod_tid, NULL, demod_thread, NULL) != 0) {
        fprintf(stderr, "Error: cannot start demod thread.\n");
        return 1;
    }

    rtlsdr_dev_t *dev = NULL;
    if (rtlsdr_open(&dev, (uint32_t)dev_index) < 0) {
        logmsg("ERR cannot open RTL-SDR device %d", dev_index);
        return 1;
    }
    if (configure_dongle(dev, freq, rate, gain, ppm) < 0) {
        rtlsdr_close(dev); return 1;
    }

    logmsg("fsk_wfmd up (gapless): f=%.4f MHz  s=%u sps  g=%.1f dB  ppm=%d  dev=%d mode=%s squelch=%.1f dB",
           freq / 1e6, rate, snap_gain(dev, gain) / 10.0, ppm, dev_index,
           demod_mode == MODE_NARROW ? "NARROW" : "WIDE", (double)squelch_db);
    logmsg("playback=%s  usb queue=%dx%u KiB  iq ring=%.1f s",
           noplay ? "OFF"
                  : (stdout_pcm ? "stdout (raw PCM)"
                                : "ffmpeg (gapless raw PCM)"),
           ASYNC_BUF_NUM, ASYNC_BUF_LEN / 1024u,
           (double)IQ_RING_BYTES / 2.0 / (double)rate);

    int fatal = 0;
    while (!g_stop) {
        rtlsdr_reset_buffer(dev);
        int r = rtlsdr_read_async(dev, usb_callback, dev,
                                  ASYNC_BUF_NUM, ASYNC_BUF_LEN);
        if (g_stop) break;

        logmsg("WARN read_async returned r=%d; device fault", r);
        logmsg("NOTE audio discontinuity in current stream (device re-init)");
        if (reinit_device(&dev, dev_index, freq, rate, gain, ppm) < 0) {
            logmsg("FATAL device unrecoverable after %d attempts; exiting",
                   REINIT_ATTEMPTS);
            fatal = 1;
            break;
        }
    }

    pthread_mutex_lock(&iq_mx);
    demod_quit = 1;
    pthread_cond_broadcast(&iq_cv);
    pthread_mutex_unlock(&iq_mx);
    pthread_join(demod_tid, NULL);

    if (!noplay && !stdout_pcm) {
        stop_player_pipe();
    }
    
    if (dev) rtlsdr_close(dev);

    logmsg("fsk_wfmd shutting down %s", fatal ? "(FATAL)" : "cleanly");
    return fatal ? 1 : 0;
}
