/*
 * pendule — cartes du chaos d'un double pendule.
 *
 * Chaque pixel est un double pendule lâché sans vitesse depuis les angles
 * (θ1, θ2) : θ1 en abscisse, θ2 en ordonnée, de -π à π. On intègre les
 * équations du mouvement (Runge-Kutta d'ordre 4) et on colore le pixel selon :
 *
 *   bascule  : le temps avant qu'un des deux bras passe par-dessus (tour complet)
 *   lyapunov : la vitesse à laquelle deux pendules presque identiques divergent
 *   etat     : les angles atteints au temps T (avec --video : un film du chaos)
 *
 * Unités : m1 = m2 = l1 = l2 = g = 1, temps en √(l/g).
 * Calcul réparti sur tous les cœurs (pthreads). PNG écrit à la main.
 */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct { double t1, t2, w1, w2; } Etat;

/* dérivées du double pendule (masses, longueurs et g égaux à 1) */
static inline Etat derivee(Etat s) {
    double d = s.t1 - s.t2, sd = sin(d), cd = cos(d);
    double den = 3 - cos(2 * d);
    Etat r;
    r.t1 = s.w1;
    r.t2 = s.w2;
    r.w1 = (-3 * sin(s.t1) - sin(s.t1 - 2 * s.t2) - 2 * sd * (s.w2 * s.w2 + s.w1 * s.w1 * cd)) / den;
    r.w2 = (2 * sd * (2 * s.w1 * s.w1 + 2 * cos(s.t1) + s.w2 * s.w2 * cd)) / den;
    return r;
}

static inline Etat rk4(Etat s, double h) {
#define COMB(a, b, k) (Etat){a.t1 + (k) * b.t1, a.t2 + (k) * b.t2, a.w1 + (k) * b.w1, a.w2 + (k) * b.w2}
    Etat k1 = derivee(s);
    Etat k2 = derivee(COMB(s, k1, h / 2));
    Etat k3 = derivee(COMB(s, k2, h / 2));
    Etat k4 = derivee(COMB(s, k3, h));
    return (Etat){s.t1 + h / 6 * (k1.t1 + 2 * k2.t1 + 2 * k3.t1 + k4.t1),
                  s.t2 + h / 6 * (k1.t2 + 2 * k2.t2 + 2 * k3.t2 + k4.t2),
                  s.w1 + h / 6 * (k1.w1 + 2 * k2.w1 + 2 * k3.w1 + k4.w1),
                  s.w2 + h / 6 * (k1.w2 + 2 * k2.w2 + 2 * k3.w2 + k4.w2)};
}

/* énergie mécanique : sert à vérifier l'intégrateur (--verifier) */
static double energie(Etat s) {
    double cin = s.w1 * s.w1 + 0.5 * s.w2 * s.w2 + s.w1 * s.w2 * cos(s.t1 - s.t2);
    double pot = -2 * cos(s.t1) - cos(s.t2);
    return cin + pot;
}

/* ------------------------------------------------------------ couleurs */

typedef struct { uint8_t r, g, b; } RGB;

/* dégradé noir -> violet profond -> magenta -> orange -> jaune pâle */
static RGB palette(double x) {
    static const double P[][3] = {{0, 0, 0}, {0.38, 0, 0.54}, {0.85, 0.12, 0.55}, {1, 0.55, 0.15}, {1, 0.97, 0.75}};
    const int n = 5;
    if (x <= 0) x = 0;
    if (x >= 1) x = 1;
    double u = x * (n - 1);
    int i = (int)u;
    if (i >= n - 1) i = n - 2;
    double f = u - i;
    RGB c;
    c.r = (uint8_t)(255 * (P[i][0] + f * (P[i + 1][0] - P[i][0])));
    c.g = (uint8_t)(255 * (P[i][1] + f * (P[i + 1][1] - P[i][1])));
    c.b = (uint8_t)(255 * (P[i][2] + f * (P[i + 1][2] - P[i][2])));
    return c;
}

/* couleur continue sur le tore (θ1, θ2) : la même pour θ et θ + 2π */
static RGB couleur_angles(double t1, double t2) {
    double r = 0.5 + 0.5 * cos(t1), g = 0.5 + 0.5 * cos(t2), b = 0.5 + 0.5 * sin(t1 + t2);
    return (RGB){(uint8_t)(255 * (0.15 + 0.85 * r)), (uint8_t)(255 * (0.1 + 0.6 * g)), (uint8_t)(255 * (0.2 + 0.8 * b))};
}

