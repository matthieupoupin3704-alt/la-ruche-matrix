#pragma once
#include <FastLED.h>
#include <math.h>

// Dimensions dynamiques (définies dans main.cpp)
extern uint8_t  matW;
extern uint8_t  matH;
extern uint16_t NUM_LEDS;

#define MATRIX_W   matW
#define MATRIX_H   matH
#define TOTAL_LEDS NUM_LEDS

static constexpr uint8_t  MATRIX_W_MAX = 64;
static constexpr uint8_t  MATRIX_H_MAX = 32;
static constexpr uint16_t MAX_LEDS_EFF = MATRIX_W_MAX * MATRIX_H_MAX;

extern CRGB*           pLEDs;
extern uint8_t         COULEUR_HUE;
extern int             vitesseEffets;
extern volatile uint8_t audioLevel;

static constexpr uint8_t HUE_DELTA = 15;
static constexpr uint8_t BRI_BASE  = 80;

// Pixel nul partagé (thread-safe en lecture seule — on n'écrit jamais dedans après init)
namespace { CRGB _nullPx = CRGB::Black; }

// Accès sécurisé — retourne une ref vers _nullPx si hors-limites
static inline CRGB& _px(int16_t x, int16_t y) {
    if (static_cast<uint16_t>(x) >= static_cast<uint16_t>(MATRIX_W) ||
        static_cast<uint16_t>(y) >= static_cast<uint16_t>(MATRIX_H))
        return _nullPx;
    return pLEDs[static_cast<uint16_t>(y) * MATRIX_W + x];
}

// Les 3 couleurs analogues
static inline CRGB _c0(uint8_t bri = 220) { return CHSV(COULEUR_HUE - HUE_DELTA, 255, bri); }
static inline CRGB _c1(uint8_t bri = 220) { return CHSV(COULEUR_HUE,             240, bri); }
static inline CRGB _c2(uint8_t bri = 220) { return CHSV(COULEUR_HUE + HUE_DELTA, 220, bri); }

// Fond coloré
static inline CRGB _base() { return CHSV(COULEUR_HUE, 200, BRI_BASE); }
static inline CRGB _white(uint8_t bri = 255) { return CRGB(bri, bri, bri); }

// Luminosité avec plancher garanti
static inline uint8_t _bri(float normalized) {
    if (normalized < 0.0f) normalized = 0.0f;
    if (normalized > 1.0f) normalized = 1.0f;
    return (uint8_t)(BRI_BASE + normalized * (255 - BRI_BASE));
}

// Palette feu
static inline CRGBPalette16 _firePalette() {
    return CRGBPalette16(
        CHSV(COULEUR_HUE, 255, 10),
        CHSV(COULEUR_HUE, 250, 38),
        _c0(100),   _c0(155),
        _c0(165),   _c0(220),   _c0(225),   _c1(232),
        _c1(238),   _c1(248),   _c2(255),   _c2(255),
        CHSV(COULEUR_HUE + HUE_DELTA, 110, 255),
        CHSV(COULEUR_HUE + HUE_DELTA, 110, 255),
        CHSV(COULEUR_HUE + HUE_DELTA,  80, 255),
        _white(255)
    );
}

// Helper float aléatoire 0..1
static inline float random8f() { return random8() / 255.0f; }

// Tracé de ligne Bresenham
static inline void _drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, CRGB color) {
    int16_t dx = abs(x1-x0), dy = abs(y1-y0);
    int16_t sx = x0<x1?1:-1, sy = y0<y1?1:-1, err = dx-dy;
    for (uint8_t step = 0; step < 40; step++) {
        _px(x0, y0) += color;
        if (x0==x1 && y0==y1) break;
        int16_t e2 = 2*err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

namespace effects {

// --- état global ---
static uint8_t _heat       [MAX_LEDS_EFF];
static float   _bY  [5],   _bVY[5];
static uint8_t _bX  [5];
static float   _ripR [3]  = { 0.0f, 8.0f, 16.0f };
static float   _ripCX[3], _ripCY[3];
static uint8_t _matrixHead[MATRIX_W_MAX];
static uint8_t _matrixLen [MATRIX_W_MAX];
static float   _distMap   [MAX_LEDS_EFF];
static float   _angMap    [MAX_LEDS_EFF];
static float   _t    = 0.0f;
static float   _lavaT = 0.0f;

// --- flags init effets stateful ---
static bool _twInit   = false;
static bool _bb2d     = false;
static bool _sfInit   = false;
static bool _golInit  = false;
static bool _rainInit = false;
static bool _dropsInit = false;
static bool _profInit = false;
static bool _spkInit  = false;

static constexpr uint8_t COUNT = 32;

static void _buildDistMaps() {
    for (int y = 0; y < MATRIX_H; y++) {
        for (int x = 0; x < MATRIX_W; x++) {
            const float dx = x - MATRIX_W * 0.5f;
            const float dy = y - MATRIX_H * 0.5f;
            _distMap[y * MATRIX_W + x] = sqrtf(dx*dx + dy*dy);
            _angMap [y * MATRIX_W + x] = atan2f(dy, dx);
        }
    }
}

static void _resetState() {
    memset(_heat, 0, sizeof(_heat));
    for (uint8_t i = 0; i < 5; i++) {
        _bY [i] = random8(MATRIX_H);
        _bVY[i] = random8(10, 30) / 20.0f;
        _bX [i] = random8(MATRIX_W);
    }
    for (uint8_t x = 0; x < MATRIX_W_MAX; x++) {
        _matrixHead[x] = random8(MATRIX_H);
        _matrixLen [x] = random8(4, 14);
    }
    for (uint8_t i = 0; i < 3; i++) {
        _ripCX[i] = 5 + random8(MATRIX_W - 10);
        _ripCY[i] = 3 + random8(MATRIX_H - 6);
    }
    _lavaT = 0.0f;
    _twInit = _bb2d = _sfInit = _golInit = _rainInit = _dropsInit = _profInit = _spkInit = false;
    _buildDistMaps();
}

void init()   { _resetState(); }
void reinit() { _resetState(); }

// --- effets ---
void solidPulse() {
    float wave = sinf(_t * 2.0f) * 0.5f + sinf(_t * 3.1f) * 0.35f + sinf(_t * 1.7f) * 0.15f;
    float audioBri = audioLevel / 255.0f * 0.45f; 
    float baseBri = (wave + 1.0f) * 0.5f + audioBri;
    if (baseBri > 1.0f) baseBri = 1.0f;
    uint8_t sat   = 195 + (uint8_t)(baseBri * 60.0f);
    int8_t  hueOff = (int8_t)(wave * HUE_DELTA);

    for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float dist     = _distMap[y * MATRIX_W + x];
            float radial   = sinf(dist * 0.55f - _t * 1.8f) * 0.18f;
            float diagonal = sinf((x + y) * 0.22f + _t * 1.1f) * 0.10f;
            int8_t pixHue  = hueOff + (int8_t)((x - MATRIX_W * 0.5f) * 0.2f);
            _px(x, y) = CHSV(COULEUR_HUE + pixHue, sat, _bri(baseBri + radial + diagonal));
        }
    }
}

void fire() {
    CRGBPalette16 pal = _firePalette();
    uint8_t cooling  = map(vitesseEffets, 1, 100, 35, 12);
    uint8_t ignition = map(vitesseEffets, 1, 100, 110, 230);

    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t cool = random8(0, ((cooling * 10) / MATRIX_H) + 2);
        if (random8() < 25) cool += random8(3, 12);
        _heat[i] = qsub8(_heat[i], cool);
    }

    for (int y = MATRIX_H - 1; y >= 2; y--) {
        uint16_t rowOff = y * MATRIX_W;
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            int8_t drift = (int8_t)random8(0, 4) - 1; 
            uint8_t nx = (uint8_t)constrain((int)x + drift, 0, MATRIX_W - 1);
            _heat[rowOff + x] = ((uint16_t)_heat[rowOff - MATRIX_W + nx] * 2 +
                                 (uint16_t)_heat[rowOff - MATRIX_W + x]  * 3 +
                                 (uint16_t)_heat[rowOff - 2*MATRIX_W + x]) / 6;
        }
    }

    uint8_t audioBoost = map(audioLevel, 0, 255, 0, 90);
    for (uint8_t x = 0; x < MATRIX_W; x++) {
        float boost = 1.0f - fabsf(x - MATRIX_W * 0.5f) / (MATRIX_W * 0.5f);
        uint8_t ign = qadd8((uint8_t)(ignition * (0.7f + boost * 0.3f)), audioBoost);
        if (random8() < 185) _heat[x] = qadd8(_heat[x], random8(ign / 2, ign));
    }

    static uint8_t spkX[8];
    static float   spkY[8], spkV[8];
    if (!_spkInit) {
        for (uint8_t i = 0; i < 8; i++) {
            spkX[i] = random8(MATRIX_W);
            spkY[i] = MATRIX_H - 1;
            spkV[i] = random8(8, 25) / 10.0f;
        }
        _spkInit = true;
    }
    for (uint8_t i = 0; i < 8; i++) {
        spkY[i] -= spkV[i] * 0.45f;
        if (spkY[i] < 0) {
            spkX[i] = random8(MATRIX_W);
            spkY[i] = MATRIX_H - 1 - random8(3);
            spkV[i] = random8(8, 25) / 10.0f;
        }
        uint8_t sy = (uint8_t)spkY[i];
        if (sy < MATRIX_H)
            _heat[sy * MATRIX_W + spkX[i]] = qadd8(_heat[sy * MATRIX_W + spkX[i]], 140);
    }

    for (uint8_t y = 0; y < MATRIX_H; y++) {
        uint16_t srcRow = (MATRIX_H - 1 - y) * MATRIX_W;
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            uint8_t heat = _heat[srcRow + x];
            if (heat > 185) heat = 255; 
            _px(x, y) = ColorFromPalette(pal, heat, 255, LINEARBLEND);
        }
    }
}

void meteorRain() {
    const uint8_t TAIL = 10;
    uint32_t tn = (uint32_t)(_t * 80.0f);

    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t x = i % MATRIX_W, y = i / MATRIX_W;
        uint8_t n = inoise8(x * 8 + tn, y * 8);
        uint8_t hue = COULEUR_HUE + (int8_t)((n - 128) * (int16_t)HUE_DELTA / 128);
        pLEDs[i] = CHSV(hue, 215, _bri(n / 255.0f * 0.28f + 0.05f));
    }

    for (uint8_t y = 0; y < MATRIX_H; y += 2) {
        float phase = _t * 10.0f + y * 7.5f;
        uint8_t headX = (uint32_t)phase % (MATRIX_W + TAIL);
        for (uint8_t i = 0; i < TAIL; i++) {
            int16_t x = (int16_t)headX - i;
            if (x >= 0 && x < MATRIX_W) {
                float frac = 1.0f - (float)i / TAIL;
                uint8_t bri = (uint8_t)(frac * frac * 255.0f);
                uint8_t sat = (i == 0) ? 100 : (i < TAIL/3) ? 170 : 230;
                uint8_t hue = (i < TAIL/3) ? COULEUR_HUE + HUE_DELTA : COULEUR_HUE;
                _px(x, y) += CHSV(hue, sat, bri);
            }
        }
        if (headX < MATRIX_W) {
            if (y > 0)           _px(headX, y-1) += CHSV(COULEUR_HUE+HUE_DELTA, 200, 70);
            if (y < MATRIX_H-1)  _px(headX, y+1) += CHSV(COULEUR_HUE+HUE_DELTA, 200, 70);
        }
    }
    for (uint8_t x = 0; x < MATRIX_W; x += 8) {
        float phase = _t * 8.0f + x * 5.3f;
        uint8_t headY = (uint32_t)phase % (MATRIX_H + 6);
        for (uint8_t i = 0; i < 6; i++) {
            int16_t y = (int16_t)headY - i;
            if (y >= 0 && y < MATRIX_H) {
                float frac = 1.0f - (float)i / 6.0f;
                _px(x, y) += CHSV(COULEUR_HUE - HUE_DELTA, 220, (uint8_t)(frac * frac * 200.0f));
            }
        }
    }
}

