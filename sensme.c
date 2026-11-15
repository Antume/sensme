/*
 * sensme.c - Terminal SensMe-like music mood classifier
 *
 * Decodes each audio file with libmpg123, extracts audio features
 * (tempo, RMS loudness, spectral centroid, spectral flux, zero-crossing rate),
 * maps them onto a 2D mood plane (valence x intensity) and copies the files
 * into mood-named folders. Source files are never modified or deleted.
 *
 * Build:
 *   make                 (Linux / macOS)
 *   make STATIC=1        (single self-contained binary)
 *   There is an interactive prompt when sensme is started without arguments
 *   on a terminal: it asks for the source and output folders.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <mpg123.h>

#ifndef SENSME_VERSION
#define SENSME_VERSION "0.1"
#endif

/* ---- platform shims (POSIX + Windows/MinGW) ---------------------- */
#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  include <windows.h>
#  ifndef PATH_MAX
#    define PATH_MAX 4096
#  endif
#  define strcasecmp _stricmp
#  define mkdir_mode(p, m) _mkdir(p)
#  define realpath(p, buf) _fullpath((buf), (p), PATH_MAX)
#  define lstat stat
#  ifndef S_ISLNK
#    define S_ISLNK(m) 0
#  endif
static char *win_strcasestr(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    if (!n) return (char *)hay;
    for (const char *p = hay; *p; p++)
        if (!_strnicmp(p, needle, n)) return (char *)p;
    return NULL;
}
#  define strcasestr win_strcasestr
#else
#  define mkdir_mode(p, m) mkdir((p), (m))
#endif

/* ------------------------------------------------------------------ */
/* tuning                                                             */
/* ------------------------------------------------------------------ */
#define FFT_N        2048          /* analysis window (~46 ms @44.1k) */
#define HOP          1024          /* ~21.5 frames/s                 */
#define BANDS        24
#define MAX_SECONDS  90.0          /* analysis excerpt length        */
#define MAX_SAMPLES  (2L * 48000L * 60L)  /* 60 s of stereo reserve */

#define BPM_LO       60.0
#define BPM_HI       175.0
#define CENT_LO      400.0
#define CENT_HI      4200.0
#define FLUX_LO      0.10
#define FLUX_HI      1.60

static int g_color = 1;
static const char *C_RESET = "", *C_BOLD = "", *C_DIM = "";
static const char *C_TITLE = "", *C_OK = "", *C_WARN = "", *C_ERR = "";

/* ------------------------------------------------------------------ */
/* moods                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *folder;
    const char *label;
    double x, y;
} Mood;

/* x: sad(-1) .. happy(+1)   y: calm(-1) .. intense(+1)
 * centres are in absolute (calibrated) coordinates, not library ranks */
static const Mood MOODS[] = {
    { "01_Happy_Upbeat",   "Happy / Upbeat",    +0.45, +0.45 },
    { "02_Energetic",      "Energetic",         +0.05, +0.65 },
    { "03_Calm",           "Calm / Relaxing",   +0.10, -0.55 },
    { "04_Sad_Melancholy", "Sad / Melancholy",  -0.40, -0.45 },
    { "05_Dark_Intense",   "Dark / Intense",    -0.45, +0.55 },
    { "06_Emotional",      "Emotional",         -0.05, +0.05 },
    { "07_Bright",         "Bright / Cheerful", +0.55,  0.00 },
};
#define NMOODS ((int)(sizeof(MOODS) / sizeof(MOODS[0])))

/* ------------------------------------------------------------------ */
/* helpers                                                            */
/* ------------------------------------------------------------------ */
static void color_init(void)
{
    const char *t = getenv("TERM");
    if (getenv("NO_COLOR") || !t || !strcmp(t, "dumb") || !isatty(STDOUT_FILENO))
        return;
    g_color    = 1;
    C_RESET    = "\033[0m";
    C_BOLD     = "\033[1m";
    C_DIM      = "\033[2m";
    C_TITLE    = "\033[1;36m";
    C_OK       = "\033[1;32m";
    C_WARN     = "\033[1;33m";
    C_ERR      = "\033[1;31m";
}

static double clamp01(double v) { return v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v); }
static double clampd(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) { perror("malloc"); exit(1); }
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) { perror("realloc"); exit(1); }
    return q;
}

static char *xstrdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *r = xmalloc(n);
    memcpy(r, s, n);
    return r;
}

static char *join_path(const char *a, const char *b)
{
    size_t n = strlen(a) + strlen(b) + 2;
    char *r = xmalloc(n);
    snprintf(r, n, "%s/%s", a, b);
    return r;
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int is_audio_ext(const char *name)
{
    const char *e = strrchr(name, '.');
    if (!e) return 0;
    return !strcasecmp(e, ".mp3");
}

static int mkdir_p(const char *path)
{
    char *tmp = xstrdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir_mode(tmp, 0755) != 0 && errno != EEXIST) { free(tmp); return -1; }
            *p = '/';
        }
    }
    int rc = mkdir_mode(tmp, 0755);
    free(tmp);
    return (rc == 0 || errno == EEXIST) ? 0 : -1;
}

static int path_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

static int is_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static const char *home_dir(void)
{
    const char *h = getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) h = getenv("USERPROFILE");
#endif
    return (h && *h) ? h : NULL;
}

/* strip surrounding whitespace / newline and one pair of quotes */
static void clean_path(char *s)
{
    char *b = s, *e;
    size_t n;

    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);

    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' ||
                     e[-1] == '\r' || e[-1] == '\n'))
        *--e = 0;

    n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') ||
                   (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = 0;
        clean_path(s);
    }
}

/* expand a leading "~" or "~/" using $HOME */
static const char *expand_tilde(const char *in, char *buf, size_t n)
{
    const char *home = home_dir();
    if (in[0] == '~' && (in[1] == 0 || in[1] == '/' || in[1] == '\\') && home)
        snprintf(buf, n, "%s%s", home, in + 1);
    else
        snprintf(buf, n, "%s", in);
    return buf;
}

/* ------------------------------------------------------------------ */
/* file collection                                                    */
/* ------------------------------------------------------------------ */
typedef struct { char **v; int n, cap; } StrList;

static void sl_push(StrList *l, char *s)
{
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->v = xrealloc(l->v, sizeof(char *) * (size_t)l->cap);
    }
    l->v[l->n++] = s;
}

static void sl_free(StrList *l)
{
    for (int i = 0; i < l->n; i++) free(l->v[i]);
    free(l->v);
    memset(l, 0, sizeof *l);
}