/* ------------------------------------------------------------- PNG brut */

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}
static uint32_t crc_maj(uint32_t c, const uint8_t *b, size_t n) {
    while (n--) c = crc_table[(c ^ *b++) & 0xFF] ^ (c >> 8);
    return c;
}
static void be32(FILE *f, uint32_t v) {
    uint8_t b[4] = {v >> 24, v >> 16, v >> 8, v};
    fwrite(b, 1, 4, f);
}
static void chunk(FILE *f, const char *type, const uint8_t *d, size_t n) {
    be32(f, (uint32_t)n);
    uint32_t c = crc_maj(crc_maj(0xFFFFFFFFu, (const uint8_t *)type, 4), d, n);
    fwrite(type, 1, 4, f);
    fwrite(d, 1, n, f);
    be32(f, ~c);
}
static int png(const char *chemin, int w, int h, const RGB *px) {
    size_t ligne = (size_t)w * 3 + 1, n = ligne * h, nb = n / 65535 + 1;
    uint8_t *brut = malloc(n), *z = malloc(n + nb * 5 + 6), *p = z;
    if (!brut || !z) return -1;
    for (int y = 0; y < h; y++) {
        brut[y * ligne] = 0;
        memcpy(brut + y * ligne + 1, px + (size_t)y * w, (size_t)w * 3);
    }
    *p++ = 0x78; *p++ = 0x01;
    uint32_t a = 1, b = 0;
    for (size_t pos = 0; pos < n;) {
        size_t k = n - pos > 65535 ? 65535 : n - pos;
        *p++ = pos + k == n;
        *p++ = k; *p++ = k >> 8; *p++ = ~k; *p++ = ~k >> 8;
        memcpy(p, brut + pos, k);
        for (size_t i = 0; i < k; i++) { a = (a + brut[pos + i]) % 65521; b = (b + a) % 65521; }
        p += k; pos += k;
    }
    uint32_t ad = b << 16 | a;
    *p++ = ad >> 24; *p++ = ad >> 16; *p++ = ad >> 8; *p++ = ad;
    FILE *f = fopen(chemin, "wb");
    if (!f) { free(brut); free(z); return -1; }
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f);
    uint8_t ihdr[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
    chunk(f, "IHDR", ihdr, 13);
    chunk(f, "IDAT", z, (size_t)(p - z));
    chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(brut); free(z);
    return 0;
}

/* --------------------------------------------------------------- calcul */

enum { BASCULE, LYAPUNOV, ETAT };

typedef struct {
    int mode, w, h, images;
    double cx, cy, largeur; /* fenêtre en radians */
    double dt, tmax;
    RGB *px;     /* images * w * h */
    atomic_int ligne_suivante;
    atomic_long pas_total;
} Travail;

static RGB pixel_bascule(const Travail *T, double t1, double t2, long *pas) {
    /* conservation de l'énergie : si 2cos θ1 + cos θ2 > 1, aucun bras ne peut faire le tour */
    if (2 * cos(t1) + cos(t2) > 1) return (RGB){0, 0, 0};
    Etat s = {t1, t2, 0, 0};
    long n = (long)(T->tmax / T->dt), i;
    for (i = 1; i <= n; i++) {
        s = rk4(s, T->dt);
        if (fabs(s.t1) > M_PI || fabs(s.t2) > M_PI) break;
    }
    *pas += i;
    if (i > n) return (RGB){8, 0, 14};
    /* temps de bascule en échelle log : rapide = clair */
    double t = i * T->dt, x = 1 - log(t) / log(T->tmax);
    return palette(0.12 + 0.88 * x);
}