void twinkle() {
    struct TwinkleStar { uint8_t x, y, hue, sat; float phase, speed; };
    static TwinkleStar stars[24];
    if (!_twInit) {
        for (uint8_t i = 0; i < 24; i++) {
            stars[i] = { random8(MATRIX_W_MAX), random8(MATRIX_H_MAX),
                         (uint8_t)(COULEUR_HUE + (int8_t)random8(HUE_DELTA*2) - HUE_DELTA),
                         (uint8_t)(random8(60) + 140),
                         random8f() * 6.28f, 0.04f + random8f() * 0.06f };
        }
        _twInit = true;
    }

    fill_solid(pLEDs, TOTAL_LEDS, CRGB::Black);
    uint32_t tn = (uint32_t)(_t * 8.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t x = i % MATRIX_W, y = i / MATRIX_W;
        uint8_t n = inoise8(x * 18, y * 18 + tn);
        if (n > 170) {
            uint8_t bri = (uint8_t)((n - 170) / 85.0f * 18.0f);
            pLEDs[i] = CHSV(COULEUR_HUE - HUE_DELTA, 230, bri);
        }
    }

    float speedMul = 0.5f + vitesseEffets * 0.015f;
    for (uint8_t i = 0; i < 24; i++) {
        TwinkleStar& s = stars[i];
        if (s.x >= MATRIX_W || s.y >= MATRIX_H) {
            s.x = random8(MATRIX_W); s.y = random8(MATRIX_H);
        }
        s.phase += s.speed * speedMul;
        if (s.phase > 6.28f) {
            s.phase -= 6.28f;
            s.x   = random8(MATRIX_W); s.y = random8(MATRIX_H);
            s.hue = (uint8_t)(COULEUR_HUE + (int8_t)(random8(HUE_DELTA*2)) - HUE_DELTA);
            s.sat = random8(80) + 120;
            s.speed = 0.03f + random8f() * 0.05f;
        }
        float brightness = (sinf(s.phase) + 1.0f) * 0.5f; 
        uint8_t bri = (uint8_t)(brightness * brightness * 255.0f);
        if (bri < 8) continue;

        _px(s.x, s.y) = CHSV(s.hue, (uint8_t)(s.sat * (1.0f - brightness * 0.5f)), bri);
        uint8_t armBri = (uint8_t)(bri * 0.55f);
        uint8_t arm2Bri = (uint8_t)(bri * 0.22f);
        _px(s.x+1, s.y) += CHSV(s.hue, s.sat, armBri);
        _px(s.x-1, s.y) += CHSV(s.hue, s.sat, armBri);
        _px(s.x, s.y+1) += CHSV(s.hue, s.sat, armBri);
        _px(s.x, s.y-1) += CHSV(s.hue, s.sat, armBri);
        if (brightness > 0.6f) {
            _px(s.x+2, s.y) += CHSV(s.hue, s.sat, arm2Bri);
            _px(s.x-2, s.y) += CHSV(s.hue, s.sat, arm2Bri);
            _px(s.x, s.y+2) += CHSV(s.hue, s.sat, arm2Bri);
            _px(s.x, s.y-2) += CHSV(s.hue, s.sat, arm2Bri);
            _px(s.x+1, s.y+1) += CHSV(s.hue, s.sat, arm2Bri);
            _px(s.x-1, s.y+1) += CHSV(s.hue, s.sat, arm2Bri);
            _px(s.x+1, s.y-1) += CHSV(s.hue, s.sat, arm2Bri);
            _px(s.x-1, s.y-1) += CHSV(s.hue, s.sat, arm2Bri);
        }
    }
}

void plasma() {
    float sx[MATRIX_W_MAX], sy[MATRIX_H_MAX], sxy[MATRIX_W_MAX + MATRIX_H_MAX];
    float speed = 0.8f + vitesseEffets * 0.008f;
    for (int x = 0; x < MATRIX_W; x++) sx[x]  = sinf(x * 0.22f + _t * 1.2f * speed);
    for (int y = 0; y < MATRIX_H; y++) sy[y]  = sinf(y * 0.22f - _t * 0.9f * speed);
    for (int i = 0; i < MATRIX_W+MATRIX_H; i++) sxy[i] = sinf(i * 0.14f + _t * 0.7f * speed);

    float audioAmp = 1.0f + audioLevel / 255.0f * 0.8f; 
    for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float dist   = _distMap[y * MATRIX_W + x];
            float radial = sinf(dist * 0.5f - _t * 1.4f * speed) * 0.3f;
            float v = (sx[x] + sy[y] + sxy[x + y] + radial) * 0.25f * audioAmp;
            uint8_t hue = COULEUR_HUE + (int8_t)(v * HUE_DELTA * 1.4f);
            uint8_t sat = 215 + (uint8_t)(fabsf(v) * 40.0f);
            _px(x, y) = CHSV(hue, sat, _bri((v + 1.0f) * 0.5f));
        }
    }
}

void bouncingBalls() {
    float sx[MATRIX_W_MAX], sy[MATRIX_H_MAX];
    for (int x = 0; x < MATRIX_W; x++) sx[x] = sinf(x * 0.2f + _t * 0.8f);
    for (int y = 0; y < MATRIX_H; y++) sy[y] = sinf(y * 0.2f - _t * 0.5f);

    for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            _px(x, y) = CHSV(COULEUR_HUE + (int8_t)(sx[x]*sy[y]*HUE_DELTA), 190,
                          _bri((sx[x]*sy[y] + 1.0f) * 0.28f));
        }
    }

    static float _bfX[5], _bvX[5];
    if (!_bb2d) {
        for (uint8_t i = 0; i < 5; i++) {
            _bfX[i] = _bX[i];
            _bvX[i] = (random8(8, 22) / 10.0f) * (random8(2) ? 1.0f : -1.0f);
        }
        _bb2d = true;
    }

    float gravity = 0.06f + vitesseEffets * 0.005f;
    for (uint8_t i = 0; i < 5; i++) {
        _bfX[i] += _bvX[i] * 0.35f;
        if (_bfX[i] < 0)             { _bfX[i] = 0;              _bvX[i] =  fabsf(_bvX[i]); }
        if (_bfX[i] >= MATRIX_W - 1) { _bfX[i] = MATRIX_W - 1;   _bvX[i] = -fabsf(_bvX[i]); }

        _bY[i]  += _bVY[i];
        _bVY[i] += gravity;
        if (_bY[i] >= MATRIX_H - 1) {
            _bY[i]  = MATRIX_H - 1;
            _bVY[i] *= -0.88f;
            float audioKick = audioLevel / 255.0f * 2.5f; 
            if (fabsf(_bVY[i]) < 0.5f) _bVY[i] = -(1.5f + random8(10) * 0.1f + audioKick);
        }
        uint8_t hue = COULEUR_HUE + (i%3==0 ? -(int)HUE_DELTA : i%3==1 ? 0 : (int)HUE_DELTA);
        uint8_t px  = (uint8_t)_bfX[i];
        uint8_t py  = (uint8_t)_bY[i];

        float squash = fabsf(_bVY[i]) / 2.5f;
        if (_bY[i] >= MATRIX_H - 1.8f && squash > 0.2f) {
            squash = (squash > 1.0f) ? 1.0f : squash;
            for (int8_t dx = -4; dx <= 4; dx++) {
                float f = 1.0f - fabsf(dx) / 4.5f;
                _px(px+dx, py) += CHSV(hue, 240, (uint8_t)(f * squash * 180.0f));
            }
        }

        for (int8_t dy = -2; dy <= 2; dy++) {
            for (int8_t dx = -2; dx <= 2; dx++) {
                float dist = sqrtf((float)(dx*dx + dy*dy));
                if (dist > 2.3f) continue;
                float glow = 1.0f - dist / 2.3f;
                _px(px+dx, py+dy) += CHSV(hue, 220, (uint8_t)(glow*glow*160.0f));
            }
        }
        _px(px, py) = CHSV(hue, 130, 255);
    }
}

void ripple() {
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        float wave = sinf(_distMap[i] * 0.6f - _t * 2.2f + _angMap[i] * 0.5f);
        pLEDs[i] = CHSV(COULEUR_HUE + (int8_t)(wave*HUE_DELTA), 218, _bri((wave + 1.0f) * 0.38f + 0.08f));
    }

    float ripSpeed = 0.12f + vitesseEffets * 0.012f;
    float audioRipBri = 1.0f + audioLevel / 255.0f * 1.8f; 
    for (uint8_t r = 0; r < 3; r++) {
        _ripR[r] += ripSpeed;
        float ripMax = 22.0f - audioLevel / 255.0f * 8.0f;
        if (_ripR[r] > ripMax) {
            _ripR[r]  = 0.0f;
            _ripCX[r] = 4 + random8(MATRIX_W - 8);
            _ripCY[r] = 3 + random8(MATRIX_H - 6);
        }
        float rr     = _ripR[r];
        uint8_t bright = (uint8_t)constrain(int((245.0f - rr * 9.5f) * audioRipBri), 0, 255);
        if (bright < 20) continue;
        uint8_t hue = COULEUR_HUE + (r==0 ? (int)HUE_DELTA : r==1 ? 0 : -(int)HUE_DELTA);
        for (uint16_t a = 0; a < 360; a += 4) {
            float ang = a * (PI / 180.0f);
            _px((int16_t)(_ripCX[r] + rr*cosf(ang)),
                (int16_t)(_ripCY[r] + rr*sinf(ang))) += CHSV(hue, 238, bright);
        }
        float rr2 = rr - 5.0f;
        if (rr2 > 0) {
            uint8_t b2 = (uint8_t)(bright * 0.38f);
            if (b2 > 15) {
                for (uint16_t a = 0; a < 360; a += 6) {
                    float ang = a * (PI / 180.0f);
                    _px((int16_t)(_ripCX[r] + rr2*cosf(ang)),
                        (int16_t)(_ripCY[r] + rr2*sinf(ang))) += CHSV(hue+8, 220, b2);
                }
            }
        }
    }
}

void wave() {
    float sx[MATRIX_W_MAX], sy[MATRIX_H_MAX], sxy[MATRIX_W_MAX + MATRIX_H_MAX];
    float speed = 0.8f + vitesseEffets * 0.008f;
    float audioWave = 1.0f + audioLevel / 255.0f * 1.4f; 
    for (int x = 0; x < MATRIX_W; x++) sx[x]  = sinf((x/(float)MATRIX_W)*6.28f + _t*2.0f*speed) * 0.28f * audioWave;
    for (int y = 0; y < MATRIX_H; y++) sy[y]  = sinf((y/(float)MATRIX_H)*6.28f - _t*1.3f*speed) * 0.22f * audioWave;
    for (int i = 0; i < MATRIX_W+MATRIX_H; i++)
        sxy[i] = sinf((i/(float)MATRIX_W)*9.42f + _t*1.7f*speed) * 0.20f * audioWave;

    uint32_t tn  = (uint32_t)(_t * 55.0f);
    for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float dist   = _distMap[y * MATRIX_W + x];
            float radial = sinf(dist * 0.4f + _t * 2.5f * speed) * 0.18f;
            float v = sx[x] + sy[y] + sxy[x+y] + radial;
            uint8_t shim = inoise8(x*22, y*22 + tn);
            v += (shim / 255.0f - 0.5f) * 0.06f;
            uint8_t hue = COULEUR_HUE + (int8_t)(v * HUE_DELTA * 1.2f);
            uint8_t bri, sat;
            if (v > 0.62f) { bri = 255; sat = 190; } 
            else { bri = _bri((v + 1.0f) * 0.5f); sat = 228; }
            _px(x, y) = CHSV(hue, sat, bri);
        }
    }
}

void scanner() {
    uint32_t tn = (uint32_t)(_t * 55.0f);
    for (uint8_t y = 0; y < MATRIX_H; y++) {
        float sy2 = y * 0.1f;
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float v = sinf(x * 0.2f + _t * 0.5f + sy2);
            uint8_t n = inoise8(x*8 + tn, y*8);
            _px(x, y) = CHSV(COULEUR_HUE + (int8_t)(v*(HUE_DELTA/2)), 205,
                          _bri((v+1.0f)*0.28f + 0.07f + n/255.0f*0.05f));
        }
    }

    int16_t iPos = (int16_t)((sinf(_t * 2.0f) + 1.0f) * (MATRIX_W / 2.0f));
    for (int16_t dx = -3; dx <= 3; dx++) {
        float f = expf(-dx*dx * 0.5f); 
        uint8_t bri = (uint8_t)(f * 245.0f);
        uint8_t hue = (dx < 0) ? COULEUR_HUE-HUE_DELTA : (dx > 0) ? COULEUR_HUE+HUE_DELTA : COULEUR_HUE;
        for (uint8_t y = 0; y < MATRIX_H; y++)
            _px(iPos+dx, y) += CHSV(hue, 230, bri);
    }
    int16_t gPos = MATRIX_W - 1 - iPos;
    for (int16_t dx = -2; dx <= 2; dx++) {
        float f = 1.0f - fabsf(dx) / 3.0f;
        uint8_t bri = (uint8_t)(f * 95.0f);
        uint8_t hue = COULEUR_HUE + (dx >= 0 ? -(int)HUE_DELTA : (int)HUE_DELTA);
        for (uint8_t y = 0; y < MATRIX_H; y++)
            _px(gPos+dx, y) += CHSV(hue, 200, bri);
    }
}

void sparkle() {
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        pLEDs[i].nscale8(210); 
    }
    uint32_t t32 = (uint32_t)(_t * 12.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t x = i % MATRIX_W, y = i / MATRIX_W;
        uint8_t n = inoise8(x * 20 + t32, y * 20);
        if (n > 200) pLEDs[i] += CHSV(COULEUR_HUE, 255, (uint8_t)((n-200)/55.0f * 12.0f));
    }

    uint8_t rate = (uint8_t)constrain(map(vitesseEffets, 1, 100, 2, 28) + map(audioLevel, 0, 255, 0, 25), 0, 60);
    for (uint8_t s = 0; s < 3; s++) {
        if (random8() < rate) {
            uint8_t px = random8(MATRIX_W), py = random8(MATRIX_H);
            int8_t  hueOff = (int8_t)(random8(HUE_DELTA * 2)) - HUE_DELTA;
            uint8_t hue = COULEUR_HUE + hueOff;
            _px(px, py) = CHSV(hue, 60, 255);
            for (uint8_t d = 1; d <= 3; d++) {
                uint8_t b = 255 - d * 75;
                _px(px+d, py  ) += CHSV(hue, 180 + d*20, b);
                _px(px-d, py  ) += CHSV(hue, 180 + d*20, b);
                _px(px,   py+d) += CHSV(hue, 180 + d*20, b);
                _px(px,   py-d) += CHSV(hue, 180 + d*20, b);
            }
            _px(px+1, py+1) += CHSV(hue, 210, 120);
            _px(px-1, py+1) += CHSV(hue, 210, 120);
            _px(px+1, py-1) += CHSV(hue, 210, 120);
            _px(px-1, py-1) += CHSV(hue, 210, 120);
            _px(px+2, py+1) += CHSV(hue, 230, 45);
            _px(px-2, py+1) += CHSV(hue, 230, 45);
            _px(px+1, py+2) += CHSV(hue, 230, 45);
            _px(px-1, py+2) += CHSV(hue, 230, 45);
        }
    }
}