static void scan_dir(const char *dir, StrList *out)
{
    DIR *d = opendir(dir);
    if (!d) return;
    StrList sub = {0};
    struct dirent *de;
    while ((de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char *full = join_path(dir, de->d_name);
        struct stat st;
        if (lstat(full, &st) != 0) { free(full); continue; }
        if (S_ISLNK(st.st_mode)) { free(full); continue; }
        if (S_ISDIR(st.st_mode)) sl_push(&sub, full);
        else if (S_ISREG(st.st_mode) && is_audio_ext(de->d_name)) sl_push(out, full);
        else free(full);
    }
    closedir(d);
    for (int i = 0; i < sub.n; i++) { scan_dir(sub.v[i], out); free(sub.v[i]); }
    free(sub.v);
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ------------------------------------------------------------------ */
/* FFT                                                                */
/* ------------------------------------------------------------------ */
static double *g_win;
static double *g_cos, *g_sin;
static int    *g_rev;
static int     g_n;

static void fft_init(int n)
{
    g_n   = n;
    g_win = xmalloc(sizeof(double) * n);
    g_cos = xmalloc(sizeof(double) * (n / 2));
    g_sin = xmalloc(sizeof(double) * (n / 2));
    g_rev = xmalloc(sizeof(int) * n);

    for (int i = 0; i < n; i++)
        g_win[i] = 0.5 - 0.5 * cos(2.0 * M_PI * i / (n - 1));
    for (int i = 0; i < n / 2; i++) {
        double a = -2.0 * M_PI * i / n;
        g_cos[i] = cos(a);
        g_sin[i] = sin(a);
    }
    int bits = 0;
    while ((1 << bits) < n) bits++;
    for (int i = 0; i < n; i++) {
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        g_rev[i] = r;
    }
}

static void fft_free(void)
{
    free(g_win); free(g_cos); free(g_sin); free(g_rev);
}

static void fft_run(double *re, double *im)
{
    int n = g_n;
    for (int i = 0; i < n; i++) {
        int j = g_rev[i];
        if (j > i) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (int len = 2; len <= n; len <<= 1) {
        int h = len / 2, tstep = n / len;
        for (int i = 0; i < n; i += len) {
            for (int j = 0, k = 0; j < h; j++, k += tstep) {
                int a = i + j, b = a + h;
                double wr = g_cos[k], wi = g_sin[k];
                double tr = re[b] * wr - im[b] * wi;
                double ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* decoding                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    float *mono;        /* mono samples in [-1,1] */
    long   n;
    long   rate;
    double duration;    /* seconds */
    char   err[160];
} Audio;

static void audio_free(Audio *a)
{
    free(a->mono);
    memset(a, 0, sizeof *a);
}

/*
 * Decode up to max_sec seconds starting from the middle of the track
 * (skips long intros/outros). Returns 0 on success.
 */
static int decode_audio(const char *path, double max_sec, Audio *a)
{
    memset(a, 0, sizeof *a);

    int errnum = 0;
    mpg123_handle *mh = mpg123_new(NULL, &errnum);
    if (!mh) {
        snprintf(a->err, sizeof a->err, "mpg123_new: %s", mpg123_plain_strerror(errnum));
        return -1;
    }
    if (mpg123_open(mh, path) != MPG123_OK) {
        snprintf(a->err, sizeof a->err, "%s", mpg123_strerror(mh));
        mpg123_delete(mh);
        return -1;
    }

    long rate = 0;
    int channels = 0, enc = 0;
    mpg123_getformat(mh, &rate, &channels, &enc);
    if (rate <= 0 || channels <= 0) {
        snprintf(a->err, sizeof a->err, "unsupported format");
        mpg123_close(mh); mpg123_delete(mh);
        return -1;
    }

    mpg123_format_none(mh);
    mpg123_format(mh, rate, channels, MPG123_ENC_SIGNED_16);

    off_t total = mpg123_length(mh);
    double dur_est = (total > 0) ? (double)total / rate : 0.0;

    /* pick a start offset in the middle third for long tracks */
    long long skip_frames = 0;
    if (dur_est > max_sec * 1.6)
        skip_frames = (long long)((dur_est * 0.40) * rate);
    if (skip_frames > 0)
        mpg123_seek(mh, (off_t)skip_frames, SEEK_SET);

    long max_frames = (long)(max_sec * rate);
    long cap = (max_frames > 0 ? max_frames : (long)rate * 60) + rate; /* +1s headroom */
    a->mono = xmalloc(sizeof(float) * (size_t)cap);
    a->n = 0;
    a->rate = rate;

    size_t block = mpg123_outblock(mh);
    if (block < 4096) block = 4096;
    unsigned char *buf = xmalloc(block);

    size_t done = 0;
    int rc;
    int ch = channels > 2 ? 2 : channels;

    while (a->n < max_frames) {
        rc = mpg123_read(mh, buf, block, &done);
        if (done == 0) break;
        if (rc != MPG123_OK && rc != MPG123_NEW_FORMAT && rc != MPG123_DONE) break;

        size_t nfr = done / (sizeof(short) * (size_t)channels);
        short *s = (short *)buf;
        for (size_t i = 0; i < nfr && a->n < max_frames; i++) {
            double v;
            if (ch == 1) v = s[i] / 32768.0;
            else         v = (s[2 * i] + s[2 * i + 1]) / 65536.0;
            a->mono[a->n++] = (float)v;
        }
        if (rc == MPG123_DONE) break;
    }

    free(buf);
    mpg123_close(mh);
    mpg123_delete(mh);

    if (a->n < rate / 2) {           /* less than half a second */
        snprintf(a->err, sizeof a->err, "no decodable audio");
        audio_free(a);
        return -1;
    }
    a->duration = dur_est > 0 ? dur_est : (double)a->n / rate;
    return 0;
}

/* ------------------------------------------------------------------ */
/* features                                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    int    ok;
    double bpm;
    double energy;      /* 0..1 */
    double centroid;    /* Hz */
    double flux;        /* onset envelope mean */
    double zcr;         /* 0..1 */
    double onset_rate;  /* detected onsets per second */
    double pulse;       /* beat/autocorrelation strength, 0..1 */
    double flatness;    /* spectral flatness, 0..1 */
    double dyn;         /* p95/p10 frame-energy ratio */
    double low_r;       /* share of energy below 250 Hz */
    double high_r;      /* share of energy above 4 kHz */
    double mode;        /* -1 minor .. +1 major */
    double key_conf;    /* tonal confidence of the chroma fit */
    double duration;
    char   err[160];
} Features;

/* Krumhansl-Kessler key profiles, used for major/minor detection */
static const double KEY_MAJOR[12] = {
    6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88
};
static const double KEY_MINOR[12] = {
    6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17
};

static double corr12(const double *a, const double *b)
{
    double ma = 0, mb = 0;
    for (int i = 0; i < 12; i++) { ma += a[i]; mb += b[i]; }
    ma /= 12.0; mb /= 12.0;
    double num = 0, da = 0, db = 0;
    for (int i = 0; i < 12; i++) {
        double x = a[i] - ma, y = b[i] - mb;
        num += x * y; da += x * x; db += y * y;
    }
    return (da > 1e-12 && db > 1e-12) ? num / sqrt(da * db) : 0.0;
}

/* robust percentile of an unsorted array (copies, so it must be writable) */
static double pctl(double *v, long n, double q)
{
    if (n <= 0) return 0.0;
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    long idx = (long)(q * (double)(n - 1));
    if (idx < 0) idx = 0;
    if (idx >= n) idx = n - 1;
    return v[idx];
}

static void analyse(const Audio *a, Features *f)
{
    memset(f, 0, sizeof *f);
    f->duration = a->duration;

    long nframes = a->n > FFT_N ? (a->n - FFT_N) / HOP + 1 : 0;
    if (nframes < 4) {
        snprintf(f->err, sizeof f->err, "too short");
        return;
    }

    int half = FFT_N / 2;
    double *re  = xmalloc(sizeof(double) * FFT_N);
    double *im  = xmalloc(sizeof(double) * FFT_N);
    double *prev = xmalloc(sizeof(double) * half);
    memset(prev, 0, sizeof(double) * half);

    double *onset = xmalloc(sizeof(double) * (size_t)nframes);
    double *frms  = xmalloc(sizeof(double) * (size_t)nframes);
    double  chroma[12] = {0};
    double  flat_sum = 0, low_sum = 0, high_sum = 0, all_sum = 0;

    /* log-spaced band edges, 40 Hz .. 16 kHz */
    double edges[BANDS + 1];
    int    ebin[BANDS + 1];
    double lo = 40.0, hi = fmin(16000.0, a->rate * 0.49);
    for (int i = 0; i <= BANDS; i++) {
        edges[i] = lo * pow(hi / lo, (double)i / BANDS);
        ebin[i] = (int)lround(edges[i] * FFT_N / a->rate);
        if (ebin[i] < 1) ebin[i] = 1;
        if (ebin[i] > half - 1) ebin[i] = half - 1;
        if (i && ebin[i] <= ebin[i - 1]) ebin[i] = ebin[i - 1] + 1;
    }

    double rms_sum = 0, cen_sum = 0, zc_sum = 0;
    long   cnt = 0;

    for (long fi = 0; fi < nframes; fi++) {
        long off = fi * HOP;
        double energy_sum = 0;
        int zc = 0;
        for (int i = 0; i < FFT_N; i++) {
            double x = a->mono[off + i];
            energy_sum += x * x;
            if (i && ((a->mono[off + i] < 0) != (a->mono[off + i - 1] < 0))) zc++;
            re[i] = x * g_win[i];
            im[i] = 0;
        }
        rms_sum += energy_sum / FFT_N;
        zc_sum  += (double)zc / FFT_N;
        frms[fi] = energy_sum / FFT_N;

        fft_run(re, im);

        double cnum = 0, cden = 0, flux = 0;
        double log_sum = 0, lin_sum = 0;
        int    fbins = 0;
        double nyq_lo = 80.0, nyq_hi = 8000.0;

        for (int k = 1; k < half; k++) {
            double p = re[k] * re[k] + im[k] * im[k];
            double m = sqrt(p);
            double hz = (double)k * a->rate / FFT_N;
            if (k >= ebin[0]) {
                cnum += hz * m;
                cden += m;
            }
            double d = m - prev[k];
            if (d > 0) flux += d;
            prev[k] = m;

            all_sum += p;
            if (hz < 250.0) low_sum += p;
            if (hz > 4000.0) high_sum += p;

            if (hz >= nyq_lo && hz <= nyq_hi) {
                log_sum += log(p + 1e-20);
                lin_sum += p;
                fbins++;
            }

            if (hz >= 55.0 && hz <= 2100.0) {
                double midi = 69.0 + 12.0 * log2(hz / 440.0);
                int pc = ((int)floor(midi + 0.5)) % 12;
                if (pc < 0) pc += 12;
                chroma[pc] += m;
            }
        }
        if (fbins > 0 && lin_sum > 1e-20) {
            double geo = exp(log_sum / fbins);
            flat_sum += geo / (lin_sum / fbins);
        }
        if (cden > 1e-12) cen_sum += cnum / cden;

        /* normalise flux by mean magnitude so loudness does not dominate */
        onset[cnt++] = flux / (cden / half + 1e-9);
    }

    double rms = sqrt(rms_sum / (double)cnt);
    double dbfs = 20.0 * log10(rms + 1e-9);      /* -30 dBFS .. 0 dBFS */
    f->energy   = clamp01((dbfs + 30.0) / 30.0);
    f->centroid = cen_sum / cnt;
    f->zcr      = zc_sum / cnt;
    f->flatness = clamp01(flat_sum / cnt);
    f->low_r    = all_sum > 1e-12 ? low_sum / all_sum : 0.0;
    f->high_r   = all_sum > 1e-12 ? high_sum / all_sum : 0.0;

    /* dynamic range: ratio of loud to quiet frames */
    double *tmp = xmalloc(sizeof(double) * (size_t)cnt);
    memcpy(tmp, frms, sizeof(double) * (size_t)cnt);
    double p10 = pctl(tmp, cnt, 0.10);
    memcpy(tmp, frms, sizeof(double) * (size_t)cnt);
    double p95 = pctl(tmp, cnt, 0.95);
    free(tmp);
    f->dyn = clampd((p95 + 1e-9) / (p10 + 1e-6), 1.0, 50.0);

    /* chroma profile -> best major and minor key fit */
    double csum = 0;
    for (int i = 0; i < 12; i++) csum += chroma[i];
    if (csum > 1e-9) {
        for (int i = 0; i < 12; i++) chroma[i] /= csum;
        double rot[12], bm = -2.0, bn = -2.0;
        for (int r = 0; r < 12; r++) {
            for (int i = 0; i < 12; i++) rot[i] = chroma[(i + r) % 12];
            double cm = corr12(rot, KEY_MAJOR);
            double cn = corr12(rot, KEY_MINOR);
            if (cm > bm) bm = cm;
            if (cn > bn) bn = cn;
        }
        f->mode     = clampd((bm - bn) * 2.5, -1.0, 1.0);
        f->key_conf = clamp01(fmax(bm, bn));
    }

    /* onset envelope: smooth then remove mean */
    double mean = 0;
    for (long i = 0; i < cnt; i++) mean += onset[i];
    mean /= cnt;
    for (long i = 0; i < cnt; i++) onset[i] = fmax(0.0, onset[i] - mean);

    double amax = 0, asum = 0;
    for (long i = 0; i < cnt; i++) { if (onset[i] > amax) amax = onset[i]; asum += onset[i]; }
    if (amax > 0) for (long i = 0; i < cnt; i++) onset[i] /= amax;

    f->flux = amax > 0 ? asum / cnt : 0.0;

    /* onset density: count local maxima above an adaptive threshold */
    double om = 0;
    for (long i = 0; i < cnt; i++) om += onset[i];
    om /= (double)cnt;
    double ov = 0;
    for (long i = 0; i < cnt; i++) { double d = onset[i] - om; ov += d * d; }
    ov = sqrt(ov / (double)cnt);
    double thr = om + 0.6 * ov;
    int peaks = 0;
    long last_peak = -100;
    for (long i = 1; i + 1 < cnt; i++) {
        if (onset[i] >= thr && onset[i] >= onset[i - 1] && onset[i] >= onset[i + 1]
            && i - last_peak >= 4) {
            peaks++;
            last_peak = i;
        }
    }
    f->onset_rate = (double)peaks / ((double)cnt / ((double)a->rate / HOP));

    /* ---- tempo: autocorrelation of onset envelope ---- */
    double fps = (double)a->rate / HOP;
    int min_lag = (int)floor(60.0 / BPM_HI * fps);
    int max_lag = (int)ceil(60.0 / BPM_LO * fps);
    if (min_lag < 2) min_lag = 2;
    if (max_lag > cnt / 2) max_lag = (int)(cnt / 2);

    double best = -1e30;
    int best_lag = 0;
    double best_lag_f = 0;
    f->pulse = 0.0;
    if (max_lag > min_lag + 1 && amax > 0) {
        double *ac = xmalloc(sizeof(double) * (size_t)(max_lag + 2));
        for (int lag = min_lag; lag <= max_lag; lag++) {
            double s = 0;
            for (long i = 0; i + lag < cnt; i++) s += onset[i] * onset[i + lag];
            ac[lag] = s / (double)(cnt - lag);
        }
        /* mild preference for 70..145 bpm to reduce octave errors */
        for (int lag = min_lag; lag <= max_lag; lag++) {
            double b = 60.0 * fps / lag;
            double w = (b >= 70.0 && b <= 145.0) ? 1.0 : 0.80;
            if (ac[lag] * w > best) { best = ac[lag] * w; best_lag = lag; best_lag_f = lag; }
        }
        double pwr = 0;
        for (long i = 0; i < cnt; i++) pwr += onset[i] * onset[i];
        pwr /= (double)cnt;
        if (best_lag && pwr > 1e-12)
            f->pulse = clamp01(ac[best_lag] / pwr);
        /* parabolic interpolation around the peak for sub-lag precision */
        if (best_lag > min_lag && best_lag < max_lag) {
            double a0 = ac[best_lag - 1], a1 = ac[best_lag], a2 = ac[best_lag + 1];
            double denom = a0 - 2.0 * a1 + a2;
            if (fabs(denom) > 1e-12) {
                double shift = 0.5 * (a0 - a2) / denom;   /* -0.5 .. 0.5 */
                if (fabs(shift) < 1.0) best_lag_f = best_lag + shift;
            }
        }
        free(ac);
    }
    f->bpm = (best_lag && best_lag_f > 0.5) ? 60.0 * fps / best_lag_f : 0.0;
    if (!isfinite(f->bpm) || f->bpm < 20.0 || f->bpm > 400.0) f->bpm = 0.0;

    free(re); free(im); free(prev); free(onset); free(frms);
    f->ok = 1;
}

/* ------------------------------------------------------------------ */
/* classification                                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    double x, y;
    int    mood;
} Verdict;

/* ---- absolute calibration ---------------------------------------- */
/*
 * Each feature is mapped from its physical range to 0..1 with fixed
 * anchors. Unlike quantile normalisation this does not force the library
 * to fill every mood folder: a library of quiet piano pieces ends up
 * mostly in Calm/Sad/Emotional, which is the honest answer.
 */
typedef struct {
    double bpm, energy, cent, onset, pulse, flat, zcr, dyn, low, high, mode;
} Cal;

static double curve(double x, double x0, double x1, double x2, double x3,
                    double y0, double y1, double y2, double y3)
{
    if (x <= x0) return y0;
    if (x >= x3) return y3;
    double xs[4] = { x0, x1, x2, x3 };
    double ys[4] = { y0, y1, y2, y3 };
    for (int i = 0; i < 3; i++) {
        if (x <= xs[i + 1]) {
            double t = (x - xs[i]) / (xs[i + 1] - xs[i]);
            return ys[i] + t * (ys[i + 1] - ys[i]);
        }
    }
    return y3;
}

static Cal calibrate(const Features *f)
{
    Cal c;
    memset(&c, 0, sizeof c);

    double bpm = (isfinite(f->bpm) && f->bpm > 0.0) ? f->bpm : 100.0;
    int guard = 0;
    while (bpm < BPM_LO && guard++ < 64) bpm *= 2.0;
    guard = 0;
    while (bpm > BPM_HI && guard++ < 64) bpm /= 2.0;
    c.bpm = curve(bpm, 60, 80, 115, 160, 0.05, 0.22, 0.55, 1.00);

    c.energy = curve(f->energy, 0.12, 0.30, 0.55, 0.80, 0.00, 0.30, 0.70, 1.00);

    double cent = f->centroid > 1.0 ? f->centroid : 1.0;
    c.cent = curve(log(cent), log(350), log(900), log(2200), log(5500),
                   0.03, 0.22, 0.55, 1.00);

    c.onset = curve(f->onset_rate, 0.4, 1.2, 2.5, 5.0, 0.03, 0.25, 0.55, 1.00);
    c.pulse = curve(f->pulse, 0.03, 0.10, 0.24, 0.45, 0.00, 0.25, 0.65, 1.00);
    c.flat  = curve(f->flatness, 0.0004, 0.003, 0.012, 0.035, 0.00, 0.30, 0.70, 1.00);
    c.zcr   = curve(f->zcr, 0.025, 0.05, 0.10, 0.20, 0.00, 0.25, 0.60, 1.00);
    c.dyn   = curve(f->dyn, 1.5, 4.0, 9.0, 20.0, 0.00, 0.35, 0.70, 1.00);
    c.low   = curve(f->low_r, 0.04, 0.15, 0.30, 0.55, 0.00, 0.30, 0.65, 1.00);
    c.high  = curve(f->high_r, 0.010, 0.040, 0.10, 0.22, 0.00, 0.30, 0.65, 1.00);

    double conf = clamp01((f->key_conf - 0.25) / 0.55);
    c.mode = 0.5 + 0.5 * clampd(f->mode, -1.0, 1.0) * conf;
    return c;
}

static int nearest_mood(double x, double y)
{
    int best = 0;
    double bd = 1e30;
    for (int m = 0; m < NMOODS; m++) {
        double dx = x - MOODS[m].x;
        double dy = y - MOODS[m].y;
        double d = dx * dx + 0.85 * dy * dy;
        if (d < bd) { bd = d; best = m; }
    }
    return best;
}

/*
 * x: -1 sad/dark .. +1 bright/happy, y: -1 calm .. +1 intense.
 * Nearest centre in absolute coordinates, then a few guards so that the
 * intense/dark folders require actual intensity and percussion.
 */
static int mood_from_plane(double x, double y, const Cal *c, const Features *f)
{
    enum { M_HAPPY, M_ENERGETIC, M_CALM, M_SAD, M_DARK, M_EMOTIONAL, M_BRIGHT };

    /* Quiet acoustic / solo-instrument material: little high-frequency
     * energy, low noisiness, mid timbre. For these the 2D distance is
     * unreliable (arpeggios look "busy"), so decide by brightness/mode:
     * dark -> Sad, bright -> Calm, busy -> Bright. */
    int acoustic = (f->high_r < 0.05 && f->zcr < 0.09 && f->centroid < 3400.0);
    if (acoustic && c->energy < 0.70) {
        if (y > 0.40) return (x >= 0.05) ? M_BRIGHT : M_EMOTIONAL;
        if (x < -0.10) return M_SAD;
        return M_CALM;
    }

    int m = nearest_mood(x, y);

    if (m == M_DARK) {
        if (!(y >= 0.22 && (f->energy >= 0.55 || c->flat >= 0.35)))
            m = (y < -0.05) ? M_SAD : M_EMOTIONAL;
    }
    if ((m == M_ENERGETIC || m == M_HAPPY) && y < 0.08)
        m = (x >= 0.30) ? M_BRIGHT : (x <= -0.20 ? M_SAD : M_EMOTIONAL);
    if ((m == M_CALM || m == M_SAD) && y > 0.12)
        m = (x <= -0.20) ? M_EMOTIONAL : M_BRIGHT;
    /* mid-tempo material with little brightness is a ballad, not a
     * dance/rock track: avoid dumping every pop song into Energetic */
    if (m == M_ENERGETIC && f->bpm > 0 && f->bpm < 110.0 && x < 0.30 &&
        f->onset_rate < 5.0 && f->flatness < 0.05)
        m = M_EMOTIONAL;
    return m;
}

static void classify_all(const Features *feat, int n, Verdict *verd)
{
    for (int i = 0; i < n; i++) {
        if (!feat[i].ok) { verd[i].mood = -1; verd[i].x = verd[i].y = 0; continue; }
        Cal c = calibrate(&feat[i]);

        /* intensity: loudness, tempo, beat strength, onset density */
        double intensity = 0.34 * c.energy + 0.16 * c.bpm + 0.16 * c.pulse +
                           0.16 * c.onset  + 0.10 * c.flat + 0.08 * c.low;
        /* valence: mode (major/minor) is the strongest cue, then brightness */
        double valence   = 0.26 * c.mode   + 0.22 * c.cent + 0.10 * c.bpm +
                           0.12 * c.zcr    + 0.12 * c.high + 0.10 * c.flat +
                           0.08 * c.pulse;

        double x = 2.0 * valence - 1.0;
        double y = 2.0 * intensity - 1.0;

        verd[i].x = x;
        verd[i].y = y;
        verd[i].mood = mood_from_plane(x, y, &c, &feat[i]);
    }
}

/* ------------------------------------------------------------------ */
/* progress display                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    int   enabled;
    int   total, done;
    char  last[64];
    clock_t t0;
} Progress;

static int utf8_display_width(const char *s, int max_bytes)
{
    int w = 0, i = 0;
    while (s[i] && i < max_bytes) {
        unsigned char c = (unsigned char)s[i];
        int len = 1, cp = c;
        if (c >= 0xF0) { len = 4; cp = c & 0x07; }
        else if (c >= 0xE0) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xC0) { len = 2; cp = c & 0x1F; }
        for (int k = 1; k < len && s[i + k]; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        /* CJK, kana, fullwidth forms occupy two columns */
        int wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                   (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                   (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                   (cp >= 0xFFE0 && cp <= 0xFFE6);
        w += wide ? 2 : 1;
        i += len;
    }
    return w;
}

/* truncate s so its display width fits within `cols`, appending ".." */
static void truncate_display(char *out, size_t outsz, const char *s, int cols)
{
    if (cols <= 0) { out[0] = 0; return; }
    int w = 0, i = 0;
    size_t o = 0;
    while (s[i] && o + 1 < outsz) {
        unsigned char c = (unsigned char)s[i];
        int len = 1, cp = c;
        if (c >= 0xF0) { len = 4; cp = c & 0x07; }
        else if (c >= 0xE0) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xC0) { len = 2; cp = c & 0x1F; }
        for (int k = 1; k < len && s[i + k]; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
        int wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                   (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                   (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                   (cp >= 0xFFE0 && cp <= 0xFFE6);
        int cw = wide ? 2 : 1;
        if (w + cw > cols - 2) break;      /* leave room for ".." */
        for (int k = 0; k < len && s[i + k] && o + 1 < outsz; k++) out[o++] = s[i + k];
        w += cw;
        i += len;
    }
    out[o] = 0;
    if (s[i]) {
        if (o + 2 < outsz) { out[o++] = '.'; out[o++] = '.'; out[o] = 0; }
    }
}

static void progress_render(Progress *p, const char *name, const char *tag)
{
    if (!p->enabled) return;
    double frac = p->total ? (double)p->done / p->total : 0.0;
    int width = 24;
    int filled = (int)lround(frac * width);
    char bar[64];
    int o = 0;
    for (int i = 0; i < width; i++) bar[o++] = i < filled ? '#' : '.';
    bar[o] = 0;

    int cols = 100;
    const char *cs = getenv("COLUMNS");
    if (cs && atoi(cs) > 20) cols = atoi(cs);

    char shown[256];
    truncate_display(shown, sizeof shown, name ? name : "", 34);

    double el = (double)(clock() - p->t0) / CLOCKS_PER_SEC;
    double eta = p->done > 0 ? el / p->done * (p->total - p->done) : 0.0;

    char tagbuf[32];
    snprintf(tagbuf, sizeof tagbuf, "%s", tag ? tag : "");

    /* build once, then pad to the terminal width so old text is erased */
    char line[512];
    snprintf(line, sizeof line,
             "%s[%s]%s %3d/%3d %3.0f%%  %s  %-12.12s %s%3.0fs",
             C_TITLE, bar, C_RESET, p->done, p->total, frac * 100.0,
             shown, tagbuf, C_DIM, eta);
    int vis = utf8_display_width(line, (int)sizeof line);
    int pad = cols - vis - 2;
    if (pad < 0) pad = 0;
    fprintf(stderr, "\r\033[K%s%*s%s", line, pad, "", C_RESET);
    fflush(stderr);
}

static void progress_clear(void)
{
    if (isatty(STDERR_FILENO)) { fprintf(stderr, "\r\033[K"); fflush(stderr); }
}

/* ------------------------------------------------------------------ */
/* copy                                                               */
/* ------------------------------------------------------------------ */
static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[1 << 16];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) unlink(dst);
    return ok ? 0 : -1;
}

static char *unique_dest(const char *dir, const char *filename)
{
    size_t n = strlen(dir) + strlen(filename) + 32;
    char *cand = xmalloc(n);
    snprintf(cand, n, "%s/%s", dir, filename);
    if (!path_exists(cand)) return cand;

    const char *dot = strrchr(filename, '.');
    size_t stem = dot ? (size_t)(dot - filename) : strlen(filename);
    for (int i = 2; i < 10000; i++) {
        snprintf(cand, n, "%s/%.*s (%d)%s", dir, (int)stem, filename,
                 i, dot ? dot : "");
        if (!path_exists(cand)) return cand;
    }
    free(cand);
    return NULL;
}

/* copy a sibling .lrc / .LRC lyric file if present */
static void copy_sidecar(const char *src_mp3, const char *dst_mp3)
{
    const char *dot = strrchr(src_mp3, '.');
    if (!dot) return;
    size_t stem = (size_t)(dot - src_mp3);
    char *lrc = xmalloc(stem + 8);
    snprintf(lrc, stem + 8, "%.*s.lrc", (int)stem, src_mp3);
    if (!path_exists(lrc)) { free(lrc); return; }

    const char *dd = strrchr(dst_mp3, '.');
    size_t dstem = dd ? (size_t)(dd - dst_mp3) : strlen(dst_mp3);
    char *dlrc = xmalloc(dstem + 8);
    snprintf(dlrc, dstem + 8, "%.*s.lrc", (int)dstem, dst_mp3);
    if (!path_exists(dlrc)) copy_file(lrc, dlrc);
    free(lrc);
    free(dlrc);
}

/* move a file, falling back to copy+unlink across filesystems */
static int move_file(const char *src, const char *dst)
{
    if (!strcmp(src, dst)) return 0;
    if (rename(src, dst) == 0) return 0;
    if (errno == EXDEV) {
        if (copy_file(src, dst) == 0) {
            if (unlink(src) == 0) return 0;
            unlink(dst);
        }
    }
    return -1;
}

static void move_sidecar(const char *src_mp3, const char *dst_mp3)
{
    const char *dot = strrchr(src_mp3, '.');
    if (!dot) return;
    size_t stem = (size_t)(dot - src_mp3);
    char *lrc = xmalloc(stem + 8);
    snprintf(lrc, stem + 8, "%.*s.lrc", (int)stem, src_mp3);
    if (!path_exists(lrc)) { free(lrc); return; }

    const char *dd = strrchr(dst_mp3, '.');
    size_t dstem = dd ? (size_t)(dd - dst_mp3) : strlen(dst_mp3);
    char *dlrc = xmalloc(dstem + 8);
    snprintf(dlrc, dstem + 8, "%.*s.lrc", (int)dstem, dst_mp3);
    if (!path_exists(dlrc)) move_file(lrc, dlrc);
    free(lrc);
    free(dlrc);
}

/* ------------------------------------------------------------------ */
/* manual overrides                                                   */
/* ------------------------------------------------------------------ */
typedef struct {
    char *pat;
    int   mood;
} Override;

static int mood_index(const char *s)
{
    if (!s || !*s) return -1;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end && end != s && v >= 1 && v <= NMOODS &&
        (*end == 0 || *end == '_' || *end == ' ' || *end == '.'))
        return (int)v - 1;
    for (int i = 0; i < NMOODS; i++)
        if (!strcasecmp(s, MOODS[i].folder)) return i;
    for (int i = 0; i < NMOODS; i++)
        if (strcasestr(MOODS[i].folder, s)) return i;
    return -1;
}

/*
 * Overrides file: one rule per line, `pattern<TAB>mood`.
 * pattern is a case-insensitive substring of the relative path, mood is a
 * folder name (03_Calm), a number (3) or part of the name (Calm).
 * Lines starting with '#' are comments.
 */
static int load_overrides(const char *path, Override **out)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;

    Override *v = NULL;
    int n = 0, cap = 0;
    char line[1024];
    while (fgets(line, sizeof line, fp)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        size_t len = strlen(s);
        while (len && (s[len - 1] == '\n' || s[len - 1] == '\r' ||
                       s[len - 1] == ' '  || s[len - 1] == '\t'))
            s[--len] = 0;
        if (!*s || *s == '#') continue;

        char *sep = strchr(s, '\t');
        if (!sep) sep = strchr(s, '|');
        if (!sep) {
            fprintf(stderr, "override: ignoring line without tab: %s\n", s);
            continue;
        }
        *sep = 0;
        char *a = s, *b = sep + 1;
        int ma = mood_index(a), mb = mood_index(b);
        char *pat;
        int mood;
        if (ma >= 0 && mb < 0) { mood = ma; pat = b; }
        else if (mb >= 0)      { mood = mb; pat = a; }
        else {
            fprintf(stderr, "override: unknown mood '%s'\n", b);
            continue;
        }
        if (!*pat) continue;
        if (n == cap) { cap = cap ? cap * 2 : 16; v = xrealloc(v, sizeof *v * (size_t)cap); }
        v[n].pat = xstrdup(pat);
        v[n].mood = mood;
        n++;
    }
    fclose(fp);
    *out = v;
    return n;
}

static void free_overrides(Override *v, int n)
{
    for (int i = 0; i < n; i++) free(v[i].pat);
    free(v);
}

/* ------------------------------------------------------------------ */
/* misc output                                                        */
/* ------------------------------------------------------------------ */
static void human_dur(char *buf, size_t n, double s)
{
    if (s < 60.0) snprintf(buf, n, "%.0fs", s);
    else if (s < 3600.0) snprintf(buf, n, "%d:%02d", (int)(s / 60), (int)fmod(s, 60));
    else snprintf(buf, n, "%dh%02dm", (int)(s / 3600), (int)fmod(s / 60, 60));
}

/* one prompt line; returns -1 on EOF or when the user types q */
static int ask_line(const char *label, const char *def, char *buf, size_t n)
{
    printf("  %s%s%s [%s]: ", C_BOLD, label, C_RESET, def);
    fflush(stdout);
    if (!fgets(buf, (int)n, stdin)) { printf("\n"); return -1; }
    clean_path(buf);
    if (!strcasecmp(buf, "q") || !strcasecmp(buf, "quit")) return -1;
    if (!buf[0]) snprintf(buf, n, "%s", def);
    return 0;
}

/* Interactive wizard shown when sensme is started with no arguments.
 * Fills src/out and returns 0 to run, 1 if the user cancelled. */
static int interactive_setup(char *src, size_t srclen,
                             char *out, size_t outlen,
                             const char *def_src)
{
    char line[PATH_MAX], tmp[PATH_MAX], sug[PATH_MAX];

    printf("\n%sSensMe %s%s — 把 mp3 按情绪整理到 7 个文件夹\n",
           C_TITLE, SENSME_VERSION, C_RESET);
    printf("%s只读取原曲，不会修改或删除文件；同名 .lrc 会一起复制。\n%s\n",
           C_DIM, C_RESET);
    printf("  路径可以直接粘贴（支持 ~ 和带引号的路径），"
           "直接回车用中括号里的默认值，输入 q 退出。\n\n");

    for (;;) {
        if (ask_line("mp3 在哪个目录 / source folder", def_src, line, sizeof line))
            return 1;
        expand_tilde(line, tmp, sizeof tmp);
        if (!is_dir(tmp)) {
            printf("  %s目录不存在或不是文件夹：%s%s\n\n", C_ERR, tmp, C_RESET);
            continue;
        }
        snprintf(src, srclen, "%s", tmp);
        break;
    }

    snprintf(sug, sizeof sug, "%s/Music-SensMe", src);
    for (;;) {
        if (ask_line("整理后放到哪里 / output folder", sug, line, sizeof line))
            return 1;
        expand_tilde(line, tmp, sizeof tmp);
        if (!strcmp(tmp, src)) {
            printf("  %s输出目录不能和源目录相同%s\n\n", C_ERR, C_RESET);
            continue;
        }
        if (path_exists(tmp) && !is_dir(tmp)) {
            printf("  %s这里是个文件，不是文件夹：%s%s\n\n", C_ERR, tmp, C_RESET);
            continue;
        }
        snprintf(out, outlen, "%s", tmp);
        break;
    }

    StrList probe = {0};
    scan_dir(src, &probe);
    printf("\n  源目录 : %s\n  输出到 : %s\n  找到 %d 首 mp3\n\n",
           src, out, probe.n);
    sl_free(&probe);

    for (;;) {
        char yn[16];
        if (ask_line("开始分析 / start?", "Y", yn, sizeof yn)) return 1;
        if (!strcasecmp(yn, "y") || !strcasecmp(yn, "yes")) return 0;
        if (!strcasecmp(yn, "n") || !strcasecmp(yn, "no")) return 1;
        printf("  请输入 y 或 n\n");
    }
}

static void usage(const char *argv0)
{
    printf("sensme %s - analyse mp3 files, classify them by mood and copy each\n"
           "one into a mood-named folder. Source files are never modified.\n\n",
           SENSME_VERSION);
    printf("usage: %s [options]\n\n", argv0);
    printf("Run it with no options in a terminal and it will ask for the source\n"
           "and output folders.\n\n");
    printf("options:\n"
           "  -s, --src DIR     source directory     (default: ~/Music)\n"
           "  -o, --out DIR     output directory     (default: ~/Music/Music-SensMe)\n"
           "  -n, --dry-run     analyse only, copy nothing\n"
           "  -q, --quiet       hide per-file output\n"
           "  -f, --force       re-copy files that already exist in output\n"
           "  -p, --include-out also scan inside the output directory\n"
           "  -r, --reorganize DIR\n"
           "                    re-sort an existing output tree in place (no copy)\n"
           "  -O, --override FILE\n"
           "                    extra pattern -> mood rules (default:\n"
           "                    ~/.config/sensme/overrides.tsv if it exists)\n"
           "  -V, --version     show version\n"
           "  -h, --help        show this help\n\n");
    printf("override file: one rule per line, \"pattern<TAB>mood\".  pattern is a\n"
           "case-insensitive substring of the relative path, mood is a folder\n"
           "name, a number 1..7 or part of the name, e.g.\n"
           "  Flower Dance\t03_Calm\n"
           "  米津玄師\t06_Emotional\n\n");
    printf("mood folders:\n");
    for (int i = 0; i < NMOODS; i++)
        printf("  %-20s %s\n", MOODS[i].folder, MOODS[i].label);
    printf("\nNO_COLOR=1 disables the coloured output.\n");
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */
static int run(int argc, char **argv)
{
    const char *src = NULL, *out = NULL, *ov_path = NULL;
    int dry_run = 0, quiet = 0, force = 0, include_out = 0, dump = 0, reorg = 0;
    char isrc[PATH_MAX] = "", iout[PATH_MAX] = "";

    /* Termux on Android 10+ runs binaries via /system/bin/linker64 and
       repeats the executable path as the first argument. */
    int first = 1;
    while (first < argc && !strcmp(argv[first], argv[0])) first++;

    for (int i = first; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else if (!strcmp(a, "-V") || !strcmp(a, "--version")) {
            printf("sensme %s\n", SENSME_VERSION);
            return 0;
        }
        else if (!strcmp(a, "--dump")) dump = 1;
        else if ((!strcmp(a, "-s") || !strcmp(a, "--src")) && i + 1 < argc) src = argv[++i];
        else if ((!strcmp(a, "-o") || !strcmp(a, "--out")) && i + 1 < argc) out = argv[++i];
        else if ((!strcmp(a, "-r") || !strcmp(a, "--reorganize")) && i + 1 < argc) {
            reorg = 1;
            out = argv[++i];
        }
        else if ((!strcmp(a, "-O") || !strcmp(a, "--override")) && i + 1 < argc) ov_path = argv[++i];
        else if (!strcmp(a, "-n") || !strcmp(a, "--dry-run")) dry_run = 1;
        else if (!strcmp(a, "-q") || !strcmp(a, "--quiet")) quiet = 1;
        else if (!strcmp(a, "-f") || !strcmp(a, "--force")) force = 1;
        else if (!strcmp(a, "-p") || !strcmp(a, "--include-out")) include_out = 1;
        else {
            fprintf(stderr, "unknown option: %s\n\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    color_init();

    char def_src[PATH_MAX], def_out[PATH_MAX];
    const char *home = home_dir();
    if (!home) { fprintf(stderr, "HOME not set\n"); return 1; }
    snprintf(def_src, sizeof def_src, "%s/Music", home);
    snprintf(def_out, sizeof def_out, "%s/Music/Music-SensMe", home);
    if (reorg) {
        /* re-sort an existing output tree in place */
        src = out;
    }

    /* no arguments on a terminal: ask for the folders instead of failing */
    if (first == argc && isatty(STDIN_FILENO)) {
        if (!is_dir(def_src)) snprintf(def_src, sizeof def_src, "%s", home);
        if (interactive_setup(isrc, sizeof isrc, iout, sizeof iout, def_src))
            return 0;
        src = isrc;
        out = iout;
    }
    if (!src) src = def_src;
    if (!out) out = def_out;

    /* ---- manual overrides ---- */
    Override *ovs = NULL;
    int nov = 0;
    if (!ov_path) {
        static char def_ov[PATH_MAX];
        snprintf(def_ov, sizeof def_ov, "%s/.config/sensme/overrides.tsv", home);
        if (path_exists(def_ov)) ov_path = def_ov;
    }
    if (ov_path) {
        nov = load_overrides(ov_path, &ovs);
        if (!quiet)
            printf("  overrides: %d rule%s from %s\n", nov, nov == 1 ? "" : "s", ov_path);
    }

    struct stat st;
    if (stat(src, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "%serror:%s source directory not found: %s\n", C_ERR, C_RESET, src);
        return 1;
    }

    char src_abs[PATH_MAX], out_abs[PATH_MAX];
    if (!realpath(src, src_abs)) { fprintf(stderr, "realpath: %s\n", strerror(errno)); return 1; }
    if (realpath(out, out_abs)) {
        /* exists */
    } else {
        snprintf(out_abs, sizeof out_abs, "%s", out);
    }

    printf("%sSensMe %s%s - audio mood classifier\n", C_TITLE, SENSME_VERSION, C_RESET);
    printf("  source : %s\n", src_abs);
    printf("  output : %s%s\n", out_abs, dry_run ? "  [dry run]" : "");

    /* ---- collect ---- */
    StrList files = {0};
    scan_dir(src_abs, &files);
    qsort(files.v, files.n, sizeof(char *), cmp_str);

    size_t outlen = strlen(out_abs);
    int excluded = 0;
    if (!include_out && !reorg) {
        StrList keep = {0};
        for (int i = 0; i < files.n; i++) {
            const char *p = files.v[i];
            if (!strncmp(p, out_abs, outlen) && (p[outlen] == '/' || p[outlen] == 0)) {
                excluded++;
                free(files.v[i]);
            } else {
                sl_push(&keep, files.v[i]);
            }
        }
        free(files.v);
        files = keep;
    }

    if (files.n == 0) {
        printf("\n%sno .mp3 files found%s\n", C_WARN, C_RESET);
        sl_free(&files);
        return 1;
    }
    printf("  files  : %d mp3%s\n\n", files.n,
           excluded ? "  (own output excluded)" : "");
    fflush(stdout);

    /* ---- analyse ---- */
    mpg123_init();
    fft_init(FFT_N);

    int n = files.n;
    Features *feat = xmalloc(sizeof(Features) * (size_t)n);
    Verdict  *verd = xmalloc(sizeof(Verdict) * (size_t)n);
    char    **rel  = xmalloc(sizeof(char *) * (size_t)n);
    int       bad  = 0;

    Progress pr = {0};
    pr.enabled = !quiet && isatty(STDERR_FILENO);
    pr.total = n;
    pr.t0 = clock();

    clock_t c0 = clock();
    double prev_ms = 0;
    for (int i = 0; i < n; i++) {
        const char *path = files.v[i];
        const char *r = path + strlen(src_abs);
        if (*r == '/') r++;
        rel[i] = xstrdup(r);

        size_t plen = strlen(path);
        char shortname[64];
        snprintf(shortname, sizeof shortname, "%s", base_name(path));
        (void)plen;

        if (!quiet) progress_render(&pr, shortname, "analysing");

        Audio au;
        if (decode_audio(path, MAX_SECONDS, &au) != 0) {
            memset(&feat[i], 0, sizeof feat[i]);
            snprintf(feat[i].err, sizeof feat[i].err, "%s", au.err);
            bad++;
        } else {
            analyse(&au, &feat[i]);
            if (!feat[i].ok) bad++;
            audio_free(&au);
        }

        pr.done = i + 1;
        if (!quiet) progress_render(&pr, shortname, "analysed");
        else if (isatty(STDERR_FILENO)) {
            /* quiet mode still shows a lightweight counter so the terminal
             * never looks frozen */
            fprintf(stderr, "\r\033[K  analysing: %d/%d", i + 1, n);
            fflush(stderr);
        }
        if (getenv("SENSME_TRACE")) {
            clock_t now = clock();
            double per = (double)(now - c0) * 1000.0 / CLOCKS_PER_SEC;
            fprintf(stderr, "\nTRACE %d/%d %.0fms total, %.0fms this, %.1fbpm %s\n",
                    i + 1, n, per, per - prev_ms, feat[i].bpm, shortname);
            fflush(stderr);
            prev_ms = per;
        }
    }

    if (isatty(STDERR_FILENO)) {
        fprintf(stderr, "\r\033[K  analysing: %d/%d  done\n", n, n);
        fprintf(stderr, "  classifying ...");
        fflush(stderr);
    }

    /* classify once all features are known (needs the whole batch) */
    classify_all(feat, n, verd);

    int ov_applied = 0;
    if (nov) {
        for (int i = 0; i < n; i++) {
            if (verd[i].mood < 0) continue;
            for (int j = 0; j < nov; j++) {
                if (strcasestr(rel[i], ovs[j].pat)) {
                    verd[i].mood = ovs[j].mood;
                    ov_applied++;
                    break;
                }
            }
        }
    }
    progress_clear();

    double elapsed = (double)(clock() - c0) / CLOCKS_PER_SEC;

    if (dump) {
        for (int i = 0; i < n; i++) {
            if (verd[i].mood < 0) {
                printf("ERR\t%s\t%s\n", rel[i], feat[i].err);
                continue;
            }
            printf("%.1f\t%.4f\t%.1f\t%.4f\t%.4f\t%.3f\t%.3f\t%d\t"
                   "%.2f\t%.2f\t%.2f\t%.2f\t%.4f\t%.1f\t%.3f\t%.3f\t%s\n",
                   feat[i].bpm, feat[i].energy, feat[i].centroid,
                   feat[i].flux, feat[i].zcr, verd[i].x, verd[i].y,
                   verd[i].mood, feat[i].mode, feat[i].key_conf,
                   feat[i].onset_rate, feat[i].pulse, feat[i].flatness,
                   feat[i].dyn, feat[i].low_r, feat[i].high_r, rel[i]);
        }
        fflush(stdout);
    }

    /* ---- copy ---- */
    int copied = 0, failed = 0, existing = 0;
    if (!dry_run) {
        if (mkdir_p(out_abs) != 0) {
            fprintf(stderr, "%serror:%s cannot create %s: %s\n",
                    C_ERR, C_RESET, out_abs, strerror(errno));
            return 1;
        }
        printf("%s%s%s into mood folders\n", C_TITLE,
               reorg ? "Moving" : "Copying", C_RESET);
        Progress cp = {0};
        cp.enabled = !quiet && isatty(STDERR_FILENO);
        cp.total = n;
        cp.t0 = clock();

        for (int i = 0; i < n; i++) {
            if (verd[i].mood < 0) {
                cp.done = i + 1;
                if (!quiet) progress_render(&cp, base_name(files.v[i]), "skipped");
                continue;
            }
            const char *dir_rel = MOODS[verd[i].mood].folder;
            char dir[PATH_MAX];
            snprintf(dir, sizeof dir, "%s/%s", out_abs, dir_rel);
            if (mkdir_p(dir) != 0) { failed++; continue; }

            char exact[PATH_MAX + 512];
            snprintf(exact, sizeof exact, "%s/%s", dir, base_name(files.v[i]));

            int rc = -2;
            int acted = 1;
            char *dst = NULL;
            if (reorg) {
                /* already in the right folder? nothing to do */
                if (!strcmp(files.v[i], exact)) {
                    existing++;
                    acted = 0;
                    rc = 0;
                } else {
                    dst = unique_dest(dir, base_name(files.v[i]));
                    if (!dst) { failed++; continue; }
                    rc = move_file(files.v[i], dst);
                    if (rc == 0) move_sidecar(files.v[i], dst);
                }
            } else {
                dst = unique_dest(dir, base_name(files.v[i]));
                if (!dst) { failed++; continue; }
                /* skip when the exact same file is already there */
                if (!force && path_exists(exact)) {
                    existing++;
                    rc = 0;
                    free(dst);
                    dst = NULL;
                } else {
                    rc = copy_file(files.v[i], dst);
                    if (rc == 0) copy_sidecar(files.v[i], dst);
                }
            }

            if (rc == 0) {
                if (acted) copied++;
            } else {
                failed++;
                fprintf(stderr, "\n%s%s failed:%s %s\n", C_ERR,
                        reorg ? "move" : "copy", C_RESET, files.v[i]);
            }
            free(dst);

            cp.done = i + 1;
            if (!quiet)
                progress_render(&cp, base_name(files.v[i]),
                                MOODS[verd[i].mood].label);
        }
        progress_clear();
        printf("\n");

        /* drop mood folders that are empty after a reorganisation */
        if (reorg) {
            for (int m = 0; m < NMOODS; m++) {
                char d[PATH_MAX];
                snprintf(d, sizeof d, "%s/%s", out_abs, MOODS[m].folder);
                rmdir(d);
            }
        }
    }

    /* ---- summary table ---- */
    int    mcount[NMOODS] = {0};
    double mbpm[NMOODS] = {0}, mlen[NMOODS] = {0};
    double all_len = 0;
    int mbpm_n[NMOODS] = {0};
    for (int i = 0; i < n; i++) {
        if (verd[i].mood < 0) continue;
        int m = verd[i].mood;
        mcount[m]++;
        if (isfinite(feat[i].bpm) && feat[i].bpm > 0) {
            mbpm[m] += feat[i].bpm;
            mbpm_n[m]++;
        }
        mlen[m] += feat[i].duration;
        all_len += feat[i].duration;
    }

    printf("%sMood summary%s\n", C_TITLE, C_RESET);
    printf("  %-2s %-20s %-19s %5s %6s %7s\n",
           "#", "folder", "mood", "files", "bpm", "length");
    printf("  %s\n", "--------------------------------------------------------------");
    for (int m = 0; m < NMOODS; m++) {
        if (!mcount[m]) continue;
        char len[32];
        human_dur(len, sizeof len, mlen[m]);
        printf("  %-2d %-20s %-19s %5d %6.0f %7s\n",
               m + 1, MOODS[m].folder, MOODS[m].label, mcount[m],
               mbpm_n[m] ? mbpm[m] / mbpm_n[m] : 0.0, len);
    }
    printf("  %s\n", "--------------------------------------------------------------");
    char tlen[32];
    human_dur(tlen, sizeof tlen, all_len);
    printf("  %-2s %-20s %-19s %5d %6s %7s\n", "", "TOTAL", "",
           n - bad, "", tlen);

    if (!dry_run) {
        printf("\n  %-8s : %d\n", reorg ? "moved" : "copied", copied);
        if (existing) printf("  unchanged: %d (already present)\n", existing);
        if (failed)   printf("  %sfailed   : %d%s\n", C_ERR, failed, C_RESET);
        printf("  output   : %s\n", out_abs);
    }
    if (ov_applied) printf("  overrides: %d applied\n", ov_applied);
    if (bad) printf("  %sskipped  : %d (could not decode)%s\n", C_WARN, bad, C_RESET);
    char el[32];
    human_dur(el, sizeof el, elapsed);
    printf("  analysis : %s\n", el);

    /* ---- per-file listing ---- */
    if (!quiet) {
        printf("\n%sClassifications%s\n", C_TITLE, C_RESET);
        for (int m = 0; m < NMOODS; m++) {
            if (!mcount[m]) continue;
            printf("\n  %s%s%s  %s(%d files)%s\n", C_BOLD, MOODS[m].folder,
                   C_RESET, C_DIM, mcount[m], C_RESET);
            for (int i = 0; i < n; i++) {
                if (verd[i].mood != m) continue;
                printf("    %5.1f bpm  %5.1f%%  %-58.58s\n",
                       feat[i].bpm, feat[i].energy * 100.0, rel[i]);
            }
        }
        if (bad) {
            printf("\n  %sskipped%s\n", C_BOLD, C_RESET);
            for (int i = 0; i < n; i++)
                if (verd[i].mood < 0)
                    printf("    %-58.58s  %s\n", rel[i], feat[i].err);
        }
    }

    for (int i = 0; i < n; i++) free(rel[i]);
    free(rel);
    free_overrides(ovs, nov);
    free(feat);
    free(verd);
    sl_free(&files);
    fft_free();
    mpg123_exit();
    return 0;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    int rc = run(argc, argv);

    /* keep the window open when the exe is double-clicked */
    if (argc == 1 && isatty(STDIN_FILENO)) {
        printf("\n按回车退出 / Press Enter to exit...");
        fflush(stdout);
        int c;
        while ((c = getchar()) != '\n' && c != EOF) {}
    }
    return rc;
}