static RGB pixel_lyapunov(const Travail *T, double t1, double t2, long *pas) {
    /* méthode de Benettin : on suit l'écart entre deux pendules et on le renormalise */
    const double d0 = 1e-8;
    Etat a = {t1, t2, 0, 0}, b = {t1 + d0, t2, 0, 0};
    long n = (long)(T->tmax / T->dt), k = (long)(1.0 / T->dt);
    double somme = 0;
    for (long i = 1; i <= n; i++) {
        a = rk4(a, T->dt);
        b = rk4(b, T->dt);
        if (i % k == 0 || i == n) {
            double e[4] = {b.t1 - a.t1, b.t2 - a.t2, b.w1 - a.w1, b.w2 - a.w2};
            double d = sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2] + e[3] * e[3]);
            somme += log(d / d0);
            b = (Etat){a.t1 + e[0] * d0 / d, a.t2 + e[1] * d0 / d, a.w1 + e[2] * d0 / d, a.w2 + e[3] * d0 / d};
        }
    }
    *pas += 2 * n;
    double lambda = somme / T->tmax; /* exposant de Lyapunov estimé */
    return palette(lambda / 0.9);
}

static void pixel_etat(const Travail *T, double t1, double t2, long *pas, RGB *sortie, size_t ecart) {
    Etat s = {t1, t2, 0, 0};
    long n = (long)(T->tmax / T->dt);
    int img = 0;
    for (long i = 0; i <= n && img < T->images; i++) {
        /* image k prise au temps k·T/(images-1) */
        long cible = T->images == 1 ? n : (long)llround((double)img * n / (T->images - 1));
        while (img < T->images && i == cible) {
            sortie[img * ecart] = couleur_angles(s.t1, s.t2);
            img++;
            cible = T->images == 1 ? n : (long)llround((double)img * n / (T->images - 1));
        }
        if (i < n) s = rk4(s, T->dt);
    }
    *pas += n;
}

static void *ouvrier(void *arg) {
    Travail *T = arg;
    long pas = 0;
    int y;
    while ((y = atomic_fetch_add(&T->ligne_suivante, 1)) < T->h) {
        double pixel = T->largeur / T->w;
        double t2 = T->cy + (T->h / 2.0 - y - 0.5) * pixel;
        for (int x = 0; x < T->w; x++) {
            double t1 = T->cx + (x + 0.5 - T->w / 2.0) * pixel;
            size_t i = (size_t)y * T->w + x;
            if (T->mode == BASCULE) T->px[i] = pixel_bascule(T, t1, t2, &pas);
            else if (T->mode == LYAPUNOV) T->px[i] = pixel_lyapunov(T, t1, t2, &pas);
            else pixel_etat(T, t1, t2, &pas, T->px + i, (size_t)T->w * T->h);
        }
        if (y % 16 == 0 && isatty(2)) {
            fprintf(stderr, "\r%3d %%", 100 * (T->ligne_suivante < T->h ? T->ligne_suivante : T->h) / T->h);
            fflush(stderr);
        }
    }
    atomic_fetch_add(&T->pas_total, pas);
    return NULL;
}