void matrixRain() {
    for (uint8_t y = 0; y < MATRIX_H; y++) {
        uint8_t hue = COULEUR_HUE - HUE_DELTA + (uint8_t)((y * HUE_DELTA * 2) / MATRIX_H);
        uint8_t bri = (uint8_t)(BRI_BASE * 0.35f + y / (float)MATRIX_H * (BRI_BASE * 0.35f));
        CRGB rc = CHSV(hue, 235, bri);
        for (uint8_t x = 0; x < MATRIX_W; x++) _px(x, y) = rc;
    }

    uint8_t dropSpeed = (uint8_t)constrain(map(vitesseEffets, 1, 100, 6, 95) + map(audioLevel, 0, 255, 0, 50), 0, 255);
    for (uint8_t x = 0; x < MATRIX_W; x++) {
        bool moved = (random8() < dropSpeed);
        if (moved) _matrixHead[x] = (_matrixHead[x] + 1) % MATRIX_H;
        uint8_t head = _matrixHead[x], len = _matrixLen[x];

        _px(x, head) = _white(moved ? 255 : 210);

        for (uint8_t i = 1; i < len; i++) {
            int16_t y = (int16_t)head - i;
            if (y < 0) y += MATRIX_H;
            float frac = 1.0f - (float)i / len;
            uint8_t glyph = (random8() < 4) ? 35 : 0;
            uint8_t h = (i < len/3) ? COULEUR_HUE+HUE_DELTA :
                        (i < len*2/3) ? COULEUR_HUE : COULEUR_HUE-HUE_DELTA;
            _px(x, (uint8_t)y) = CHSV(h, 242, _bri(frac * frac * 0.88f) + glyph);
        }
    }
}

void vortex() {
    const float maxDist = sqrtf((MATRIX_W*0.5f)*(MATRIX_W*0.5f) + (MATRIX_H*0.5f)*(MATRIX_H*0.5f));
    float speed = 1.5f + vitesseEffets * 0.025f + audioLevel * 0.025f; 

    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        float dist = _distMap[i], ang = _angMap[i];
        ang += _t * speed + dist * 0.28f;
        float v = sinf(ang * 5.0f - dist * 0.3f) * 0.55f
                + sinf(ang * 2.0f + dist * 0.15f + _t * 0.4f) * 0.25f;
        float prox = 1.0f - dist / maxDist;
        prox = prox * prox; 
        uint8_t hue = (v < 0) ? COULEUR_HUE-HUE_DELTA : COULEUR_HUE+HUE_DELTA;
        pLEDs[i] = CHSV(hue, 238, _bri(prox * 0.55f + (v + 1.0f) * 0.24f));
    }
    float pulse = (sinf(_t * 4.5f) + 1.0f) * 0.5f;
    int16_t cx = MATRIX_W/2, cy = MATRIX_H/2;
    for (int8_t dy = -1; dy <= 1; dy++)
        for (int8_t dx = -1; dx <= 1; dx++)
            _px(cx+dx, cy+dy) += CHSV(COULEUR_HUE, 140, (uint8_t)(140 + pulse*115));
}

void colorSweep() {
    float speed = 1.5f + vitesseEffets * 0.015f;
    for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float v1 = sinf((x+y) / (float)MATRIX_W * 6.28f + _t * speed * 3.0f);
            float v2 = sinf((x-y+(int)MATRIX_H) / (float)MATRIX_W * 6.28f - _t * speed * 2.2f);
            float v  = (v1 + v2) * 0.5f;
            uint8_t hue = COULEUR_HUE + (int8_t)(v * HUE_DELTA * 1.5f);
            uint8_t sat = 210 + (uint8_t)(fabsf(v) * 45.0f);
            _px(x, y) = CHSV(hue, sat, _bri((v + 1.0f) * 0.5f));
        }
    }
}

void lavaLamp() {
    _lavaT += 0.0018f;

    uint32_t t1 = (uint32_t)(_lavaT * 28.0f);
    uint32_t t2 = (uint32_t)(_lavaT * 18.0f + 500);
    uint32_t t3 = (uint32_t)(_lavaT * 11.0f + 1000);    

    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t x = i % MATRIX_W, y = i / MATRIX_W;

        uint8_t n1 = inoise8(x * 8,       y * 14 + t1);       
        uint8_t n2 = inoise8(x * 14 + 300, y * 8  - t2);      
        uint8_t n3 = inoise8(x * 5  + 150, y * 9  + t3);      

        float f = ((uint16_t)n1 * 6 + (uint16_t)n2 * 2 + (uint16_t)n3 * 2) / (255.0f * 10.0f);

        float blob;
        if      (f < 0.42f) blob = f * 0.12f;
        else if (f < 0.60f) blob = 0.05f + (f - 0.42f) / 0.18f * 0.38f;
        else                blob = 0.43f + (f - 0.60f) * 3.8f;
        blob = constrain(blob, 0.0f, 1.0f);

        uint8_t hue = COULEUR_HUE + (int8_t)((blob * 2.0f - 1.0f) * HUE_DELTA * 1.4f);
        uint8_t sat = blob > 0.5f ? 195 : 245;
        pLEDs[i] = CHSV(hue, sat, _bri(blob * 0.92f + 0.03f));
    }
}

void aurora() {
    uint32_t tn = (uint32_t)(_t * 18.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t n = inoise8((i % MATRIX_W)*8 + tn, (i / MATRIX_W)*8);
        uint8_t bri = (uint8_t)(BRI_BASE * 0.25f + n / 255.0f * (BRI_BASE * 0.25f));
        pLEDs[i] = CHSV(COULEUR_HUE - HUE_DELTA, 225, bri);
    }

    for (uint8_t band = 0; band < 3; band++) {
        float phase1 = band * 1.8f + _t * 0.5f;
        float phase2 = band * 2.3f - _t * 0.35f;
        float yBase  = 3.0f + band * 4.5f;

        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float fx      = x / (float)MATRIX_W;
            float centerY = yBase
                + sinf(fx * 6.28f  + phase1) * 4.5f
                + sinf(fx * 12.56f + phase2) * 2.0f;
            float width   = 2.5f + sinf(fx * 3.14f + phase1*0.5f) * 1.8f;
            float intMod  = (sinf(fx * 9.42f + phase2) + 1.0f) * 0.5f;
            float curtain = (sinf(fx * 22.0f + _t * 4.2f + band * 1.7f) + 1.0f) * 0.5f;

            for (uint8_t y = 0; y < MATRIX_H; y++) {
                float dist = fabsf(y - centerY);
                if (dist >= width + 1.0f) continue;
                float intensity = 1.0f - dist / (width + 0.5f);
                if (intensity < 0) continue;
                intensity = intensity * intensity;
                intensity *= 0.58f + intMod * 0.25f + curtain * 0.17f;
                if (intensity < 0.07f) continue;

                float inner = 1.0f - dist / width;
                uint8_t hue = COULEUR_HUE
                    + (int8_t)(inner * HUE_DELTA * 2.5f - HUE_DELTA * 1.0f)
                    + (band % 2 == 0 ? -(int8_t)(HUE_DELTA/2) : (int8_t)(HUE_DELTA/2));
                uint8_t bri = (uint8_t)(intensity * 255.0f);
                uint8_t sat = 185 + (uint8_t)(inner * 70.0f);
                _px(x, y) += CHSV(hue, sat, bri);
            }
        }
    }
}

void starfield() {
    struct Star { float x, y, z, pz; };
    static Star stars[80];
    if (!_sfInit) {
        for (int i = 0; i < 80; i++)
            stars[i] = { (float)random8()-128.f, (float)random8()-128.f, (float)random8(1,255), 255.f };
        _sfInit = true;
    }

    uint32_t tn = (uint32_t)(_t * 20.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t n = inoise8((i%MATRIX_W)*10+tn, (i/MATRIX_W)*10+tn/2);
        uint8_t bri = (uint8_t)(BRI_BASE * 0.25f + n/255.0f * (BRI_BASE * 0.25f));
        pLEDs[i] = CHSV(COULEUR_HUE - HUE_DELTA + (uint8_t)(n/255.0f*HUE_DELTA*2), 205, bri);
    }

    float starSpeed = 0.5f + vitesseEffets * 0.05f + audioLevel * 0.06f; 
    for (int i = 0; i < 80; i++) {
        stars[i].pz = stars[i].z;
        stars[i].z -= starSpeed;
        if (stars[i].z <= 0) {
            stars[i] = { (float)random8()-128.f, (float)random8()-128.f, 255.f, 255.f };
            continue;
        }
        float sx  = stars[i].x / stars[i].z  * 30.0f + MATRIX_W * 0.5f;
        float sy  = stars[i].y / stars[i].z  * 15.0f + MATRIX_H * 0.5f;
        float psx = stars[i].x / stars[i].pz * 30.0f + MATRIX_W * 0.5f;
        float psy = stars[i].y / stars[i].pz * 15.0f + MATRIX_H * 0.5f;
        if (sx < 0 || sx >= MATRIX_W || sy < 0 || sy >= MATRIX_H) continue;

        float depth = 1.0f - stars[i].z / 255.0f;
        uint8_t bri = (uint8_t)(depth * depth * 250.0f);
        uint8_t hue = (depth > 0.5f) ? COULEUR_HUE + HUE_DELTA : COULEUR_HUE;

        if (depth > 0.25f && psx >= 0 && psx < MATRIX_W && psy >= 0 && psy < MATRIX_H) {
            _drawLine((int16_t)psx, (int16_t)psy, (int16_t)sx, (int16_t)sy,
                      CHSV(hue, 220, (uint8_t)(bri * 0.38f)));
        }
        _px((int16_t)sx, (int16_t)sy) = CHSV(hue, 200, max((uint8_t)BRI_BASE, bri));
    }
}

void pharmacyCross() {
    const uint8_t GREEN = 96;
    fill_solid(pLEDs, TOTAL_LEDS, CRGB::Black);

    uint32_t tn = (uint32_t)(_t * 8.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t n = inoise8((i%MATRIX_W)*26 + tn, (i/MATRIX_W)*26);
        if (n > 210) pLEDs[i] = CHSV(GREEN, 255, (n-210)/3);
    }

    const float L = 5.0f;   
    const float W = 1.6f;   
    const float D = 1.3f;   

    float yaw   = _t * (0.6f + vitesseEffets * 0.008f);
    float pitch = sinf(_t * 0.9f) * 0.35f;
    float cy_ = cosf(yaw),  sy_ = sinf(yaw);
    float cp_ = cosf(pitch), sp_ = sinf(pitch);

    const float camZ = 9.0f;
    int16_t cX = MATRIX_W / 2;
    int16_t cY = MATRIX_H / 2;

    float lxW = 0.6f, lyW = -0.5f, lzW = -0.6f;

    static float zbuf[MAX_LEDS_EFF];
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) zbuf[i] = -1e9f;

    auto project = [&](float x, float y, float z, float& sx, float& sy, float& sz) {
        float x1 = x * cy_ + z * sy_;
        float z1 = -x * sy_ + z * cy_;
        float y1 = y;
        float y2 = y1 * cp_ - z1 * sp_;
        float z2 = y1 * sp_ + z1 * cp_;
        float x2 = x1;
        sz = z2;
        float f = camZ / (camZ + z2);
        sx = cX + x2 * f * 1.2f;
        sy = cY + y2 * f * 1.2f;
    };

    auto plotFace = [&](float x, float y, float z, float nx, float ny, float nz) {
        float nx1 = nx * cy_ + nz * sy_;
        float nz1 = -nx * sy_ + nz * cy_;
        float ny1 = ny;
        float ny2 = ny1 * cp_ - nz1 * sp_;
        float nz2 = ny1 * sp_ + nz1 * cp_;
        float nx2 = nx1;
        float diff = nx2 * lxW + ny2 * lyW + nz2 * lzW;
        if (diff < 0) diff = 0;
        float lum = 0.22f + diff * 0.78f;

        float sx, sy, sz;
        project(x, y, z, sx, sy, sz);
        int16_t ix = (int16_t)(sx + 0.5f);
        int16_t iy = (int16_t)(sy + 0.5f);
        if (ix < 0 || ix >= MATRIX_W || iy < 0 || iy >= MATRIX_H) return;
        uint16_t idx = iy * MATRIX_W + ix;
        if (sz <= zbuf[idx]) return;
        zbuf[idx] = sz;
        uint8_t bri = (uint8_t)constrain(lum * 235.0f, 20.0f, 255.0f);
        uint8_t sat = (bri > 210) ? 140 : 235;
        pLEDs[idx] = CHSV(GREEN, sat, bri);
    };

    const float step = 0.25f;
    for (float s = -L; s <= L; s += step) {
        for (float t = -W; t <= W; t += step) {
            plotFace(s, t, -D,  0, 0, -1);
            plotFace(s, t, +D,  0, 0, +1);
            plotFace(t, s, -D,  0, 0, -1);
            plotFace(t, s, +D,  0, 0, +1);
        }
    }
    for (float s = -L; s <= L; s += step) {
        for (float z = -D; z <= D; z += step) {
            plotFace(s, -W, z,  0, -1, 0);
            plotFace(s, +W, z,  0, +1, 0);
            plotFace(-W, s, z, -1, 0, 0);
            plotFace(+W, s, z, +1, 0, 0);
        }
    }
    for (float t = -W; t <= W; t += step) {
        for (float z = -D; z <= D; z += step) {
            plotFace(-L, t, z, -1, 0, 0);
            plotFace(+L, t, z, +1, 0, 0);
            plotFace(t, -L, z,  0, -1, 0);
            plotFace(t, +L, z,  0, +1, 0);
        }
    }

    float pulse = (sinf(_t * 2.0f) + 1.0f) * 0.5f;
    uint8_t boost = (uint8_t)(pulse * 20.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        if (zbuf[i] > -1e8f) pLEDs[i] += CHSV(GREEN, 200, boost);
    }
}

void synthwaveRoad() {
    const uint8_t HUE_SUN_HOT  = 20;                    
    const uint8_t HUE_CYAN     = COULEUR_HUE + 80;      
    const uint8_t HUE_PURPLE   = COULEUR_HUE + 34;
    const uint8_t HUE_NEON_PINK = COULEUR_HUE - 8;
    const uint8_t hY = MATRIX_H / 2;                    
    float beat = audioLevel / 255.0f;
    float neonDrive = 1.0f + beat * 1.4f;

    uint32_t tn = (uint32_t)(_t * 18.0f);
    for (uint8_t y = 0; y < hY; y++) {
        float f = (float)y / (float)hY;           
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            uint8_t n = inoise8(x * 20 + tn, y * 26);
            uint8_t n2 = inoise8(x * 8 - tn / 2, y * 30 + tn / 3);
            uint8_t n3 = inoise8(x * 28 + tn * 2, y * 10 - tn);
            float neb = (n - 128) / 255.0f;
            float streak = (n2 - 128) / 255.0f;
            float cloud = (n3 - 128) / 255.0f;
            uint8_t hue = HUE_PURPLE
                + (int8_t)(f * 22.0f)
                + (int8_t)(neb * 10.0f)
                + (int8_t)(streak * 8.0f)
                + (int8_t)(sinf(x * 0.25f + _t * 0.6f) * 5.0f)
                + (int8_t)(sinf((x + y) * 0.18f + _t * 2.4f * neonDrive) * 4.0f);
            float briF = 0.01f + f * f * 0.24f
                + (neb > 0 ? neb * 0.07f : 0)
                + (streak > 0 ? streak * 0.05f : 0)
                + (cloud > 0 ? cloud * 0.06f : 0)
                + beat * 0.05f;
            _px(x, y) = CHSV(hue, 240, _bri(briF));
        }
    }

    for (uint8_t i = 0; i < 22; i++) {
        uint8_t sx = (i * 37 + 11) % MATRIX_W;
        uint8_t sy = (i * 13 + 2)  % (hY > 1 ? hY - 1 : 1);
        float tw = sinf(_t * (1.5f + (i & 7) * 0.13f) + i * 0.7f);
        if (tw > 0.0f) {
            uint8_t sb = (uint8_t)(80 + tw * tw * 150);
            uint8_t sh = (i % 3 == 0) ? HUE_CYAN : (i % 3 == 1) ? HUE_NEON_PINK : (uint8_t)(HUE_SUN_HOT + 8);
            _px(sx, sy) += CHSV(sh, 60, sb);
            if (tw > 0.85f) {
                _px(sx-1, sy) += CHSV(sh, 80, 40);
                _px(sx+1, sy) += CHSV(sh, 80, 40);
                _px(sx, sy-1) += CHSV(sh, 80, 40);
                _px(sx, sy+1) += CHSV(sh, 80, 40);
            }
        }
    }

    for (uint8_t c = 0; c < 3; c++) {
        float phase = fmodf(_t * (4.5f + c * 1.1f + beat * 4.0f) + c * 4.2f, (float)(MATRIX_W + 20));
        int16_t cx = MATRIX_W - (int16_t)phase;
        int16_t cy = (int16_t)(2 + c * 3 + sinf(_t * (1.8f + c * 0.4f)) * 1.2f);
        uint8_t ch = (c == 0) ? HUE_CYAN : (c == 1) ? HUE_NEON_PINK : (uint8_t)(HUE_SUN_HOT + 6);
        for (uint8_t t = 0; t < 6; t++) {
            int16_t tx = cx + t;
            int16_t ty = cy + t / 2;
            uint8_t tb = (uint8_t)(255 - t * 40);
            _px(tx, ty) += CHSV(ch, 150, tb);
        }
    }

    int16_t sunX = MATRIX_W / 2;
    int16_t sunY = hY - 1;                   
    float pulse = (sinf(_t * 1.6f) + 1.0f) * 0.5f;
    int8_t sunR = 5;
    if (MATRIX_H >= 32) { sunR = 7; sunY = hY - 2; }

    for (int8_t dy = -sunR; dy <= sunR; dy++) {
        for (int8_t dx = -sunR; dx <= sunR; dx++) {
            float d = sqrtf((float)(dx*dx + dy*dy));
            if (d > (float)sunR) continue;
            int8_t bandY = dy + sunR;
            bool inBand = false;
            if (dy >= 0) {
                int8_t bandW = 1 + dy / 2;   
                int8_t bp = bandY % (bandW + 2);
                if (bp < bandW && dy > 0) inBand = true;
            }
            float vf = (float)(dy + sunR) / (float)(sunR * 2);   
            int16_t hueSigned = (int16_t)HUE_SUN_HOT - (int16_t)(vf * 41.0f);
            uint8_t hue = (uint8_t)((hueSigned + 256) & 0xFF);
            float g = 1.0f - d / (float)sunR;
            uint8_t bri = (uint8_t)(g * (210.0f + pulse * 45.0f));
            if (inBand) { bri = (uint8_t)(bri * 0.08f); }
            int8_t glitchX = (audioLevel > 150 && (((dy + (int16_t)(_t * 30.0f)) & 3) == 0))
                ? (int8_t)(sinf(_t * 42.0f + dy * 0.9f) * 1.5f)
                : 0;
            uint8_t gHue = hue + (audioLevel > 170 ? (uint8_t)(sinf(_t * 24.0f + dy) * 6.0f) : 0);
            _px(sunX + dx + glitchX, sunY + dy) = CHSV(gHue, 240, bri);
        }
    }
    for (int8_t dy = -sunR-2; dy <= sunR+2; dy++) {
        for (int8_t dx = -sunR-2; dx <= sunR+2; dx++) {
            float d = sqrtf((float)(dx*dx + dy*dy));
            if (d <= sunR || d > sunR + 2.5f) continue;
            float g = 1.0f - (d - sunR) / 2.5f;
            uint8_t hue = (dy < 0) ? (uint8_t)(HUE_SUN_HOT + 8) : (uint8_t)235;
            _px(sunX + dx, sunY + dy) += CHSV(hue, 230, (uint8_t)(g * (60.0f + pulse * 40.0f)));
        }
    }

    for (uint8_t x = 0; x < MATRIX_W; x++) {
        float m = sinf(x * 0.42f) * 1.3f + sinf(x * 0.19f + 1.3f) * 2.2f + cosf(x * 0.73f) * 0.8f;
        int16_t peak = hY - 1 - (int16_t)(fabsf(m));
        if (peak < 0) peak = 0;
        for (int16_t y = peak; y < hY; y++) {
            float df = (float)(y - peak) / (float)(hY - peak + 1);
            uint8_t bri = (uint8_t)(8 + df * 28);
            int16_t rel = (int16_t)x - sunX;
            if (abs(rel) <= sunR + 1 && (y >= sunY - sunR - 1 && y <= sunY + sunR + 1)) continue;
            _px(x, y) = CHSV(COULEUR_HUE - 6, 250, bri);
        }
    }

    for (uint8_t x = 0; x < MATRIX_W; x++) {
        uint8_t hue = (x & 1) ? HUE_CYAN : HUE_NEON_PINK;
        uint8_t bri = (uint8_t)(170 + ((x * 13 + (uint8_t)(_t * 20.0f * neonDrive)) & 0x0F) + beat * 70.0f);
        _px(x, hY) = CHSV(hue, 205, bri);
    }
    for (uint8_t x = 0; x < MATRIX_W; x++) {
        if (hY > 0) _px(x, hY - 1) += CHSV((x & 1) ? HUE_CYAN : HUE_NEON_PINK, 220, 50);
    }
    if (audioLevel > 145) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            _px(x, hY) += CHSV(HUE_SUN_HOT, 120, 85);
            if (hY + 1 < MATRIX_H) _px(x, hY + 1) += CHSV(HUE_SUN_HOT + 8, 170, 35);
        }
    }

    float scroll = fmodf(_t * (0.4f + vitesseEffets * 0.04f), 1.0f);
    uint8_t groundH = MATRIX_H - hY - 1;
    if (groundH == 0) groundH = 1;

    for (uint8_t y = hY + 1; y < MATRIX_H; y++) {
        float fy = (float)(y - hY) / (float)groundH;
        uint8_t bri = (uint8_t)(4 + fy * 14);
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            _px(x, y) = CHSV(COULEUR_HUE + 4, 250, bri);
        }
    }

    for (uint8_t k = 0; k < 8; k++) {
        float z = (float)k + scroll;            
        if (z < 0.15f) continue;
        float yf = (float)(hY + 1) + (float)groundH * (1.0f - 1.0f / (z * 0.45f + 1.0f));
        int16_t y = (int16_t)yf;
        if (y <= hY || y >= MATRIX_H) continue;
        float dof = 1.0f - (float)(y - hY) / (float)groundH;   
        uint8_t bri = (uint8_t)(80 + (1.0f - dof) * 150);
        uint8_t lineHue = (k & 1) ? (uint8_t)(HUE_CYAN + k * 2) : (uint8_t)(HUE_NEON_PINK + k * 2);
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            _px(x, y) = CHSV(lineHue, 220, bri);
        }
    }

    float half = (MATRIX_W - 1) * 0.5f;
    for (int8_t n = -4; n <= 4; n++) {
        if (n == 0) continue;
        float slope = (float)n * (MATRIX_W * 0.14f);
        for (uint8_t y = hY + 1; y < MATRIX_H; y++) {
            float fy = (float)(y - hY) / (float)groundH;
            float xf = half + slope * fy;
            int16_t x = (int16_t)(xf + 0.5f);
            if (x < 0 || x >= MATRIX_W) continue;
            uint8_t bri = (uint8_t)(40 + fy * fy * 170);
            uint8_t hue = (abs(n) == 1) ? HUE_NEON_PINK : ((n & 1) ? HUE_CYAN : HUE_PURPLE);
            _px(x, y) = CHSV(hue, 230, bri);
            float frac = xf - floorf(xf);
            if (frac > 0.3f && x + 1 < MATRIX_W)
                _px(x+1, y) += CHSV(hue, 230, (uint8_t)(bri * frac * 0.6f));
        }
    }

    for (uint8_t y = hY + 1; y < MATRIX_H; y++) {
        float fy = (float)(y - hY) / (float)groundH;
        int16_t lx = (int16_t)(half - half * (0.92f * fy));
        int16_t rx = (int16_t)(half + half * (0.92f * fy));
        uint8_t railHue = (uint8_t)(HUE_NEON_PINK + sinf(_t * 2.2f + fy * 5.0f) * 18.0f);
        uint8_t railBri = (uint8_t)(110 + fy * 120 + beat * 40.0f);
        _px(lx, y) += CHSV(railHue, 240, railBri);
        _px(rx, y) += CHSV(HUE_CYAN, 220, railBri);
    }

    for (uint8_t y = hY + 1; y < MATRIX_H; y++) {
        float fy = (float)(y - hY) / (float)groundH;
        int16_t rx = sunX + (int16_t)(sinf(_t * 4.0f + fy * 3.0f) * 0.6f);
        float shimmer = (sinf(_t * 10.0f + y * 1.8f) + 1.0f) * 0.5f;
        if (shimmer > 0.5f) {
            uint8_t bri = (uint8_t)((shimmer - 0.5f) * 120.0f * (1.0f - fy * 0.5f));
            uint8_t hue = HUE_SUN_HOT + (uint8_t)(fy * 40);
            _px(rx, y) += CHSV(hue, 230, bri);
        }
    }

    if (MATRIX_H >= 16) {
        int16_t bx = MATRIX_W / 2 - 2;
        int16_t by2 = MATRIX_H - 2;
        float bob = sinf(_t * 8.0f) * 0.4f;
        by2 += (int16_t)bob;

        for (int8_t dx = -3; dx <= 3; dx++) {
            CRGB& sp = _px(bx + dx, by2 + 1);
            sp.nscale8(80);
        }

        _px(bx - 2, by2)     = CHSV(HUE_CYAN, 180, 220);
        _px(bx + 2, by2)     = CHSV(HUE_CYAN, 180, 220);
        _px(bx - 1, by2)     = CHSV(COULEUR_HUE, 240, 255);
        _px(bx,     by2)     = CHSV(COULEUR_HUE, 240, 255);
        _px(bx + 1, by2)     = CHSV(COULEUR_HUE, 240, 255);
        _px(bx - 1, by2 - 1) = CHSV(HUE_SUN_HOT, 220, 220);
        _px(bx,     by2 - 1) = CHSV(HUE_SUN_HOT, 200, 255);
        _px(bx + 1, by2 - 1) = CHSV(HUE_CYAN, 230, 200);
        _px(bx, by2 - 2) = CHSV(HUE_CYAN, 240, 230);

        _px(bx - 3, by2)     += CHSV(45, 140, 255);
        if (bx - 4 >= 0) _px(bx - 4, by2) += CHSV(45, 160, 120);

        for (uint8_t i = 1; i <= 8; i++) {
            int16_t tx = bx + 2 + i;
            if (tx >= MATRIX_W) break;
            float fade = 1.0f - (float)i / 9.0f;
            uint8_t tb = (uint8_t)(fade * (180.0f + pulse * 60.0f));
            _px(tx, by2)     += CHSV(COULEUR_HUE, 240, tb);
            _px(tx, by2 - 1) += CHSV(HUE_CYAN,    240, (uint8_t)(tb * 0.5f));
        }
    }

    for (uint8_t y = 0; y < MATRIX_H; y += 2) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            CRGB& p = _px(x, y);
            p.r = scale8(p.r, 220);
            p.g = scale8(p.g, 220);
            p.b = scale8(p.b, 220);
        }
    }

    if (audioLevel > 175) {
        for (uint8_t y = 0; y < MATRIX_H; y++) {
            for (uint8_t x = 0; x < MATRIX_W; x++) {
                if (((x + y + (uint8_t)(_t * 10.0f)) & 3) == 0)
                    _px(x, y) += CHSV((uint8_t)(HUE_CYAN + x * 2), 90, 35);
            }
        }
    }
}