static double maintenant(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

/* intègre un seul pendule et affiche la dérive de l'énergie, pour juger dt */
static void verifier(double t1, double t2, double dt, double tmax) {
    Etat s = {t1, t2, 0, 0};
    double e0 = energie(s), pire = 0;
    long n = (long)(tmax / dt);
    for (long i = 1; i <= n; i++) {
        s = rk4(s, dt);
        double e = fabs(energie(s) - e0);
        if (e > pire) pire = e;
    }
    printf("θ1=%.3f θ2=%.3f dt=%g T=%g : énergie %.9f, dérive max %.2e\n", t1, t2, dt, tmax, e0, pire);
}

static void aide(void) {
    puts("pendule — cartes du chaos d'un double pendule\n\n"
         "  pendule [bascule|lyapunov|etat] [options] -o image.png\n\n"
         "  -o FICHIER       image de sortie (PNG)\n"
         "  --taille N       largeur et hauteur en pixels (défaut 600)\n"
         "  --temps T        durée simulée (défaut : 100 / 20 / 15 selon le mode)\n"
         "  --dt H           pas de Runge-Kutta (défaut 0.01)\n"
         "  --centre X Y     centre de la vue en radians (défaut 0 0)\n"
         "  --largeur L      largeur de la vue en radians (défaut 2π)\n"
         "  --video DOSSIER  (mode etat) une image par instant, pour en faire un film\n"
         "  --images N       nombre d'images du film (défaut 120)\n"
         "  --fils N         nombre de threads (défaut : tous les cœurs)\n"
         "  --verifier θ1 θ2 juste mesurer la dérive d'énergie d'un pendule\n");
}

int main(int argc, char **argv) {
    Travail T = {.mode = BASCULE, .w = 600, .h = 600, .images = 1, .cx = 0, .cy = 0, .largeur = 2 * M_PI, .dt = 0.01, .tmax = -1};
    const char *sortie = NULL, *video = NULL;
    int fils = (int)sysconf(_SC_NPROCESSORS_ONLN), images_video = 120, verif = 0;
    double v1 = 0, v2 = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        int reste = argc - i - 1;
#define BESOIN(k) if (reste < k) { fprintf(stderr, "valeur manquante après %s\n", a); return 1; }
        if (!strcmp(a, "bascule")) T.mode = BASCULE;
        else if (!strcmp(a, "lyapunov")) T.mode = LYAPUNOV;
        else if (!strcmp(a, "etat") || !strcmp(a, "état")) T.mode = ETAT;
        else if (!strcmp(a, "-o")) { BESOIN(1); sortie = argv[++i]; }
        else if (!strcmp(a, "--taille")) { BESOIN(1); T.w = T.h = atoi(argv[++i]); }
        else if (!strcmp(a, "--temps")) { BESOIN(1); T.tmax = atof(argv[++i]); }
        else if (!strcmp(a, "--dt")) { BESOIN(1); T.dt = atof(argv[++i]); }
        else if (!strcmp(a, "--centre")) { BESOIN(2); T.cx = atof(argv[++i]); T.cy = atof(argv[++i]); }
        else if (!strcmp(a, "--largeur")) { BESOIN(1); T.largeur = atof(argv[++i]); }
        else if (!strcmp(a, "--video")) { BESOIN(1); video = argv[++i]; T.mode = ETAT; }
        else if (!strcmp(a, "--images")) { BESOIN(1); images_video = atoi(argv[++i]); }
        else if (!strcmp(a, "--fils")) { BESOIN(1); fils = atoi(argv[++i]); }
        else if (!strcmp(a, "--verifier")) { BESOIN(2); verif = 1; v1 = atof(argv[++i]); v2 = atof(argv[++i]); }
        else if (!strcmp(a, "-h") || !strcmp(a, "--aide")) { aide(); return 0; }
        else { fprintf(stderr, "option inconnue : %s (voir --aide)\n", a); return 1; }
    }
    if (verif) {
        for (double h = 0.04; h >= 0.0025; h /= 2) verifier(v1, v2, h, T.tmax > 0 ? T.tmax : 100);
        return 0;
    }
    if (!sortie && !video) { aide(); return 1; }
    if (T.w < 8 || T.w > 8000 || T.dt <= 0 || fils < 1 || images_video < 2) { fputs("paramètres invalides\n", stderr); return 1; }
    if (T.tmax <= 0) T.tmax = T.mode == BASCULE ? 100 : T.mode == LYAPUNOV ? 20 : 15;
    if (video) T.images = images_video;

    T.px = calloc((size_t)T.w * T.h * T.images, sizeof(RGB));
    if (!T.px) { fputs("plus de mémoire\n", stderr); return 1; }
    double debut = maintenant();
    pthread_t th[256];
    if (fils > 256) fils = 256;
    for (int k = 0; k < fils; k++) pthread_create(&th[k], NULL, ouvrier, &T);
    for (int k = 0; k < fils; k++) pthread_join(th[k], NULL);
    double duree = maintenant() - debut;
    fprintf(stderr, "\r%d×%d pixels, %.2e pas de RK4 en %.1f s sur %d threads (%.1f M pas/s)\n", T.w, T.h,
            (double)T.pas_total, duree, fils, T.pas_total / duree / 1e6);

    crc_init();
    if (video) {
        mkdir(video, 0755);
        char chemin[4096];
        for (int k = 0; k < T.images; k++) {
            snprintf(chemin, sizeof chemin, "%s/%04d.png", video, k);
            if (png(chemin, T.w, T.h, T.px + (size_t)k * T.w * T.h)) { perror(chemin); return 1; }
        }
        fprintf(stderr, "%d images dans %s/ — pour la vidéo :\n  ffmpeg -framerate 30 -i %s/%%04d.png -pix_fmt yuv420p chaos.mp4\n",
                T.images, video, video);
    }
    if (sortie) {
        if (png(sortie, T.w, T.h, T.px + (size_t)(T.images - 1) * T.w * T.h)) { perror(sortie); return 1; }
        fprintf(stderr, "-> %s\n", sortie);
    }
    free(T.px);
    return 0;
}