void nyanCat() {
    uint32_t tn = (uint32_t)(_t * 50.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t n = inoise8((i%MATRIX_W)*12+tn, (i/MATRIX_W)*12);
        pLEDs[i] = CHSV(COULEUR_HUE - HUE_DELTA, 188, _bri(n/255.0f*0.20f + 0.04f));
    }

    int16_t cx = MATRIX_W/2 + 2;
    int16_t cy = 8 + (int16_t)(sinf(_t * 2.5f) * 2.5f);

    for (int16_t trail = 1; trail <= 20; trail++) {
        int16_t tx = cx - trail - 4;
        if (tx < 0) continue;
        float wave = sinf(_t * 3.0f + trail * 0.28f);
        int16_t ty = cy + (int16_t)(wave * 2.0f);
        uint8_t hue = (uint8_t)((int16_t)COULEUR_HUE + (int8_t)((trail - 10) * (HUE_DELTA / 10)));
        uint8_t bri = (uint8_t)((1.0f - trail / 21.0f) * 225.0f + 30.0f);
        _px(tx, ty  ) += CHSV(hue, 255, bri);
        _px(tx, ty-1) += CHSV(hue, 255, (uint8_t)(bri * 0.55f));
        _px(tx, ty+1) += CHSV(hue, 255, (uint8_t)(bri * 0.55f));
    }

    for (uint8_t si = 0; si < 3; si++) {
        if (random8() < 14) {
            uint8_t spx = random8(MATRIX_W), spy = random8(MATRIX_H);
            _px(spx, spy) += CHSV(COULEUR_HUE + HUE_DELTA, 80, 210);
        }
    }

    for (int8_t dy = -1; dy <= 1; dy++)
        for (int8_t dx = -2; dx <= 2; dx++)
            _px(cx+dx, cy+dy) = _c1(195 - abs(dy)*25);
            
    _px(cx-2,cy-1)=_c0(155); _px(cx+2,cy-1)=_c0(155);
    _px(cx-2,cy+1)=_c0(155); _px(cx+2,cy+1)=_c0(155);
    _px(cx+1, cy) = CRGB::Black;
    
    bool ear = ((int)(_t * 4.0f)) % 2;
    _px(cx-1, cy-2) = ear ? _c2(215) : _c2(170);
    _px(cx+1, cy-2) = ear ? _c2(215) : _c2(170);
    int legPhase = (int)(_t * 8.0f) % 4;
    _px(cx-1, cy+2) = (legPhase < 2) ? _c0(200) : _c1(140);
    _px(cx+1, cy+2) = (legPhase >= 2) ? _c0(200) : _c1(140);
}

void spiral() {
    const float maxDist = sqrtf((MATRIX_W*0.5f)*(MATRIX_W*0.5f) + (MATRIX_H*0.5f)*(MATRIX_H*0.5f));
    float speed = 1.5f + vitesseEffets * 0.02f + audioLevel * 0.018f; 

    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        float dist = _distMap[i];
        float ang  = _angMap[i];
        float phase1 = ang - dist * 0.5f + _t * speed;
        float phase2 = -ang - dist * 0.4f - _t * speed * 0.7f;
        float v1 = sinf(phase1 * 3.0f) * 0.5f + 0.5f;
        float v2 = sinf(phase2 * 3.0f) * 0.5f + 0.5f;
        float v  = v1 * 0.62f + v2 * 0.38f;
        float prox = 1.0f - dist / maxDist;
        uint8_t hue = COULEUR_HUE
            + (int8_t)(sinf(phase1) * HUE_DELTA * 0.8f)
            + (int8_t)(sinf(phase2) * HUE_DELTA * 0.5f);
        uint8_t sat = 212 + (uint8_t)(v * 43.0f);
        pLEDs[i] = CHSV(hue, sat, _bri(v * 0.55f + prox * 0.3f + 0.08f));
    }
    float pulse = (sinf(_t * 5.0f) + 1.0f) * 0.5f;
    int16_t cx = MATRIX_W/2, cy = MATRIX_H/2;
    for (int8_t dy = -1; dy <= 1; dy++)
        for (int8_t dx = -1; dx <= 1; dx++)
            _px(cx+dx, cy+dy) += CHSV(COULEUR_HUE, 115, (uint8_t)(95 + pulse*160));
}

void pongGame() {
    fill_solid(pLEDs, TOTAL_LEDS, CHSV(COULEUR_HUE - HUE_DELTA, 188, _bri(0.07f)));
    for (uint8_t y = 0; y < MATRIX_H; y += 2) _px(MATRIX_W/2, y) = _c1(120);

    static float   paddle1Y = MATRIX_H/2, paddle2Y = MATRIX_H/2;
    static float   ballX = MATRIX_W/2, ballY = MATRIX_H/2;
    static float   ballVX = 1.2f, ballVY = 0.8f;
    static float   trailX[4] = {}, trailY[4] = {};
    static uint8_t flashTimer = 0;

    for (int8_t t = 3; t > 0; t--) { trailX[t] = trailX[t-1]; trailY[t] = trailY[t-1]; }
    trailX[0] = ballX; trailY[0] = ballY;
    for (uint8_t t = 1; t < 4; t++) {
        uint8_t tb = (4 - t) * 45;
        _px((int16_t)trailX[t], (int16_t)trailY[t]) += _c1(tb);
    }

    float dt = vitesseEffets * 0.008f; 
    ballX += ballVX * dt;
    ballY += ballVY * dt;

    if (ballY <= 0 || ballY >= MATRIX_H-1) { ballVY *= -1; flashTimer = 3; }
    if (ballX < MATRIX_W/2) paddle1Y += (ballY - paddle1Y) * 0.15f;
    else                     paddle2Y += (ballY - paddle2Y) * 0.15f;

    if (ballX <= 2 && fabsf(ballY - paddle1Y) < 4) {
        ballVX = -ballVX * 1.05f;
        ballVY += (ballY - paddle1Y) * 0.05f;
        flashTimer = 5;
    }
    if (ballX >= MATRIX_W-3 && fabsf(ballY - paddle2Y) < 4) {
        ballVX = -ballVX * 1.05f;
        ballVY += (ballY - paddle2Y) * 0.05f;
        flashTimer = 5;
    }
    if (ballVX >  3.0f) ballVX =  3.0f;
    if (ballVX < -3.0f) ballVX = -3.0f;
    if (ballVY >  2.0f) ballVY =  2.0f;
    if (ballVY < -2.0f) ballVY = -2.0f;

    if (ballX < 0 || ballX >= MATRIX_W) {
        ballX = MATRIX_W/2; ballY = MATRIX_H/2;
        ballVX = (ballVX < 0) ? 1.2f : -1.2f;
        ballVY = (random8() < 128) ? 0.8f : -0.8f;
    }

    for (int8_t h = -3; h <= 3; h++) {
        float f = 1.0f - fabsf(h) / 3.8f;
        _px(1,          (int16_t)paddle1Y+h) = CHSV(COULEUR_HUE-HUE_DELTA, 235, (uint8_t)(f*255));
        _px(MATRIX_W-2, (int16_t)paddle2Y+h) = CHSV(COULEUR_HUE+HUE_DELTA, 235, (uint8_t)(f*255));
    }
    if (flashTimer > 0) {
        uint8_t fb = flashTimer * 38;
        _px((int16_t)ballX-1, (int16_t)ballY) += CHSV(COULEUR_HUE, 200, fb);
        _px((int16_t)ballX+1, (int16_t)ballY) += CHSV(COULEUR_HUE, 200, fb);
        _px((int16_t)ballX, (int16_t)ballY-1) += CHSV(COULEUR_HUE, 200, fb);
        _px((int16_t)ballX, (int16_t)ballY+1) += CHSV(COULEUR_HUE, 200, fb);
        flashTimer--;
    }
    _px((int16_t)ballX, (int16_t)ballY) = CHSV(COULEUR_HUE, 140, 255);
}

void cube3D() {
    uint32_t tn = (uint32_t)(_t * 40.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t n = inoise8((i%MATRIX_W)*8+tn, (i/MATRIX_W)*8);
        pLEDs[i] = CHSV(COULEUR_HUE-HUE_DELTA, 228, _bri(n/255.0f*0.20f + 0.04f));
    }

    float speed = 0.5f + vitesseEffets * 0.02f;
    float aX = _t*0.5f*speed, aY = _t*0.7f*speed;
    float sX = sinf(aX), cX = cosf(aX), sY = sinf(aY), cY = cosf(aY);
    float size = 5.0f;

    float verts[8][3] = {
        {-size,-size,-size},{size,-size,-size},{size,size,-size},{-size,size,-size},
        {-size,-size, size},{size,-size, size},{size,size, size},{-size,size, size}
    };
    int16_t proj[8][2]; float depth[8];
    for (uint8_t i = 0; i < 8; i++) {
        float x=verts[i][0], y=verts[i][1], z=verts[i][2];
        float y1=y*cX-z*sX, z1=y*sX+z*cX;
        float x2=x*cY+z1*sY, z2=-x*sY+z1*cY;
        float sc = 28.0f/(28.0f+z2);
        proj[i][0] = (int16_t)(x2*sc) + MATRIX_W/2;
        proj[i][1] = (int16_t)(y1*sc) + MATRIX_H/2;
        depth[i]   = z2;
    }
    uint8_t edges[12][2] = {{0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}};
    for (uint8_t e = 0; e < 12; e++) {
        uint8_t hue = (e<4)?COULEUR_HUE-HUE_DELTA:(e<8)?COULEUR_HUE+HUE_DELTA:COULEUR_HUE;
        float d = (depth[edges[e][0]] + depth[edges[e][1]]) * 0.5f;
        uint8_t bri = (uint8_t)(175 + max(0.0f, -d) * 5.5f);
        _drawLine(proj[edges[e][0]][0], proj[edges[e][0]][1],
                  proj[edges[e][1]][0], proj[edges[e][1]][1], CHSV(hue, 238, bri));
    }
    for (uint8_t i = 0; i < 8; i++) {
        float b = (1.0f - depth[i] / (size*2.0f)) * 195.0f;
        _px(proj[i][0], proj[i][1]) += CHSV(COULEUR_HUE, 170, (uint8_t)constrain(b, 80.0f, 255.0f));
    }
}

void pyramid3D() {
    uint32_t tn = (uint32_t)(_t * 50.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t n = inoise8((i%MATRIX_W)*10+tn, (i/MATRIX_W)*10);
        pLEDs[i] = CHSV(COULEUR_HUE, 218, _bri(n/255.0f*0.22f + 0.05f));
    }

    float speed = 0.8f + vitesseEffets * 0.02f;
    float aY = _t*1.0f*speed;
    float sY = sinf(aY), cY = cosf(aY);
    float size = 6.0f;

    float verts[5][3] = {
        {0,-size*1.2f,0},
        {-size,size*0.8f,-size},{size,size*0.8f,-size},
        { size,size*0.8f, size},{-size,size*0.8f, size}
    };
    int16_t proj[5][2]; float depth[5];
    for (uint8_t i = 0; i < 5; i++) {
        float x=verts[i][0], y=verts[i][1], z=verts[i][2];
        float x2=x*cY+z*sY, z2=-x*sY+z*cY;
        float sc = 28.0f/(28.0f+z2);
        proj[i][0] = (int16_t)(x2*sc) + MATRIX_W/2;
        proj[i][1] = (int16_t)(y *sc) + MATRIX_H/2;
        depth[i]   = z2;
    }
    uint8_t edges[8][2] = {{0,1},{0,2},{0,3},{0,4},{1,2},{2,3},{3,4},{4,1}};
    for (uint8_t e = 0; e < 8; e++) {
        uint8_t hue = (e<4) ? COULEUR_HUE+HUE_DELTA : COULEUR_HUE;
        float d = (depth[edges[e][0]] + depth[edges[e][1]]) * 0.5f;
        uint8_t bri = (uint8_t)(180 + max(0.0f, -d) * 5.5f);
        _drawLine(proj[edges[e][0]][0], proj[edges[e][0]][1],
                  proj[edges[e][1]][0], proj[edges[e][1]][1], CHSV(hue, 238, bri));
    }
    for (uint8_t i = 0; i < 5; i++) {
        float b = (1.0f - depth[i] / (size*2.0f)) * 205.0f;
        uint8_t hue = (i == 0) ? COULEUR_HUE+HUE_DELTA : COULEUR_HUE;
        _px(proj[i][0], proj[i][1]) += CHSV(hue, 155, (uint8_t)constrain(b, 80.0f, 255.0f));
    }
}

void doomCorridor() {
    const uint8_t HUE_BRICK  = 12;    
    const uint8_t HUE_LAVA   = 8;     
    const uint8_t HUE_TORCH  = 22;    
    const uint8_t HUE_BLOOD  = 0;     
    const uint8_t HUE_TOXIC  = 96;    
    const uint8_t HUE_ARCANE = 168;   

    float scrollZ = _t * (0.4f + vitesseEffets * 0.035f);
    float headBob = sinf(_t * 5.0f) * 0.9f + sinf(_t * 2.3f) * 0.3f;
    float headSway = sinf(_t * 3.2f) * 0.5f;
    int16_t horizon = MATRIX_H / 2 + (int16_t)headBob;
    int16_t centerX = MATRIX_W / 2 + (int16_t)headSway;

    float globalFlicker = 0.85f + (sinf(_t * 21.0f) + sinf(_t * 37.0f + 1.2f)) * 0.075f;
    float damageFlash = audioLevel / 255.0f;
    float frenzy = 0.6f + damageFlash * 1.8f;
    float toxicPulse = (sinf(_t * 6.5f) + 1.0f) * 0.5f;

    for (int16_t y = 0; y < horizon; y++) {
        float fy = (float)(horizon - y) / (float)(horizon + 1);   
        uint8_t baseBri = (uint8_t)(4 + (1.0f - fy) * 28);         
        uint8_t beam = ((uint8_t)(scrollZ * 3.0f + fy * 6.0f)) & 3;
        uint8_t rowBri = (beam == 0) ? (uint8_t)(baseBri * 0.4f) : baseBri;
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            int16_t rx = (int16_t)x - centerX;
            float lf = 1.0f - fabsf((float)rx) / (MATRIX_W * 0.6f);
            if (lf < 0.0f) lf = 0.0f;
            uint8_t bri = (uint8_t)(rowBri * (0.4f + lf * 0.6f));
            uint8_t crack = inoise8((uint16_t)(x * 34 + (uint16_t)(scrollZ * 55.0f)), (uint16_t)(y * 46 + 123));
            uint8_t hue = HUE_BRICK;
            uint8_t sat = 220;
            if (crack > 236) {
                hue = HUE_ARCANE;
                sat = 170;
                bri = qadd8(bri, (uint8_t)((crack - 236) * 6));
            } else if (crack > 220) {
                hue = HUE_TOXIC;
                sat = 190;
                bri = qadd8(bri, (uint8_t)((crack - 220) * 3));
            }
            _px(x, y) = CHSV(hue, sat, bri);
        }
    }

    for (uint8_t x = 1; x < MATRIX_W; x += 4) {
        float dripPhase = fmodf(_t * (0.55f + (x & 3) * 0.12f) + x * 0.19f, 1.0f);
        int16_t dy = 1 + (int16_t)(dripPhase * (horizon > 2 ? (horizon - 2) : 1));
        if (((x + (uint8_t)(_t * 8.0f)) % 9) == 0) {
            _px(x, dy) += CHSV(HUE_BLOOD, 255, 95);
            _px(x, dy + 1) += CHSV(HUE_BLOOD, 255, 58);
        }
    }

    uint32_t lt = (uint32_t)(_t * 40.0f);
    for (int16_t y = horizon; y < MATRIX_H; y++) {
        float fy = (float)(y - horizon) / (float)(MATRIX_H - horizon + 1);  
        float depth = fy;                                  
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float persp = 1.0f / (1.0f - fy * 0.85f + 0.05f);
            uint8_t nx = (uint8_t)((x - centerX) * 12 * persp + lt);
            uint8_t ny = (uint8_t)(fy * 180 - lt * 2);
            uint8_t n = inoise8(nx, ny);
            uint8_t crack = inoise8(x * 40 + lt/2, y * 40);
            uint8_t vent = inoise8(x * 15 + lt * 2, y * 22 - lt);
            float lavaF = n / 255.0f;
            uint8_t hue;
            uint8_t sat = 240;
            uint8_t bri;
            if (lavaF > 0.55f) {
                float hot = (lavaF - 0.55f) / 0.45f;
                hue = HUE_LAVA + (uint8_t)(hot * 15);
                bri = (uint8_t)(60 + hot * 180 * depth + 30);
            } else {
                hue = HUE_BRICK;
                bri = (uint8_t)(8 + lavaF * 40 * depth);
            }
            if (crack > 200) {
                bri = qadd8(bri, (uint8_t)((crack - 200) * 2 * depth));
                hue = HUE_LAVA;
            }
            if (vent > 236) {
                hue = (uint8_t)(HUE_TOXIC + (vent & 0x07));
                sat = 195;
                bri = qadd8(bri, (uint8_t)((vent - 236) * 5));
            }
            _px(x, y) = CHSV(hue, sat, (uint8_t)(bri * globalFlicker));
        }
    }

    float halfW = MATRIX_W * 0.5f;
    for (uint8_t x = 0; x < MATRIX_W; x++) {
        float dx = ((float)x - centerX) / halfW;   
        float adx = fabsf(dx);
        if (adx < 0.02f) continue;                  
        float wallHalfH = (MATRIX_H * 0.5f) * adx;
        int16_t wTop = horizon - (int16_t)wallHalfH;
        int16_t wBot = horizon + (int16_t)wallHalfH;
        if (wTop < 0) wTop = 0;
        if (wBot >= MATRIX_H) wBot = MATRIX_H - 1;

        float depth = 1.0f - adx;     
        float zCoord = 1.0f / (adx + 0.05f);
        float brickU = (zCoord * 1.2f + scrollZ * 4.0f);  
        uint8_t brickCol = ((uint32_t)brickU) & 7;

        for (int16_t y = wTop; y <= wBot; y++) {
            int16_t dy = y - horizon;
            float v = (float)(dy + (int16_t)wallHalfH) / (float)(wallHalfH * 2.0f + 0.001f);
            uint8_t brickRow = (uint8_t)(v * 6.0f);
            uint8_t rowOffset = (brickRow & 1) ? 3 : 0;
            uint8_t cell = (brickCol + rowOffset) & 7;

            bool joint = (cell == 0) || (brickRow > 0 && ((uint8_t)(v * 6.0f * 2) & 7) == 0);
            uint8_t noise = inoise8((uint8_t)(brickU * 8), (uint8_t)(v * 120));
            float textureF = 0.7f + (noise / 255.0f) * 0.3f;

            float lightF = depth * depth * globalFlicker;
            uint8_t bri = (uint8_t)(lightF * 200.0f * textureF);
            if (joint) bri = (uint8_t)(bri * 0.25f);

            uint8_t hue = HUE_BRICK + (noise > 180 ? 4 : 0);
            uint8_t sat = 230;

            uint8_t runeHash = (uint8_t)(brickU * 10.0f) + (uint8_t)y + (uint8_t)(scrollZ * 20.0f);
            bool rune = !joint && depth > 0.2f && (runeHash % 17 == 0);
            if (rune) {
                hue = ((x + y) & 1) ? HUE_TOXIC : HUE_ARCANE;
                sat = 200;
                bri = qadd8(bri, 85);
            }

            if (!joint && cell == 4 && ((((uint8_t)(zCoord * 3.0f)) + (uint8_t)y) & 7) == 0) {
                hue = HUE_ARCANE;
                sat = 170;
                bri = qadd8(bri, 45);
            }

            if (!joint && depth > 0.45f) {
                uint8_t sig = (uint8_t)(x * 3 + y * 7 + (uint8_t)(scrollZ * 22.0f));
                if ((sig & 0x1F) == 0) {
                    hue = (sig & 0x20) ? HUE_BLOOD : HUE_ARCANE;
                    sat = 225;
                    bri = qadd8(bri, (uint8_t)(35 * frenzy));
                }
            }
            _px(x, y) = CHSV(hue, sat, bri);
        }

        if (wTop > 0)   _px(x, wTop) = CHSV(HUE_BRICK, 255, 6);
        if (wBot < MATRIX_H-1) _px(x, wBot) = CHSV(HUE_BRICK, 255, 6);
    }

    for (uint8_t x = 0; x < MATRIX_W; x++) {
        float dx = ((float)x - centerX) / halfW;
        float adx = fabsf(dx);
        if (adx < 0.02f) continue;
        float wallHalfH = (MATRIX_H * 0.5f) * adx;
        int16_t wBot = horizon + (int16_t)wallHalfH;
        for (int8_t k = 0; k < 3; k++) {
            int16_t yy = wBot - k;
            if (yy < 0 || yy >= MATRIX_H) continue;
            float glow = (1.0f - k / 3.0f) * (1.0f - adx) * 0.7f;
            _px(x, yy) += CHSV(HUE_LAVA, 240, (uint8_t)(glow * 120));
        }
    }

    {
        float demonPulse = (sinf(_t * 1.3f) + 1.0f) * 0.5f;
        int16_t dcx = centerX;
        int16_t dcy = horizon;
        int16_t eyeDx = 1;
        float eyePulse = (sinf(_t * 4.0f) + 1.0f) * 0.5f;
        uint8_t eyeBri = (uint8_t)(100 + eyePulse * 155);
        _px(dcx - eyeDx, dcy - 1) += CHSV(HUE_BLOOD, 255, eyeBri);
        _px(dcx + eyeDx, dcy - 1) += CHSV(HUE_ARCANE, 220, eyeBri);
        
        for (int8_t dy = -2; dy <= 1; dy++) {
            for (int8_t dx = -2; dx <= 2; dx++) {
                if (dy == -1 && abs(dx) == eyeDx) continue;
                int16_t px_ = dcx + dx;
                int16_t py_ = dcy + dy;
                if (px_ < 0 || px_ >= MATRIX_W || py_ < 0 || py_ >= MATRIX_H) continue;
                CRGB& p = _px(px_, py_);
                p.nscale8(90);
            }
        }
        _px(dcx, dcy) += CHSV(HUE_BLOOD, 240, (uint8_t)(demonPulse * 22));
        _px(dcx, dcy) += CHSV(HUE_ARCANE, 190, (uint8_t)(demonPulse * 45));
    }

    {
        float portal = (sinf(_t * 2.1f) + 1.0f) * 0.5f;
        int16_t px0 = centerX;
        int16_t py0 = horizon;
        for (int8_t r = 2; r <= 4; r++) {
            uint8_t rb = (uint8_t)(25 + portal * (70 - r * 8) + damageFlash * 45.0f);
            uint8_t rh = (r & 1) ? HUE_ARCANE : HUE_TOXIC;
            for (uint16_t a = 0; a < 360; a += 24) {
                float ang = a * (PI / 180.0f);
                _px((int16_t)(px0 + cosf(ang) * r),
                    (int16_t)(py0 + sinf(ang) * (r * 0.6f))) += CHSV(rh, 210, rb);
            }
        }
    }

    struct Torch { int8_t side; float zWorld; };
    const Torch torches[] = {
        {-1, 1.5f}, {+1, 2.8f}, {-1, 4.2f}, {+1, 5.6f}, {-1, 7.0f}, {+1, 8.4f}
    };
    const uint8_t NT = 6;
    for (uint8_t i = 0; i < NT; i++) {
        float z = fmodf(torches[i].zWorld + 10.0f - scrollZ * 2.0f, 10.0f);
        if (z < 0.3f || z > 9.0f) continue;

        float adx = 1.0f / (z * 0.5f + 1.0f);   
        if (adx > 0.95f) continue;
        int16_t tx = centerX + (int16_t)(adx * halfW * torches[i].side);
        float wallHalfH = (MATRIX_H * 0.5f) * adx;
        int16_t ty = horizon - (int16_t)(wallHalfH * 0.5f);   
        if (ty < 0 || ty >= MATRIX_H) continue;

        float flick = (sinf(_t * 22.0f + i * 2.1f) + sinf(_t * 35.0f + i * 1.3f) + sinf(_t * 13.0f + i)) * 0.22f + 0.78f;
        float depth = 1.0f - adx;
        uint8_t baseBri = (uint8_t)(flick * 240.0f * (0.5f + depth * 0.5f));

        if (tx >= 0 && tx < MATRIX_W) {
            _px(tx, ty + 1) = CHSV(HUE_BRICK, 255, 20);
        }

        int8_t flameH = (depth > 0.4f) ? 3 : 2;
        for (int8_t fy = 0; fy < flameH; fy++) {
            int16_t fpy = ty - fy;
            if (fpy < 0 || fpy >= MATRIX_H) continue;
            uint8_t hue = (fy == 0) ? ((i & 1) ? HUE_TOXIC : 50) : (fy == 1 ? HUE_TORCH : HUE_LAVA);
            uint8_t sat = (fy == 0) ? ((i & 1) ? 180 : 150) : 240;
            uint8_t fb = (uint8_t)(baseBri * (1.0f - fy * 0.25f));
            _px(tx, fpy) = CHSV(hue, sat, fb);
            if (fy <= 1 && depth > 0.3f) {
                float jitter = sinf(_t * 30.0f + i + fy * 2) * 0.5f;
                int16_t jx = (jitter > 0) ? tx + 1 : tx - 1;
                _px(jx, fpy) += CHSV(hue, sat, (uint8_t)(fb * 0.45f));
            }
        }

        for (int8_t dy = -3; dy <= 3; dy++) {
            for (int8_t dx = -3; dx <= 3; dx++) {
                float d = sqrtf((float)(dx*dx + dy*dy));
                if (d < 0.5f || d > 3.5f) continue;
                float g = 1.0f - d / 3.5f;
                g *= g;
                int16_t hx = tx + dx;
                int16_t hy = ty + dy;
                if (hx < 0 || hx >= MATRIX_W || hy < 0 || hy >= MATRIX_H) continue;
                uint8_t hb = (uint8_t)(g * baseBri * 0.4f);
                _px(hx, hy) += CHSV(HUE_TORCH, 220, hb);
            }
        }

        if ((i & 1) == 1) {
            for (int8_t dy = -2; dy <= 1; dy++) {
                int16_t fy = ty + dy;
                if (fy < 0 || fy >= MATRIX_H) continue;
                uint8_t fb = (uint8_t)((2 - abs(dy)) * 28 * (0.5f + toxicPulse * 0.5f));
                _px(tx - 1, fy) += CHSV(HUE_TOXIC, 180, fb);
                _px(tx + 1, fy) += CHSV(HUE_TOXIC, 180, fb);
            }
        }

        for (uint8_t s = 0; s < 2; s++) {
            float sph = _t * 6.0f + i * 3.3f + s * 1.7f;
            float sp  = fmodf(sph, 1.5f);
            int16_t spy = ty - (int16_t)(sp * 4.0f);
            int16_t spx = tx + (int16_t)(sinf(sph * 3.0f) * 1.5f);
            if (spy >= 0 && spy < MATRIX_H && spx >= 0 && spx < MATRIX_W && sp < 1.2f) {
                uint8_t sb = (uint8_t)((1.2f - sp) * 150 * depth);
                _px(spx, spy) += CHSV(HUE_TORCH + 10, 200, sb);
            }
        }
    }

    float cxF = (MATRIX_W - 1) * 0.5f;
    float cyF = (MATRIX_H - 1) * 0.5f;
    float maxD = sqrtf(cxF*cxF + cyF*cyF);
    for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float dx = (float)x - cxF;
            float dy = (float)y - cyF;
            float d = sqrtf(dx*dx + dy*dy) / maxD;
            if (d > 0.65f) {
                float vign = 1.0f - (d - 0.65f) / 0.45f;
                if (vign < 0.35f) vign = 0.35f;
                CRGB& p = _px(x, y);
                p.nscale8((uint8_t)(vign * 255));
            }
        }
    }

    if (damageFlash > 0.3f) {
        uint8_t add = (uint8_t)((damageFlash - 0.3f) * 120);
        for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
            pLEDs[i] += CHSV(HUE_BLOOD, 240, add);
        }
        if (damageFlash > 0.62f) {
            uint8_t addArc = (uint8_t)((damageFlash - 0.62f) * 180);
            for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
                pLEDs[i] += CHSV(HUE_ARCANE, 220, addArc);
            }
        }
    }
}

void pacMan() {
    fill_solid(pLEDs, TOTAL_LEDS, CHSV(COULEUR_HUE - HUE_DELTA, 218, _bri(0.04f)));

    float speed  = 0.9f + vitesseEffets * 0.06f;
    float cycle  = MATRIX_W + 14.0f;
    float pacFX  = fmodf(_t * speed, cycle);
    int16_t px   = (int16_t)pacFX - 2;
    int16_t py   = MATRIX_H / 2;

    for (uint8_t x = 2; x < MATRIX_W; x += 4) {
        if (x > px + 2) _px(x, py) = CHSV(COULEUR_HUE, 220, 190);
    }

    float mouth = (sinf(_t * 10.0f) + 1.0f) * 0.24f + 0.06f;
    for (int8_t dy = -2; dy <= 2; dy++) {
        for (int8_t dx = -2; dx <= 2; dx++) {
            float dist = sqrtf((float)(dx*dx + dy*dy));
            if (dist > 2.5f) continue;
            if (fabsf(atan2f((float)dy, (float)dx)) < mouth) continue;
            _px(px+dx, py+dy) = CHSV(COULEUR_HUE, 215, 200 + (uint8_t)((1.0f-dist/2.5f)*55));
        }
    }
    _px(px, py-1) = CHSV(COULEUR_HUE - HUE_DELTA, 220, 255); 

    int16_t gx = px - 9;
    for (int8_t dy = -2; dy <= 2; dy++) {
        for (int8_t dx = -2; dx <= 2; dx++) {
            if (abs(dx)==2 && abs(dy)==2) continue;
            if (dy==2 && (dx==-1 || dx==1)) continue; 
            float bf = 1.0f - fabsf((float)dy) / 3.5f;
            _px(gx+dx, py+dy) = CHSV(COULEUR_HUE+HUE_DELTA, 240, (uint8_t)(140+bf*100));
        }
    }
    _px(gx-1, py-1) = CHSV(COULEUR_HUE-HUE_DELTA, 60, 255);
    _px(gx+1, py-1) = CHSV(COULEUR_HUE-HUE_DELTA, 60, 255);

    int16_t gx2 = px - 17;
    for (int8_t dy = -2; dy <= 2; dy++) {
        for (int8_t dx = -2; dx <= 2; dx++) {
            if (abs(dx)==2 && abs(dy)==2) continue;
            if (dy==2 && (dx==-1 || dx==1)) continue;
            float bf = 1.0f - fabsf((float)dy) / 3.5f;
            _px(gx2+dx, py+dy) = CHSV(COULEUR_HUE, 240, (uint8_t)(115+bf*80));
        }
    }
    _px(gx2-1, py-1) = CHSV(COULEUR_HUE-HUE_DELTA, 60, 255);
    _px(gx2+1, py-1) = CHSV(COULEUR_HUE-HUE_DELTA, 60, 255);
}

void heartbeat() {
    static float   ecgBuf[MATRIX_W_MAX] = {};
    static uint8_t wPos             = 0;
    static float   qrsPhase         = -1.0f;
    static uint8_t autoTimer        = 0;
    static float   lastAudio        = 0.0f;

    float audioN = audioLevel / 255.0f;
    bool trigger = (audioN > 0.35f && lastAudio < 0.2f);
    lastAudio = audioN;

    if (!trigger) {
        autoTimer++;
        uint8_t period = (uint8_t)map(vitesseEffets, 1, 100, 55, 12); 
        if (autoTimer >= period) { autoTimer = 0; trigger = true; }
    }
    if (trigger && qrsPhase < 0.0f) qrsPhase = 0.0f;

    float sample = 0.0f;
    if (qrsPhase >= 0.0f) {
        float amp = 0.55f + audioN * 0.45f;
        if      (qrsPhase < 0.08f)  sample =  qrsPhase/0.08f * 0.25f;
        else if (qrsPhase < 0.18f)  sample =  0.25f + (qrsPhase-0.08f)/0.1f * amp;
        else if (qrsPhase < 0.30f)  sample =  amp   - (qrsPhase-0.18f)/0.12f * amp * 1.8f;
        else if (qrsPhase < 0.48f)  sample = -amp*0.4f + (qrsPhase-0.30f)/0.18f * amp*0.55f;
        else if (qrsPhase < 0.70f)  sample =  (qrsPhase-0.48f)/0.22f * 0.30f;
        else                        sample =  0.30f * (1.0f - (qrsPhase-0.70f)/0.30f);
        qrsPhase += 0.055f;
        if (qrsPhase > 1.0f) qrsPhase = -1.0f;
    }

    ecgBuf[wPos] = sample;
    wPos = (wPos + 1) % MATRIX_W;

    float heartGlow = (qrsPhase >= 0.0f && qrsPhase < 0.5f)
        ? sinf(qrsPhase / 0.5f * 3.14159f) : 0.0f;
    uint8_t bgBri = (uint8_t)(6.0f + heartGlow * 28.0f);
    fill_solid(pLEDs, TOTAL_LEDS, CHSV(COULEUR_HUE - HUE_DELTA, 240, bgBri));

    for (uint8_t x = 0; x < MATRIX_W; x += 4)
        for (uint8_t y = 0; y < MATRIX_H; y += 4)
            _px(x, y) += CHSV(COULEUR_HUE, 200, 18 + (uint8_t)(heartGlow * 12.0f));

    int16_t baseline = MATRIX_H / 2 + 1;
    for (uint8_t x = 0; x < MATRIX_W; x++)
        _px(x, baseline) += CHSV(COULEUR_HUE, 180, 35);

    for (uint8_t x = 0; x < MATRIX_W; x++) {
        float s = ecgBuf[(wPos + x) % MATRIX_W];
        int16_t y = constrain((int16_t)(baseline - s*(MATRIX_H*0.42f)), 0, MATRIX_H-1);
        float amp = fabsf(s);
        _px(x, y) = CHSV(COULEUR_HUE, 225, 130 + (uint8_t)(amp*125));
        if (amp > 0.08f) {
            _px(x, y-1) += CHSV(COULEUR_HUE, 210, (uint8_t)(amp*85));
            _px(x, y+1) += CHSV(COULEUR_HUE, 210, (uint8_t)(amp*85));
        }
    }
}

void gameOfLife() {
    static bool    grid[MAX_LEDS_EFF] = {};
    static bool    nxt [MAX_LEDS_EFF] = {};
    static uint8_t age [MAX_LEDS_EFF] = {};
    static uint8_t  stagnation = 0;
    static uint32_t lastStep   = 0;

    if (!_golInit) {
        for (uint16_t i = 0; i < TOTAL_LEDS; i++) { grid[i] = random8() < 85; age[i] = 0; }
        stagnation = 0;
        _golInit = true;
    }

    uint32_t tn = (uint32_t)(_t * 6.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t x = i % MATRIX_W, y = i / MATRIX_W;
        uint8_t n = inoise8(x * 25 + tn, y * 25);
        uint8_t bri = (n > 200) ? (uint8_t)((n-200)/55.0f * 10.0f) : 3;
        pLEDs[i] = CHSV(COULEUR_HUE - HUE_DELTA, 220, bri);
    }

    uint32_t interval = (uint32_t)map(vitesseEffets, 1, 100, 320, 45);
    if (millis() - lastStep > interval) {
        lastStep = millis();
        uint8_t changes = 0;
        for (uint8_t y = 0; y < MATRIX_H; y++) {
            for (uint8_t x = 0; x < MATRIX_W; x++) {
                uint8_t n = 0;
                for (int8_t dy = -1; dy <= 1; dy++)
                    for (int8_t dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        n += grid[((y+dy+MATRIX_H)%MATRIX_H)*MATRIX_W + ((x+dx+MATRIX_W)%MATRIX_W)];
                    }
                bool alive = grid[y*MATRIX_W+x];
                bool next  = alive ? (n==2||n==3) : (n==3);
                nxt[y*MATRIX_W+x] = next;
                if (next != alive) changes++;
            }
        }
        for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
            age[i]  = nxt[i] ? qadd8(age[i], 14) : 0;
            grid[i] = nxt[i];
        }
        stagnation = (changes < 5) ? qadd8(stagnation, 1) : 0;
        if (stagnation > 7) _golInit = false;
    }

    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        if (grid[i]) {
            float mat = age[i] / 255.0f;
            _px(i%MATRIX_W, i/MATRIX_W) = CHSV(
                COULEUR_HUE + (int8_t)(mat*HUE_DELTA*1.5f),
                240,
                80 + (uint8_t)(mat*175)
            );
        }
    }
}

void dna() {
    fill_solid(pLEDs, TOTAL_LEDS, CHSV(COULEUR_HUE - HUE_DELTA, 215, _bri(0.04f)));

    float scroll = _t * (vitesseEffets * 0.007f + 0.05f) * 9.0f;

    for (uint8_t x = 0; x < MATRIX_W; x++) {
        float phase = x * 0.42f - scroll;
        float amp   = MATRIX_H / 2.0f - 1.5f;
        float y1f   = MATRIX_H / 2.0f + sinf(phase) * amp;
        float y2f   = MATRIX_H / 2.0f - sinf(phase) * amp;
        int16_t y1  = constrain((int16_t)y1f, 0, MATRIX_H-1);
        int16_t y2  = constrain((int16_t)y2f, 0, MATRIX_H-1);

        _px(x, y1) = CHSV(COULEUR_HUE, 240, 245);
        _px(x, constrain(y1+1, 0, MATRIX_H-1)) += CHSV(COULEUR_HUE, 230, 65);

        _px(x, y2) = CHSV(COULEUR_HUE + HUE_DELTA, 240, 245);
        _px(x, constrain(y2-1, 0, MATRIX_H-1)) += CHSV(COULEUR_HUE + HUE_DELTA, 230, 65);

        if (fabsf(cosf(phase)) < 0.30f) {
            int16_t yFrom = min(y1, y2), yTo = max(y1, y2);
            for (int16_t y = yFrom; y <= yTo; y++) {
                float mid = (yTo>yFrom) ? fabsf((float)(y-yFrom)/(yTo-yFrom) - 0.5f) : 0.0f;
                _px(x, y) += CHSV(COULEUR_HUE, 200, (uint8_t)((0.5f-mid)*2.0f*150));
            }
        }
    }
}

void rain() {
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) { 
        pLEDs[i].nscale8(208); 
        pLEDs[i] += CHSV(COULEUR_HUE-HUE_DELTA, 215, 5); 
    }

    struct Drop { uint8_t x; float y, vy; bool alive; };
    static Drop drops[24] = {};
    if (!_rainInit) { memset(drops, 0, sizeof(drops)); _rainInit = true; }

    uint8_t spawnRate = (uint8_t)constrain(vitesseEffets/2 + map(audioLevel, 0, 255, 0, 80), 0, 200);
    if (random8() < spawnRate) {
        for (uint8_t i = 0; i < 24; i++) {
            if (!drops[i].alive) { drops[i] = { random8(MATRIX_W), 0.0f, 0.12f+random8(10)*0.05f, true }; break; }
        }
    }

    for (uint8_t i = 0; i < 24; i++) {
        if (!drops[i].alive) continue;
        drops[i].vy = min(drops[i].vy + 0.07f, 2.2f);
        drops[i].y += drops[i].vy;

        if (drops[i].y >= MATRIX_H - 1) {
            uint8_t sx = drops[i].x;
            for (int8_t dx = -3; dx <= 3; dx++) {
                float f = 1.0f - fabsf(dx) / 3.6f;
                _px(sx+dx, MATRIX_H-1) += CHSV(COULEUR_HUE, 215, (uint8_t)(f*215));
                if (abs(dx) >= 2) _px(sx+dx, MATRIX_H-2) += CHSV(COULEUR_HUE, 200, (uint8_t)(f*75));
            }
            drops[i].alive = false;
            continue;
        }

        float tailLen = 1.5f + drops[i].vy * 0.8f;
        for (uint8_t t = 0; t <= (uint8_t)tailLen; t++) {
            int16_t ty = (int16_t)drops[i].y - t;
            if (ty < 0) break;
            float b = (1.0f - t/(tailLen+1)) * (1.0f - t/(tailLen+1));
            _px(drops[i].x, ty) += CHSV(COULEUR_HUE, 210, (uint8_t)(b*235));
        }
    }
}

void audioVisualizer() {
    static uint8_t barH    [MATRIX_W_MAX] = {};
    static uint8_t peakY   [MATRIX_W_MAX] = {};
    static uint8_t peakTmr [MATRIX_W_MAX] = {};

    static float colProfile[MATRIX_W_MAX];
    if (!_profInit) {
        for (uint8_t x = 0; x < MATRIX_W_MAX; x++) {
            float norm    = (float)x / (MATRIX_W_MAX - 1);
            float bass    = 0.55f + 0.45f * sinf(norm * 3.14159f);
            colProfile[x] = bass * ((x & 1) ? 0.82f : 1.0f);
        }
        _profInit = true;
    }

    float audioN = audioLevel / 255.0f;

    uint8_t bgBri = (uint8_t)(audioN * audioN * 55.0f); 
    uint8_t breathe = (uint8_t)(sinf(_t * 1.2f) * 0.5f + 0.5f) * 12;
    bgBri = qadd8(bgBri, breathe);

    for (uint8_t y = 0; y < MATRIX_H; y++) {
        for (uint8_t x = 0; x < MATRIX_W; x++) {
            float yRatio = (float)(MATRIX_H - 1 - y) / (float)(MATRIX_H - 1);
            uint8_t pxBri = (uint8_t)(bgBri * (0.3f + 0.7f * (1.0f - yRatio)));
            _px(x, y) = CHSV(COULEUR_HUE, 255, pxBri);
        }
    }

    for (uint8_t x = 0; x < MATRIX_W; x++) {
        uint8_t colNoise = inoise8(x * 42, (uint32_t)(_t * 18.0f));
        float noiseMod   = 0.78f + colNoise / 255.0f * 0.44f;

        uint8_t targetH = (uint8_t)(audioN * MATRIX_H * colProfile[x] * noiseMod);

        if (targetH > barH[x])   barH[x] = targetH;
        else if (barH[x] > 0)    barH[x]--;

        if (barH[x] >= peakY[x]) {
            peakY[x]   = barH[x];
            peakTmr[x] = 18;
        } else if (peakTmr[x] > 0) {
            peakTmr[x]--;
        } else if (peakY[x] > 0) {
            peakY[x]--;
        }

        for (uint8_t h = 0; h < barH[x]; h++) {
            uint8_t y     = MATRIX_H - 1 - h;
            float   ratio = (float)h / (float)(MATRIX_H - 1);
            uint8_t hue   = (uint8_t)(COULEUR_HUE + ratio * 18.0f); 
            uint8_t sat   = (uint8_t)(255.0f - ratio * 30.0f);      
            uint8_t bri   = (uint8_t)(100.0f + ratio * 155.0f);     
            _px(x, y) = CHSV(hue, sat, bri);
        }

        if (peakY[x] > 0 && peakY[x] < MATRIX_H) {
            _px(x, MATRIX_H - 1 - peakY[x]) = CHSV((uint8_t)(COULEUR_HUE + 22), 200, 255);
        }
    }
}

void lightning() {
    struct Drop { int8_t x; int8_t y; uint8_t len; uint8_t spd; };
    static Drop drops[28];
    static uint8_t flash         = 0;
    static uint8_t boltCool      = 40;
    static int8_t  boltPath[MATRIX_H_MAX] = {};
    static uint8_t boltFrames    = 0;
    static uint8_t boltMaxY      = 0;
    static uint8_t boltBranchY   = 0;
    static int8_t  boltBranchDir = 0;
    static uint8_t boltBranchLen = 0;

    if (!_dropsInit) {
        for (uint8_t i = 0; i < 28; i++) {
            drops[i].x   = random8(MATRIX_W_MAX);
            drops[i].y   = random8(MATRIX_H_MAX);
            drops[i].len = 2 + random8(3);
            drops[i].spd = 1 + random8(2);
        }
        _dropsInit = true;
    }

    uint32_t tn = (uint32_t)(_t * 12.0f);
    for (uint16_t i = 0; i < TOTAL_LEDS; i++) {
        uint8_t x = i % MATRIX_W, y = i / MATRIX_W;
        uint8_t n = inoise8(x * 18 + tn, y * 28);
        float fy = (float)y / MATRIX_H;
        uint8_t base = (uint8_t)(n * 0.06f + 4);            
        uint8_t fl = (uint8_t)(flash * (0.35f + fy * 0.65f)); 
        pLEDs[i] = CHSV(165, 200 - (uint8_t)(fl > 100 ? 100 : fl), qadd8(base, fl));
    }

    for (uint8_t i = 0; i < 28; i++) {
        Drop& d = drops[i];
        for (uint8_t k = 0; k < d.len; k++) {
            int16_t px = d.x - k;      
            int16_t py = d.y - k * 2;
            if (px < 0 || py < 0 || px >= MATRIX_W || py >= MATRIX_H) continue;
            uint8_t db = 160 - k * 45;
            _px(px, py) += CHSV(160, 120, db);
        }
        d.y += d.spd;
        d.x += 1; 
        if (d.y >= MATRIX_H + d.len * 2 || d.x >= MATRIX_W + d.len) {
            d.x = random8(MATRIX_W);
            d.y = 0;
            d.len = 2 + random8(3);
            d.spd = 1 + random8(2);
        }
    }

    if (boltFrames > 0) {
        uint8_t alpha = (boltFrames >= 3) ? 255 : (uint8_t)(boltFrames * 85);
        for (uint8_t y = 0; y <= boltMaxY && y < MATRIX_H; y++) {
            int8_t bx = boltPath[y];
            if (bx < 0 || bx >= MATRIX_W) continue;
            _px(bx, y) = CHSV(170, 20, alpha);
            if (bx-1 >= 0)       _px(bx-1, y) += CHSV(170, 150, (uint8_t)(alpha * 0.5f));
            if (bx+1 < MATRIX_W) _px(bx+1, y) += CHSV(170, 150, (uint8_t)(alpha * 0.5f));
            if (bx-2 >= 0)       _px(bx-2, y) += CHSV(170, 180, (uint8_t)(alpha * 0.2f));
            if (bx+2 < MATRIX_W) _px(bx+2, y) += CHSV(170, 180, (uint8_t)(alpha * 0.2f));
        }
        if (boltBranchLen > 0) {
            int16_t bbx = boltPath[boltBranchY];
            for (uint8_t k = 0; k < boltBranchLen; k++) {
                bbx += boltBranchDir;
                int16_t bby = boltBranchY + k;
                if (bbx < 0 || bbx >= MATRIX_W || bby >= MATRIX_H) break;
                _px(bbx, bby) = CHSV(170, 30, (uint8_t)(alpha * 0.8f));
                if (bbx+1 < MATRIX_W) _px(bbx+1, bby) += CHSV(170, 160, (uint8_t)(alpha * 0.3f));
                if (bbx-1 >= 0)       _px(bbx-1, bby) += CHSV(170, 160, (uint8_t)(alpha * 0.3f));
            }
        }
        boltFrames--;
    } else if (boltCool > 0) {
        boltCool--;
    } else if (random8() < 8) {
        int16_t bx = 3 + random8(MATRIX_W > 6 ? MATRIX_W - 6 : 1);
        boltMaxY = MATRIX_H - 1;
        for (uint8_t y = 0; y < MATRIX_H; y++) {
            boltPath[y] = (int8_t)bx;
            int r = random8();
            if (r < 80)      bx -= 1;
            else if (r < 160) bx += 1;
            if (bx < 1) bx = 1;
            if (bx > MATRIX_W - 2) bx = MATRIX_W - 2;
        }
        boltBranchY   = 3 + random8(MATRIX_H > 8 ? MATRIX_H - 8 : 1);
        boltBranchDir = (random8() < 128) ? -1 : 1;
        boltBranchLen = 3 + random8(5);
        boltFrames    = 5 + random8(4);
        flash         = 220;
        boltCool      = 35 + random8(80);
    }
    flash = (uint8_t)(flash * 0.78f);
}

void dvdBounce() {
    static const uint8_t LOGO_W = 11;
    static const uint8_t LOGO_H = 5;
    static const uint16_t logo[5] = {
        0b11001010110, 
        0b10101010101, 
        0b10101010101, 
        0b10100100101, 
        0b11000100110, 
    };

    static float dvdX = 2.0f, dvdY = 2.0f;
    static float dvdVX = 1.0f, dvdVY = 1.0f; 
    static uint8_t dvdHue = 0;
    static uint8_t flashTimer = 0;
    static float subStep = 0.0f; 

    float speed = 0.04f + vitesseEffets * 0.0018f;
    subStep += speed;

    bool moved = false;
    while (subStep >= 1.0f) {
        subStep -= 1.0f;
        dvdX += dvdVX;
        dvdY += dvdVY;
        moved = true;

        bool hitX = false, hitY = false;
        if (dvdX <= 0)                 { dvdX = 0; dvdVX = 1; hitX = true; }
        if (dvdX >= MATRIX_W - LOGO_W) { dvdX = MATRIX_W - LOGO_W; dvdVX = -1; hitX = true; }
        if (dvdY <= 0)                 { dvdY = 0; dvdVY = 1; hitY = true; }
        if (dvdY >= MATRIX_H - LOGO_H) { dvdY = MATRIX_H - LOGO_H; dvdVY = -1; hitY = true; }

        if (hitX && hitY) { dvdHue += 50; flashTimer = 10; }
        else if (hitX || hitY) dvdHue += 23;
    }
    (void)moved;

    fill_solid(pLEDs, TOTAL_LEDS, CRGB::Black);

    if (flashTimer > 0) {
        uint8_t fb = flashTimer * 12;
        for (uint16_t i = 0; i < TOTAL_LEDS; i++)
            pLEDs[i] = CHSV(dvdHue, 80, fb);
        flashTimer--;
    }

    int16_t lx = (int16_t)dvdX;
    int16_t ly = (int16_t)dvdY;
    for (uint8_t row = 0; row < LOGO_H; row++) {
        uint16_t bits = logo[row];
        for (uint8_t col = 0; col < LOGO_W; col++) {
            if (bits & (1 << (LOGO_W - 1 - col))) {
                uint8_t bri = 240;
                if (row == 0) bri = 255;
                else if (row == LOGO_H - 1) bri = 200;
                _px(lx + col, ly + row) = CHSV(dvdHue, 230, bri);
            }
        }
    }
}

// --- interface publique ---
typedef void (*EffectFn)();
static const EffectFn _table[COUNT] = {
    solidPulse,     fire,           meteorRain,     twinkle,
    plasma,         bouncingBalls,  ripple,         wave,
    scanner,        sparkle,        matrixRain,     vortex,
    colorSweep,     lavaLamp,       aurora,         starfield,
    pharmacyCross,  synthwaveRoad,  nyanCat,
    spiral,         pongGame,       cube3D,         pyramid3D,
    doomCorridor,   audioVisualizer,
    pacMan,         heartbeat,      gameOfLife,     dna,        rain,
    lightning,      dvdBounce,
};

static const char* _names[COUNT] = {
    "Pulse Analogue",   "Feu de Teinte",    "Météores",         "Etoiles",
    "Plasma",           "Balles Rebonds",   "Ondulations",      "Vagues",
    "Scanner",          "Sparkle",          "Matrix Rain",      "Vortex",
    "Balayage",         "Lampe à Lave",     "Aurore",           "Voyage 3D",
    "Croix Pharmacie",  "Moto Synthwave",   "Nyan Cat",         "Spirale",
    "Pong Game",        "Cube 3D",          "Pyramide 3D",      "Couloir Doom",
    "Audio Visu",
    "Pac-Man",          "Heartbeat",        "Game of Life",     "ADN",      "Pluie",
    "Orage",            "DVD"
};

void run(uint8_t idx) {
    _t += vitesseEffets * 0.002f;
    if (idx < COUNT) _table[idx]();
}

const char* name(uint8_t idx) { return (idx < COUNT) ? _names[idx] : "?"; }

} // namespace effects