#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_log.h>

#include <Adafruit_GFX.h>
#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoOblique12pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBoldOblique12pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Fonts/FreeSerifItalic12pt7b.h>
#include <Fonts/FreeSerifBoldItalic12pt7b.h>
#include <Fonts/Picopixel.h>
#include <Fonts/TomThumb.h>
#include <Fonts/Org_01.h>

#include "effects.h"
#include <FastLED.h>

// --- config matrice ---

static constexpr uint8_t  PIN_DATA    = 13;
static constexpr uint8_t  MIC_PIN     = 4;
static constexpr bool     MIC_ENABLED = false;
static constexpr uint8_t  TILE_W      = 16;  // un panneau WS2812B fait 16 colonnes
static constexpr uint8_t  TILE_H      = 16;  // et 16 lignes (256 LEDs par tuile)
// MAX_LEDS est la capacité maximale du tableau : 4 tuiles (config 1×4 ou 2×2).
// On alloue toujours pour le pire cas afin d'éviter les réallocations dynamiques.
static constexpr uint16_t MAX_LEDS    = TILE_W * TILE_H * 4;

static constexpr uint32_t DUREE_EFFET_MS = 15000;  // durée avant rotation automatique des effets
#ifdef WOKWI_SIM
static constexpr uint8_t  TARGET_FPS  = 10;  // FPS réduit sur Wokwi : le simulateur est lent
#else
static constexpr uint8_t  TARGET_FPS  = 30;
#endif
static constexpr uint32_t FRAME_DELAY = 1000 / TARGET_FPS;  // période d'une frame en ms

// --- topologie ---

// Décrit l'arrangement physique des panneaux : nombre de tuiles en largeur × hauteur.
enum class Topologie : uint8_t { T1x1 = 0, T1x2 = 1, T1x4 = 2, T2x2 = 3 };

uint8_t   matW        = 32;
uint8_t   matH        = 16;
uint16_t  NUM_LEDS    = 32 * 16;
Topologie topoActuelle = Topologie::T1x2;

void applyTopologie(Topologie t) {
    topoActuelle = t;
    switch (t) {
        case Topologie::T1x1: matW = 16; matH = 16; break;
        case Topologie::T1x2: matW = 32; matH = 16; break;
        case Topologie::T1x4: matW = 64; matH = 16; break;
        case Topologie::T2x2: matW = 32; matH = 32; break;
    }
    NUM_LEDS = static_cast<uint16_t>(matW) * matH;
}

// --- réseau & état global ---

WebServer server(80);
String    webSerialBuffer;

// Mutex FreeRTOS qui protège tous les buffers partagés entre la tâche LED (cœur 1)
// et la tâche web (cœur 0). Sans lui, une requête HTTP qui modifie leds[] pendant
// un FastLED.show() provoquerait des corruptions visuelles aléatoires.
SemaphoreHandle_t matrixMutex;

String  messageGlobal    = "La Ruche";
uint8_t COULEUR_HUE      = 120;
int     vitesseEffets    = 50;
#ifdef WOKWI_SIM
uint8_t luminosite       = 255;
#else
uint8_t luminosite       = 80;
#endif
bool    effetAutomatique = true;
uint8_t effetManuel      = 0;

uint32_t lastFrameTime   = 0;
bool     modeDessin      = false;
volatile bool pendingClearAll = false;

// --- buffers LED ---
// Pipeline à trois étages :
//   leds[]          ← les effets écrivent ici (espace "logique" XY linéaire)
//   finalBuffer[]   ← compositeFrame() superpose le texte/masque par-dessus leds[]
//   hardwareBuffer[]← flushToDisplay() réordonne finalBuffer[] selon le câblage serpentin
// Cette séparation permet de modifier les effets et le texte indépendamment,
// sans jamais écrire directement dans l'ordre physique des LEDs.

CRGB    leds         [MAX_LEDS];
CRGB    finalBuffer  [MAX_LEDS];
CRGB    hardwareBuffer[MAX_LEDS];
uint8_t textMask     [MAX_LEDS];  // 1 = pixel de texte/dessin actif, 0 = fond

CRGB* pLEDs = leds;

volatile uint8_t audioLevel = 0;

CRGB    cachedTextColor    = CRGB::White;
CRGB    cachedOutlineColor = CRGB::Black;
uint8_t lastHueForText     = 255;

// Choisit automatiquement texte noir ou blanc selon la luminance perçue de la teinte.
// La pondération ITU-R BT.601 (299/587/114) reflète la sensibilité de l'œil humain.
void updateTextColors() {
    if (COULEUR_HUE == lastHueForText) return;  // recalcul uniquement si la teinte a changé
    lastHueForText = COULEUR_HUE;
    CRGB base = CHSV(COULEUR_HUE, 240, 200);
    uint32_t luma = static_cast<uint32_t>(base.r) * 299u
                  + static_cast<uint32_t>(base.g) * 587u
                  + static_cast<uint32_t>(base.b) * 114u;
    bool useDark = (luma > 127500u);
    cachedTextColor    = useDark ? CRGB::Black : CRGB::White;
    cachedOutlineColor = useDark ? CRGB::White : CRGB::Black;
}

// --- mapping XY ---
// Traduit une coordonnée (x, y) logique en index physique dans le tableau de LEDs.
// Les panneaux WS2812B 16×16 sont câblés en serpentin COLONNE par colonne :
// la colonne 0 part du haut, la colonne 1 repart du bas, etc.
// Ce câblage est dicté par le fabricant du panneau et non modifiable.
// En cas de coordonnées hors-limites, on retourne MAX_LEDS (index "poubelle" ignoré).

#ifdef WOKWI_SIM
// Wokwi : ordre linéaire ligne par ligne, gauche→droite
uint16_t XY(uint8_t x, uint8_t y) {
    if (x >= MATRIX_W || y >= MATRIX_H) return MAX_LEDS;
    return static_cast<uint16_t>(y) * MATRIX_W + x;
}
#else
// Hardware réel : serpentin colonne par colonne
// Colonnes paires haut→bas, colonnes impaires bas→haut
uint16_t XY(uint8_t x, uint8_t y) {
    if (x >= MATRIX_W || y >= MATRIX_H) return MAX_LEDS;

    const uint8_t  lx  = x % TILE_W;          // colonne locale dans la tuile
    const uint8_t  ly  = y % TILE_H;          // ligne locale dans la tuile
    const uint8_t  px  = x / TILE_W;          // indice de tuile en X
    const uint8_t  py  = y / TILE_H;          // indice de tuile en Y
    const uint8_t  tpr = MATRIX_W / TILE_W;   // nombre de tuiles par rangée
    const uint16_t panelOffset = (static_cast<uint16_t>(py) * tpr + px) * (TILE_W * TILE_H);
    // Inversion de direction sur les colonnes impaires : c'est le cœur du serpentin
    const uint16_t localIdx    = (lx & 1u) ? (lx * TILE_H + (TILE_H - 1u - ly))
                                            : (lx * TILE_H + ly);
    return panelOffset + localIdx;
}
#endif


// --- texte GFX ---

class MatrixTextGFX : public Adafruit_GFX {
public:
    MatrixTextGFX() : Adafruit_GFX(64, 32) {}

    void resize(uint8_t w, uint8_t h) { _width = w; _height = h; }

    void drawPixel(int16_t x, int16_t y, uint16_t color) override {
        if (static_cast<uint16_t>(x) < MATRIX_W && static_cast<uint16_t>(y) < MATRIX_H)
            textMask[static_cast<uint16_t>(y) * MATRIX_W + x] = color ? 1u : 0u;
    }
};

MatrixTextGFX gfxText;

struct FontDef { const char* name; const GFXfont* font; int baselineOffset; };

static const FontDef FONTS[] = {
    {"Standard 5x7",    nullptr,         4},
    {"Micro Pixel",     &Picopixel,     10},
    {"Tom Thumb",       &TomThumb,      10},
    {"Arcade",          &Org_01,        10},
    {"Terminal",        &FreeMono9pt7b, 13},
    {"Clean",           &FreeSans9pt7b, 13},
    {"Gros (Déborde)",  &FreeSans12pt7b,16},
};
static constexpr uint8_t NUM_FONTS = sizeof(FONTS) / sizeof(FONTS[0]);

uint8_t  policeActuelle  = 0;
int16_t  textX           = 32;
int16_t  textWidth       = 0;
uint32_t lastScrollUpdate = 0;
bool     texteEnCours    = false;

void measureText() {
    gfxText.setFont(FONTS[policeActuelle].font);
    int16_t  x1, y1;
    uint16_t w, h;
    gfxText.getTextBounds(messageGlobal, 0, 0, &x1, &y1, &w, &h);
    textWidth = w ? static_cast<int16_t>(w) : static_cast<int16_t>(messageGlobal.length() * 12);
}

uint8_t  effetActuel       = 0;
uint32_t dernierChangement = 0;


// --- serial web ---

void logMessage(const char* msg) {
    Serial.printf("[%lu ms] %s\n", millis(), msg);
    if (webSerialBuffer.length() < 2000)
        webSerialBuffer += "[" + String(millis() / 1000) + "s] " + msg + "<br>";
    else
        webSerialBuffer = webSerialBuffer.substring(1000);
}


// --- page web ---

const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="fr">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no, viewport-fit=cover">
  <title>La Ruche Matrix</title>
  <style>
    :root { --bg: #000000; --glass: rgba(20, 20, 20, 0.45); --glass-hover: rgba(35, 35, 35, 0.6); --glass-border: rgba(255, 255, 255, 0.06); --glass-border-light: rgba(255, 255, 255, 0.15); --text: #ffffff; --text-muted: #999999; --r-xl: 32px; --r-lg: 20px; --r-md: 12px; --r-sm: 8px; }
    * { box-sizing: border-box; margin: 0; padding: 0; }
    html { -webkit-text-size-adjust: 100%; scroll-behavior: smooth; }
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Helvetica, Arial, sans-serif; background: var(--bg); color: var(--text); min-height: 100dvh; display: flex; flex-direction: column; align-items: center; padding: env(safe-area-inset-top, 40px) 20px calc(env(safe-area-inset-bottom, 0px) + 60px); position: relative; overflow-x: hidden; }
    .bg-blobs { position: fixed; inset: -10%; z-index: 0; pointer-events: none; background: #020202; filter: blur(60px); }
    .blob { position: absolute; border-radius: 50%; background: rgba(255, 255, 255, 0.08); animation: float 20s infinite alternate cubic-bezier(0.45, 0.05, 0.55, 0.95); will-change: transform; }
    .blob-1 { top: 10%; left: 10%; width: 50vw; height: 50vw; background: rgba(255,255,255,0.06); } .blob-2 { top: 40%; right: 10%; width: 60vw; height: 60vw; animation-delay: -5s; animation-direction: alternate-reverse; background: rgba(255,255,255,0.04); } .blob-3 { bottom: 10%; left: 30%; width: 45vw; height: 45vw; animation-delay: -10s; background: rgba(255,255,255,0.05); }
    @keyframes float { 0% { transform: translate(0, 0) scale(1); } 50% { transform: translate(15vw, 10vh) scale(1.1); } 100% { transform: translate(-10vw, 20vh) scale(0.9); } }
    .bg-overlay { position: fixed; inset: 0; z-index: 1; background: rgba(0, 0, 0, 0.5); pointer-events: none; }
    .page { position: relative; z-index: 10; width: 100%; max-width: 500px; display: flex; flex-direction: column; gap: 24px; }
    header { display: flex; flex-direction: column; align-items: center; justify-content: center; margin-bottom: 20px; animation: fadeInDown 0.8s ease-out; }
    @keyframes fadeInDown { from { opacity: 0; transform: translateY(-30px); } to { opacity: 1; transform: translateY(0); } }
    .logo-container { width: 100px; height: 110px; display: flex; align-items: center; justify-content: center; margin-bottom: 16px; animation: pulseLogo 4s ease-in-out infinite alternate; }
    @keyframes pulseLogo { 0% { filter: drop-shadow(0 0 10px rgba(255,255,255,0.1)); transform: scale(1); } 100% { filter: drop-shadow(0 0 25px rgba(255,255,255,0.4)); transform: scale(1.03); } }
    .logo-container svg { width: 100%; height: 100%; }
    .header-info { text-align: center; } .header-info h1 { font-size: 2.2rem; font-weight: 800; letter-spacing: 0.1em; text-transform: uppercase; margin-bottom: 4px; } .header-meta { font-size: 0.85rem; color: var(--text-muted); font-weight: 300; letter-spacing: 0.05em; }
    .card { background: var(--glass); backdrop-filter: blur(40px) saturate(150%); -webkit-backdrop-filter: blur(40px) saturate(150%); border: 1px solid var(--glass-border); border-top: 1px solid rgba(255, 255, 255, 0.15); border-left: 1px solid rgba(255, 255, 255, 0.1); border-radius: var(--r-xl); padding: 28px; box-shadow: 0 30px 60px rgba(0,0,0,0.6), inset 0 1px 0 rgba(255,255,255,0.1); transition: transform 0.3s ease; animation: fadeInUp 0.6s backwards; }
    .card:nth-child(2) { animation-delay: 0.1s; } .card:nth-child(3) { animation-delay: 0.2s; } .card:nth-child(4) { animation-delay: 0.3s; }
    @keyframes fadeInUp { from { opacity: 0; transform: translateY(20px); } to { opacity: 1; transform: translateY(0); } }
    .sec-label { font-size: 0.7rem; font-weight: 800; text-transform: uppercase; letter-spacing: 0.2em; color: var(--text-muted); margin-bottom: 20px; display: flex; align-items: center; gap: 10px; } .sec-label::after { content: ""; flex: 1; height: 1px; background: linear-gradient(90deg, var(--glass-border-light), transparent); }
    .tabs { display: flex; background: rgba(0,0,0,0.4); border-radius: var(--r-md); padding: 6px; margin-bottom: 24px; border: 1px solid var(--glass-border); box-shadow: inset 0 2px 10px rgba(0,0,0,0.5); }
    .tab-btn { flex: 1; text-align: center; padding: 12px; font-weight: 600; font-size: 0.85rem; color: var(--text-muted); cursor: pointer; border-radius: var(--r-sm); transition: 0.3s cubic-bezier(0.4, 0, 0.2, 1); }
    .tab-btn.active { background: var(--text); color: var(--bg); box-shadow: 0 4px 15px rgba(255,255,255,0.2); }
    .pane { display: none; } .pane.active { display: block; animation: fadeIn 0.4s ease-out; } @keyframes fadeIn { from { opacity: 0; } to { opacity: 1; } }
    .msg-row { display: flex; gap: 12px; margin-bottom:12px; } input[type=text] { flex: 1; min-width: 0; background: rgba(0,0,0,0.4); border: 1px solid var(--glass-border); border-radius: var(--r-md); color: var(--text); padding: 16px; font-size: 1rem; outline: none; transition: 0.3s; box-shadow: inset 0 2px 5px rgba(0,0,0,0.5); } input[type=text]:focus { border-color: rgba(255,255,255,0.5); background: rgba(0,0,0,0.6); }
    .btn-action { background: var(--text); color: var(--bg); font-weight: 800; border: none; border-radius: var(--r-md); padding: 0 24px; cursor: pointer; transition: 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275); } .btn-action:hover { transform: scale(1.05); box-shadow: 0 0 15px rgba(255,255,255,0.3); }
    .btn-reset { width: 100%; padding: 16px; background: rgba(255,255,255,0.06); border: 1px solid var(--glass-border-light); border-radius: var(--r-lg); color: var(--text); font-size: 0.95rem; font-weight: 700; cursor: pointer; transition: 0.3s; letter-spacing: 0.05em; display: flex; align-items: center; justify-content: center; gap: 10px; } .btn-reset:hover { background: var(--text); color: var(--bg); box-shadow: 0 4px 15px rgba(255,255,255,0.2); }
    .select-font { width: 100%; background: rgba(0,0,0,0.4); border: 1px solid var(--glass-border); border-radius: var(--r-md); color: var(--text); padding: 12px 16px; font-size: 0.9rem; font-weight: 600; outline: none; transition: 0.3s; box-shadow: inset 0 2px 5px rgba(0,0,0,0.5); appearance: none; cursor: pointer; }
    .select-font:focus { border-color: rgba(255,255,255,0.5); }
    .canvas-container { width: 100%; background: #000; border-radius: var(--r-lg); margin-bottom: 16px; overflow: hidden; border: 1px solid var(--glass-border); box-shadow: inset 0 0 40px rgba(0,0,0,0.8), 0 10px 30px rgba(0,0,0,0.5); }
    #liveCanvas, #drawCanvas { width: 100%; height: 100%; image-rendering: pixelated; display: block; }
    #drawCanvas { cursor: crosshair; touch-action: none; }
    .draw-tools { display: flex; gap: 10px; } .tool-btn { flex: 1; background: rgba(255,255,255,0.03); color: var(--text-muted); border: 1px solid var(--glass-border); border-top: 1px solid rgba(255,255,255,0.1); border-radius: var(--r-md); padding: 12px; font-size: 0.85rem; font-weight: 600; cursor: pointer; transition: 0.2s; box-shadow: 0 4px 10px rgba(0,0,0,0.2); } .tool-btn.active { background: var(--text); color: var(--bg); border-color: var(--text); } .tool-btn:hover:not(.active) { color: var(--text); border-color: var(--glass-border-light); background: rgba(255,255,255,0.08); }
    .slider-row { margin-bottom: 30px; } .slider-row:last-child { margin-bottom: 0; } .slider-meta { display: flex; justify-content: space-between; align-items: center; margin-bottom: 16px; } .slider-name { font-size: 0.95rem; font-weight: 600; color: var(--text); display: flex; align-items: center; gap: 8px;} .slider-val { font-family: ui-monospace, "SF Mono", Consolas, "Courier New", monospace; font-size: 0.85rem; font-weight: 700; background: rgba(255,255,255,0.1); padding: 4px 10px; border-radius: var(--r-sm); border: 1px solid rgba(255,255,255,0.05); }
    input[type=range] { -webkit-appearance: none; width: 100%; height: 10px; border-radius: 5px; background: rgba(0,0,0,0.5); outline: none; cursor: pointer; box-shadow: inset 0 2px 5px rgba(0,0,0,0.8), 0 1px 0 rgba(255,255,255,0.1); } input[type=range]::-webkit-slider-thumb { -webkit-appearance: none; width: 28px; height: 28px; border-radius: 50%; background: radial-gradient(circle at center, #ffffff 25%, rgba(255,255,255,0.6) 35%, rgba(255,255,255,0.15) 100%); backdrop-filter: blur(4px); border: 1px solid rgba(255, 255, 255, 0.7); box-shadow: 0 4px 12px rgba(0,0,0,0.6), inset 0 2px 4px rgba(255,255,255,0.8); cursor: pointer; transition: transform 0.2s cubic-bezier(0.175, 0.885, 0.32, 1.275), box-shadow 0.2s; } input[type=range]::-webkit-slider-thumb:hover { transform: scale(1.15); box-shadow: 0 6px 16px rgba(0,0,0,0.8), inset 0 2px 6px rgba(255,255,255,1); border-color: #ffffff; }
    #hue { background: linear-gradient(to right, hsl(0,90%,60%), hsl(30,90%,60%), hsl(60,90%,60%), hsl(90,90%,60%), hsl(120,90%,60%), hsl(150,90%,60%), hsl(180,90%,60%), hsl(210,90%,60%), hsl(240,90%,60%), hsl(270,90%,60%), hsl(300,90%,60%), hsl(330,90%,60%), hsl(360,90%,60%)); box-shadow: 0 2px 10px rgba(0,0,0,0.5); } .color-presets { display: flex; gap: 8px; justify-content: space-between; margin-bottom: 16px; } .c-btn { width: 32px; height: 32px; border-radius: 50%; cursor: pointer; border: 2px solid rgba(255,255,255,0.2); transition: 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275); box-shadow: 0 4px 10px rgba(0,0,0,0.3); } .c-btn:hover { transform: scale(1.2); border-color: #fff; box-shadow: 0 6px 15px rgba(0,0,0,0.5); }
    #lum.unlocked { background: linear-gradient(to right, rgba(255,255,255,0.4) 0%, rgba(255,255,255,0.4) 70%, transparent 70%, transparent 100%), repeating-linear-gradient(45deg, rgba(255,255,255,0.1), rgba(255,255,255,0.1) 10px, rgba(0,0,0,0.6) 10px, rgba(0,0,0,0.6) 20px); } .unlock-btn { background: rgba(255,255,255,0.05); border: 1px solid rgba(255,255,255,0.15); color: var(--text-muted); font-size: 0.7rem; font-weight: 700; padding: 4px 10px; border-radius: 12px; cursor: pointer; transition: 0.3s; text-transform: uppercase; letter-spacing: 0.05em; } .unlock-btn:hover { background: rgba(255,255,255,0.15); color: var(--text); border-color: rgba(255,255,255,0.4); }
    .fx-top { display: flex; align-items: center; justify-content: space-between; margin-bottom: 20px; } .toggle-wrap { display: flex; align-items: center; gap: 10px; cursor: pointer; user-select: none; } .toggle-label { font-size: 0.85rem; font-weight: 600; color: var(--text-muted); transition: 0.2s;} .toggle-wrap:hover .toggle-label { color: var(--text); } .ios-pill { width: 48px; height: 26px; border-radius: 13px; position: relative; background: rgba(0,0,0,0.6); border: 1px solid var(--glass-border); transition: 0.3s; box-shadow: inset 0 2px 5px rgba(0,0,0,0.5); } .ios-pill.on { background: #fff; border-color: #fff; box-shadow: 0 0 10px rgba(255,255,255,0.3); } .ios-knob { position: absolute; top: 2px; left: 2px; width: 20px; height: 20px; border-radius: 50%; background: #888; transition: 0.3s cubic-bezier(0.4, 0, 0.2, 1); box-shadow: 0 2px 4px rgba(0,0,0,0.4); } .ios-pill.on .ios-knob { transform: translateX(22px); background: #000; }
    .fx-grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; } @media(min-width: 420px) { .fx-grid { grid-template-columns: repeat(4, 1fr); } } .eff-btn { background: rgba(255,255,255,0.03); border: 1px solid var(--glass-border); border-radius: var(--r-md); color: var(--text-muted); font-size: 0.75rem; font-weight: 600; padding: 14px 6px; text-align: center; cursor: pointer; transition: 0.2s; } .eff-btn:hover { background: rgba(255,255,255,0.08); color: var(--text); border-color: var(--glass-border-light); } .eff-btn.active { background: var(--text); border-color: var(--text); color: var(--bg); font-weight: 800; box-shadow: 0 4px 15px rgba(255,255,255,0.2); }
    .topo-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
    .topo-btn { background: rgba(255,255,255,0.03); border: 1px solid var(--glass-border); border-radius: var(--r-md); color: var(--text-muted); font-size: 0.8rem; font-weight: 700; padding: 14px 8px; text-align: center; cursor: pointer; transition: 0.2s; display: flex; flex-direction: column; align-items: center; gap: 8px; }
    .topo-btn:hover { background: rgba(255,255,255,0.08); color: var(--text); border-color: var(--glass-border-light); }
    .topo-btn.active { background: var(--text); border-color: var(--text); color: var(--bg); box-shadow: 0 4px 15px rgba(255,255,255,0.2); }
    .topo-preview { display: inline-flex; flex-direction: column; gap: 2px; }
    .topo-row { display: flex; gap: 2px; align-items: flex-start; }
    .topo-panel { background: currentColor; border-radius: 2px; opacity: 0.6; flex-shrink: 0; }
    .topo-btn.active .topo-panel { opacity: 1; }
    .topo-info { font-size: 0.65rem; opacity: 0.7; font-weight: 400; }
    .modal-overlay { position: fixed; inset: 0; background: rgba(0,0,0,0.7); backdrop-filter: blur(15px); display: flex; align-items: center; justify-content: center; z-index: 100; opacity: 0; pointer-events: none; transition: 0.3s ease; padding: 20px; } .modal-overlay.active { opacity: 1; pointer-events: all; } .modal { background: rgba(15, 15, 15, 0.85); border: 1px solid rgba(255,255,255,0.15); border-top: 1px solid rgba(255,255,255,0.3); border-radius: var(--r-xl); padding: 32px; width: 100%; max-width: 360px; text-align: center; transform: scale(0.9); transition: 0.3s cubic-bezier(0.175, 0.885, 0.32, 1.275); box-shadow: 0 30px 60px rgba(0,0,0,0.8); } .modal-overlay.active .modal { transform: scale(1); } .modal-icon { margin-bottom: 20px; display: flex; justify-content: center; } .modal-icon svg { width: 48px; height: 48px; color: #fff; filter: drop-shadow(0 0 10px rgba(255,255,255,0.3)); } .modal h3 { font-size: 1.3rem; margin-bottom: 12px; font-weight: 800; text-transform: uppercase; letter-spacing: 0.05em; } .modal p { font-size: 0.9rem; color: var(--text-muted); line-height: 1.6; margin-bottom: 28px; } .modal-actions { display: flex; gap: 12px; } .btn-modal { flex: 1; padding: 14px; border-radius: var(--r-md); font-weight: 700; cursor: pointer; border: none; transition: 0.2s; } .btn-cancel { background: rgba(255,255,255,0.08); color: #fff; border: 1px solid rgba(255,255,255,0.1); } .btn-cancel:hover { background: rgba(255,255,255,0.15); } .btn-confirm { background: #fff; color: #000; } .btn-confirm:hover { transform: scale(1.05); box-shadow: 0 0 20px rgba(255,255,255,0.4); }
    .footer { text-align: center; margin-top: 20px; font-size: 0.75rem; color: rgba(255,255,255,0.4); letter-spacing: 0.05em; line-height: 1.6; } .footer strong { color: rgba(255,255,255,0.8); font-weight: 600; }
    #status { position: fixed; top: 20px; left: 50%; transform: translateX(-50%); background: rgba(20,20,20,0.8); backdrop-filter: blur(10px); border: 1px solid rgba(255,255,255,0.15); padding: 10px 20px; border-radius: 20px; font-size: 0.8rem; font-weight: 600; color: #fff; z-index: 50; opacity: 0; transition: 0.3s; pointer-events: none; box-shadow: 0 10px 20px rgba(0,0,0,0.5); } #status.show { opacity: 1; transform: translateX(-50%) translateY(10px); }
  </style>
</head>
<body>
<div class="bg-blobs"><div class="blob blob-1"></div><div class="blob blob-2"></div><div class="blob blob-3"></div></div>
<div class="bg-overlay"></div>
<div id="status">Connecté</div>
<div class="page">
  <header>
    <div class="logo-container">
      <svg viewBox="0 0 100 112" fill="none" xmlns="http://www.w3.org/2000/svg">
        <polygon points="50,14 86.3,35 86.3,77 50,98 13.7,77 13.7,35" stroke="white" stroke-width="3" stroke-linejoin="round"/>
        <polygon points="50,42 62.1,49 62.1,63 50,70 37.9,63 37.9,49" stroke="white" stroke-width="2"/>
        <polygon points="74.2,42 86.3,49 86.3,63 74.2,70 62.1,63 62.1,49" stroke="white" stroke-width="2"/>
        <polygon points="62.1,21 74.2,28 74.2,42 62.1,49 50,42 50,28" stroke="white" stroke-width="2"/>
        <polygon points="37.9,21 50,28 50,42 37.9,49 25.8,42 25.8,28" stroke="white" stroke-width="2"/>
        <polygon points="25.8,42 37.9,49 37.9,63 25.8,70 13.7,63 13.7,49" stroke="white" stroke-width="2"/>
        <polygon points="37.9,63 50,70 50,84 37.9,91 25.8,84 25.8,70" stroke="white" stroke-width="2"/>
        <polygon points="62.1,63 74.2,70 74.2,84 62.1,91 50,84 50,70" stroke="white" stroke-width="2"/>
      </svg>
    </div>
    <div class="header-info"><h1>La Ruche</h1><div class="header-meta">MATRICE LED INTERACTIVE</div></div>
  </header>

  <button class="btn-reset" onclick="resetAuto()">
    <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M23 4v6h-6"></path><path d="M1 20v-6h6"></path><path d="M3.51 9a9 9 0 0114.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0020.49 15"></path></svg>
    Mode Automatique
  </button>

  <div class="card">
    <div class="fx-top" style="margin-bottom:16px;">
      <div class="sec-label" style="margin:0;">Visualisation</div>
      <div class="toggle-wrap" onclick="toggleLive()"><span class="toggle-label">Direct</span><div class="ios-pill" id="livePill"><div class="ios-knob"></div></div></div>
    </div>
    <div class="canvas-container" id="liveContainer"><canvas id="liveCanvas"></canvas></div>
  </div>

  <div class="card">
    <div class="sec-label">Configuration des panneaux</div>
    <div class="topo-grid">
      <button class="topo-btn" id="topo-0" onclick="setTopo(0)">
        <div class="topo-preview"><div class="topo-row"><div class="topo-panel" style="width:20px;height:20px;"></div></div></div>
        1×1<span class="topo-info">16×16 px · 1 panneau</span>
      </button>
      <button class="topo-btn" id="topo-1" onclick="setTopo(1)">
        <div class="topo-preview"><div class="topo-row"><div class="topo-panel" style="width:10px;height:10px;"></div><div class="topo-panel" style="width:10px;height:10px;"></div></div></div>
        1×2<span class="topo-info">32×16 px · 2 panneaux</span>
      </button>
      <button class="topo-btn" id="topo-2" onclick="setTopo(2)">
        <div class="topo-preview"><div class="topo-row"><div class="topo-panel" style="width:10px;height:10px;"></div><div class="topo-panel" style="width:10px;height:10px;"></div><div class="topo-panel" style="width:10px;height:10px;"></div><div class="topo-panel" style="width:10px;height:10px;"></div></div></div>
        1×4<span class="topo-info">64×16 px · 4 panneaux</span>
      </button>
      <button class="topo-btn" id="topo-3" onclick="setTopo(3)">
        <div class="topo-preview"><div class="topo-row"><div class="topo-panel" style="width:10px;height:10px;"></div><div class="topo-panel" style="width:10px;height:10px;"></div></div><div class="topo-row" style="margin-top:2px;"><div class="topo-panel" style="width:10px;height:10px;"></div><div class="topo-panel" style="width:10px;height:10px;"></div></div></div>
        2×2<span class="topo-info">32×32 px · 4 panneaux</span>
      </button>
    </div>
  </div>

  <div class="card">
    <div class="tabs">
      <div id="btn-txt" class="tab-btn active" onclick="switchTab('txt')">Texte Défilant</div>
      <div id="btn-draw" class="tab-btn" onclick="switchTab('draw')">Dessin Libre</div>
    </div>
    <div id="pane-txt" class="pane active">
      <div class="msg-row">
        <input type="text" id="msg" placeholder="Entrez un message..." maxlength="80">
        <button class="btn-action" onclick="sendMsg()">OK</button>
      </div>
      <select id="fontSel" class="select-font" onchange="sendParam('font', this.value)">
        <option value="0">Chargement des polices...</option>
      </select>
    </div>
    <div id="pane-draw" class="pane">
      <div class="canvas-container" id="drawContainer" style="margin-bottom:12px;"><canvas id="drawCanvas"></canvas></div>
      <div class="draw-tools">
        <button id="tool-brush" class="tool-btn active" onclick="setTool(1)"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:middle; margin-right:4px;"><path d="M12 19l7-7 3 3-7 7-3-3z"></path><path d="M18 13l-1.5-7.5L2 2l3.5 14.5L13 18l5-5z"></path><path d="M2 2l7.586 7.586"></path><circle cx="11" cy="11" r="2"></circle></svg> Tracer</button>
        <button id="tool-eraser" class="tool-btn" onclick="setTool(0)"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:middle; margin-right:4px;"><rect x="4" y="4" width="16" height="16" rx="2" ry="2"></rect><path d="M4 12h16"></path></svg> Gommer</button>
        <button class="tool-btn" onclick="clearCanvas()" style="flex:0.5; padding:12px 0;"><svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" style="vertical-align:middle;"><polyline points="3 6 5 6 21 6"></polyline><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6m3 0V4a2 2 0 0 1 2-2h4a2 2 0 0 1 2 2v2"></path></svg></button>
      </div>
      <button class="btn-action" onclick="sendRucheLogo()" style="width:100%;margin-top:10px;display:flex;align-items:center;justify-content:center;gap:10px;">
        <svg width="22" height="24" viewBox="0 0 100 112" fill="none" xmlns="http://www.w3.org/2000/svg" style="flex-shrink:0">
          <polygon points="50,14 86.3,35 86.3,77 50,98 13.7,77 13.7,35" stroke="currentColor" stroke-width="5" stroke-linejoin="round"/>
          <polygon points="50,42 62.1,49 62.1,63 50,70 37.9,63 37.9,49" stroke="currentColor" stroke-width="4"/>
          <polygon points="74.2,42 86.3,49 86.3,63 74.2,70 62.1,63 62.1,49" stroke="currentColor" stroke-width="4"/>
          <polygon points="62.1,21 74.2,28 74.2,42 62.1,49 50,42 50,28" stroke="currentColor" stroke-width="4"/>
          <polygon points="37.9,21 50,28 50,42 37.9,49 25.8,42 25.8,28" stroke="currentColor" stroke-width="4"/>
          <polygon points="25.8,42 37.9,49 37.9,63 25.8,70 13.7,63 13.7,49" stroke="currentColor" stroke-width="4"/>
          <polygon points="37.9,63 50,70 50,84 37.9,91 25.8,84 25.8,70" stroke="currentColor" stroke-width="4"/>
          <polygon points="62.1,63 74.2,70 74.2,84 62.1,91 50,84 50,70" stroke="currentColor" stroke-width="4"/>
        </svg>
        Afficher logo La Ruche
      </button>
    </div>
  </div>

  <div class="card">
    <div class="sec-label">Paramètres</div>
    <div class="slider-row"><div class="slider-meta"><span class="slider-name">Teinte</span><span class="slider-val" id="hueVal">120</span></div><div class="color-presets"><div class="c-btn" style="background:#ff3b3b;" onclick="setHue(0)"></div><div class="c-btn" style="background:#ffdd00;" onclick="setHue(64)"></div><div class="c-btn" style="background:#00ff88;" onclick="setHue(96)"></div><div class="c-btn" style="background:#00ddff;" onclick="setHue(128)"></div><div class="c-btn" style="background:#4d6fff;" onclick="setHue(160)"></div><div class="c-btn" style="background:#ff00ff;" onclick="setHue(224)"></div></div><input type="range" id="hue" min="0" max="255" value="120" oninput="hueVal.textContent=this.value" onchange="sendParam('hue',this.value)"></div>
    <div class="slider-row"><div class="slider-meta"><span class="slider-name">Vitesse</span><span class="slider-val" id="spdVal">50</span></div><input type="range" id="spd" min="1" max="100" value="50" oninput="spdVal.textContent=this.value; styleRange(this)" onchange="sendParam('spd',this.value)"></div>
    <div class="slider-row"><div class="slider-meta"><span class="slider-name">Luminosité</span><span class="slider-val" id="lumVal">80</span></div><input type="range" id="lum" min="0" max="255" value="80" oninput="lumVal.textContent=this.value; styleRange(this)" onchange="sendParam('lum',this.value)"></div>
  </div>

  <div class="card">
    <div class="fx-top"><div class="sec-label" style="margin:0;">Animations</div><div class="toggle-wrap" onclick="toggleAuto()"><span class="toggle-label">Aléatoire</span><div class="ios-pill on" id="autoPill"><div class="ios-knob"></div></div></div></div>
    <div class="fx-grid" id="fxGrid"></div>
  </div>

  <div class="footer">Développé rucheusement par<br><strong>Matthieu POUPIN</strong> et <strong>Marceau GUIGUI</strong></div>
</div>

<script>
let EFFECTS=[], autoMode=true, currentEff=0, currentTopo=1, matW=32, matH=16;
const TOPO_DIMS=[{w:16,h:16},{w:32,h:16},{w:64,h:16},{w:32,h:32}];
const lCvs=document.getElementById('liveCanvas'), lCtx=lCvs.getContext('2d');
let liveImgData=null;

function resizeCanvases(){
  const d=TOPO_DIMS[currentTopo]; matW=d.w; matH=d.h;
  lCvs.width=matW; lCvs.height=matH;
  liveImgData=lCtx.createImageData(matW,matH);
  document.getElementById('liveContainer').style.aspectRatio=matW+'/'+matH;
  const prev=dCtx.getImageData(0,0,dCvs.width,dCvs.height);
  const off=document.createElement('canvas'); off.width=dCvs.width; off.height=dCvs.height;
  off.getContext('2d').putImageData(prev,0,0);
  dCvs.width=matW; dCvs.height=matH;
  dCtx.fillStyle='black'; dCtx.fillRect(0,0,matW,matH);
  dCtx.drawImage(off,0,0,matW,matH);
  document.getElementById('drawContainer').style.aspectRatio=matW+'/'+matH;
}
function setTopo(idx){
  currentTopo=idx;
  document.querySelectorAll('.topo-btn').forEach((b,i)=>b.classList.toggle('active',i===idx));
  resizeCanvases(); sendParam('topo',idx);
}
function syncTopo(idx){
  currentTopo=idx;
  document.querySelectorAll('.topo-btn').forEach((b,i)=>b.classList.toggle('active',i===idx));
  const d=TOPO_DIMS[idx]; matW=d.w; matH=d.h;
  lCvs.width=matW; lCvs.height=matH;
  liveImgData=lCtx.createImageData(matW,matH);
  document.getElementById('liveContainer').style.aspectRatio=matW+'/'+matH;
  dCvs.width=matW; dCvs.height=matH;
  dCtx.fillStyle='black'; dCtx.fillRect(0,0,matW,matH);
  document.getElementById('drawContainer').style.aspectRatio=matW+'/'+matH;
}
function styleRange(el){
  const pct=((el.value-el.min)/(el.max-el.min))*100;
  el.style.background=`linear-gradient(to right,rgba(255,255,255,0.7) 0%,rgba(255,255,255,0.7) ${pct}%,rgba(255,255,255,0.1) ${pct}%,rgba(255,255,255,0.1) 100%)`;
}
function initSettings(){
  fetch('/status').then(r=>r.json()).then(d=>{
    EFFECTS=d.effects; currentEff=d.eff; autoMode=d.auto;
    document.getElementById('hue').value=d.hue; document.getElementById('hueVal').textContent=d.hue;
    document.getElementById('spd').value=d.spd; document.getElementById('spdVal').textContent=d.spd; styleRange(document.getElementById('spd'));
    const fSel=document.getElementById('fontSel'); fSel.innerHTML='';
    d.fonts.forEach((f,i)=>{ fSel.innerHTML+=`<option value="${i}" ${i===d.font?'selected':''}>${f}</option>`; });
    document.getElementById('lum').value=d.lum; document.getElementById('lumVal').textContent=d.lum; styleRange(document.getElementById('lum'));
    syncTopo(d.topo||1); syncToggle(); buildGrid();
  }).catch(()=>setStatus("Connexion perdue"));
}
window.onload=initSettings;

let isLive=false;
function toggleLive(){ isLive=!isLive; document.getElementById('livePill').className='ios-pill'+(isLive?' on':''); if(isLive)fetchFrame(); }
function fetchFrame(){
  if(!isLive) return;
  fetch('/frame').then(r=>r.arrayBuffer()).then(buf=>{
    const v=new Uint8Array(buf); const n=matW*matH;
    liveImgData=lCtx.createImageData(matW,matH);
    for(let i=0;i<n&&i*3+2<v.length;i++){liveImgData.data[i*4]=v[i*3];liveImgData.data[i*4+1]=v[i*3+1];liveImgData.data[i*4+2]=v[i*3+2];liveImgData.data[i*4+3]=255;}
    lCtx.putImageData(liveImgData,0,0);
    setTimeout(()=>requestAnimationFrame(fetchFrame),100);
  }).catch(()=>{ setTimeout(fetchFrame,1000); });
}
function switchTab(tab){
  document.getElementById('btn-txt').classList.toggle('active',tab==='txt');
  document.getElementById('btn-draw').classList.toggle('active',tab==='draw');
  document.getElementById('pane-txt').classList.toggle('active',tab==='txt');
  document.getElementById('pane-draw').classList.toggle('active',tab==='draw');
}
const dCvs=document.getElementById('drawCanvas');
const dCtx=dCvs.getContext('2d',{willReadFrequently:true});
dCvs.width=32; dCvs.height=16;
dCtx.fillStyle='black'; dCtx.fillRect(0,0,32,16);
document.getElementById('drawContainer').style.aspectRatio='32/16';
let isDrawing=false, currentTool=1, lastP=null;
function setTool(t){ currentTool=t; document.getElementById('tool-brush').classList.toggle('active',t===1); document.getElementById('tool-eraser').classList.toggle('active',t===0); }
function clearCanvas(){ dCtx.fillStyle='black'; dCtx.fillRect(0,0,dCvs.width,dCvs.height); sendDrawing(); }
function getPos(e){ const r=dCvs.getBoundingClientRect(); const cX=e.touches?e.touches[0].clientX:e.clientX; const cY=e.touches?e.touches[0].clientY:e.clientY; return{x:Math.floor((cX-r.left)/r.width*matW),y:Math.floor((cY-r.top)/r.height*matH)}; }
function plot(x,y){ if(x<0||x>=matW||y<0||y>=matH)return; dCtx.fillStyle=currentTool?'white':'black'; dCtx.fillRect(x,y,1,1); }
function bresenham(x0,y0,x1,y1){ let dx=Math.abs(x1-x0),dy=Math.abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx-dy; while(true){plot(x0,y0);if(x0===x1&&y0===y1)break;let e2=2*err;if(e2>-dy){err-=dy;x0+=sx;}if(e2<dx){err+=dx;y0+=sy;}} }
function startDraw(e){ e.preventDefault(); isDrawing=true; lastP=getPos(e); plot(lastP.x,lastP.y); }
function doDraw(e){ if(!isDrawing)return; e.preventDefault(); const p=getPos(e); bresenham(lastP.x,lastP.y,p.x,p.y); lastP=p; }
function stopDraw(){ if(isDrawing){ isDrawing=false; sendDrawing(); } }
dCvs.addEventListener('mousedown',startDraw); dCvs.addEventListener('mousemove',doDraw); window.addEventListener('mouseup',stopDraw);
dCvs.addEventListener('touchstart',startDraw,{passive:false}); dCvs.addEventListener('touchmove',doDraw,{passive:false}); window.addEventListener('touchend',stopDraw);

function sendDrawing(){
  const n=matW*matH; const data=dCtx.getImageData(0,0,matW,matH).data;
  let hex='';
  for(let p=0;p<n;p+=4){
    let val=((data[p*4]>127?1:0)<<3)|((data[(p+1)*4]>127?1:0)<<2)|((data[(p+2)*4]>127?1:0)<<1)|(data[(p+3)*4]>127?1:0);
    hex+=val.toString(16);
  }
  fetch('/draw',{method:'POST',body:hex}).then(()=>setStatus('Dessin affiché')).catch(()=>setStatus('Erreur'));
}
function sendRucheLogo(){
  const LOGOS={
    '16x16': '00000000008003600c1810041004100410041004100410040c18036000800000',
    '32x16': '00000000000000000000800000036000000cd800001494000015dc00001b64000012240000136c00001dd40000149400000d9800000360000000800000000000',
    '64x16': '00000000000000000000000000000000000000008000000000000003600000000000000cd8000000000000149400000000000015dc0000000000001b640000000000001224000000000000136c0000000000001dd400000000000014940000000000000d98000000000000036000000000000000800000000000000000000000',
    '32x32': '00000000000000000000000000000000000000000000c00000033000000c1c00003b6f0000e082c003208220022082200220832002db6ce00304102002041020020410200204102002041060039b6da002608220022082200220826001a08380007b6e00001c1800000660000001800000000000000000000000000000000000'
  };
  const key=matW+'x'+matH;
  const hex=LOGOS[key];
  if(!hex){setStatus('Logo non disponible pour cette topo');return;}
  // Affiche aussi sur le canvas de dessin
  const n=matW*matH;
  dCtx.fillStyle='black'; dCtx.fillRect(0,0,matW,matH);
  dCtx.fillStyle='white';
  for(let i=0;i<hex.length;i++){
    const v=parseInt(hex[i],16);
    for(let b=3;b>=0;b--){
      if(v&(1<<b)){const p=i*4+(3-b); dCtx.fillRect(p%matW,Math.floor(p/matW),1,1);}
    }
  }
  fetch('/draw',{method:'POST',body:hex}).then(()=>setStatus('Logo Ruche affiché')).catch(()=>setStatus('Erreur'));
}
function buildGrid(){
  const g=document.getElementById('fxGrid'); g.innerHTML='';
  EFFECTS.forEach((name,i)=>{ const b=document.createElement('button'); b.className='eff-btn'+(i===currentEff&&!autoMode?' active':''); b.textContent=name; b.addEventListener('click',()=>selectEff(i)); g.appendChild(b); });
}
function selectEff(idx){ if(autoMode){autoMode=false;syncToggle();} currentEff=idx; buildGrid(); sendParam('eff',idx); }
function toggleAuto(){ autoMode=!autoMode; syncToggle(); sendParam('auto',autoMode?'1':'0'); buildGrid(); }
function syncToggle(){ document.getElementById('autoPill').className='ios-pill'+(autoMode?' on':''); }
function sendMsg(){ const v=document.getElementById('msg').value.trim(); if(!v)return; sendParam('m',v); document.getElementById('msg').value=''; }
document.getElementById('msg').addEventListener('keydown',e=>{if(e.key==='Enter'){e.preventDefault();sendMsg();}});
function setHue(val){ document.getElementById('hue').value=val; document.getElementById('hueVal').textContent=val; sendParam('hue',val); }
let debounce,statusTimer;
function sendParam(key,val){ clearTimeout(debounce); debounce=setTimeout(()=>{fetch('/msg?'+key+'='+encodeURIComponent(val)).then(()=>setStatus('À jour')).catch(()=>setStatus('Erreur réseau'));},50); }
setInterval(()=>{
  fetch('/status').then(r=>r.json()).then(d=>{
    if(autoMode&&(d.eff!==currentEff||d.font!==parseInt(document.getElementById('fontSel').value))){ currentEff=d.eff; document.getElementById('fontSel').value=d.font; buildGrid(); }
    if(d.topo!==undefined&&d.topo!==currentTopo) syncTopo(d.topo);
  }).catch(()=>{});
},2000);
function resetAuto(){ fetch('/msg?reset=1').then(()=>{ autoMode=true; syncToggle(); buildGrid(); switchTab('txt'); setStatus('Mode automatique'); }).catch(()=>setStatus('Erreur')); }
function setStatus(msg){ const s=document.getElementById('status'); s.textContent=msg; s.classList.add('show'); clearTimeout(statusTimer); statusTimer=setTimeout(()=>s.classList.remove('show'),2000); }
</script>
</body>
</html>
)rawliteral";


// --- handlers HTTP ---

void handleRoot()  { server.send_P(200, "text/html", INDEX_HTML); }

void handleFrame() {
    xSemaphoreTake(matrixMutex, portMAX_DELAY);
    const uint16_t n = NUM_LEDS;
    static uint8_t tmpFrame[MAX_LEDS * 3];
    // CRGB est packed (r,g,b) — copie directe valide
    memcpy(tmpFrame, finalBuffer, n * 3u);
    xSemaphoreGive(matrixMutex);

    server.setContentLength(n * 3u);
    server.send(200, "application/octet-stream", "");
    server.client().write(tmpFrame, n * 3u);
}

// Reçoit un dessin depuis l'interface web, encodé en hexadécimal.
// Format compact : chaque caractère hex (0-F) encode 4 pixels en binaire.
// Exemple : 'A' = 0b1010 → pixels 0,2 allumés, 1,3 éteints.
// Ce choix divise par 4 la taille du payload (vs un octet par pixel).
void handleDraw() {
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "Missing payload"); return; }
    const String& payload = server.arg("plain");
    const uint16_t expectedLen = NUM_LEDS / 4u;  // un caractère = 4 pixels
    if (payload.length() < expectedLen) { server.send(400, "text/plain", "Payload too short"); return; }

    xSemaphoreTake(matrixMutex, portMAX_DELAY);
    modeDessin    = true;
    texteEnCours  = false;
    memset(textMask, 0, MAX_LEDS);
    for (uint16_t i = 0; i < expectedLen; i++) {
        char c = payload[i];
        // Décodage manuel du nibble hex (A-F majuscule, a-f minuscule, 0-9)
        uint8_t val = (c >= 'A' && c <= 'F') ? uint8_t(c - 'A' + 10) :
                      (c >= 'a' && c <= 'f') ? uint8_t(c - 'a' + 10) :
                      (c >= '0' && c <= '9') ? uint8_t(c - '0') : 0u;
        // Extraction bit par bit des 4 pixels encodés dans ce nibble
        textMask[i*4+0] = (val >> 3) & 1u;
        textMask[i*4+1] = (val >> 2) & 1u;
        textMask[i*4+2] = (val >> 1) & 1u;
        textMask[i*4+3] =  val       & 1u;
    }
    xSemaphoreGive(matrixMutex);
    server.send(200, "text/plain", "OK");
}

void handleMsg() {
    bool changed = false;
    xSemaphoreTake(matrixMutex, portMAX_DELAY);

    if (server.hasArg("m")) {
        messageGlobal = server.arg("m");
        measureText(); textX = MATRIX_W; texteEnCours = true; modeDessin = false;
        changed = true;
    }
    if (server.hasArg("font")) {
        policeActuelle = static_cast<uint8_t>(constrain(server.arg("font").toInt(), 0, NUM_FONTS - 1));
        measureText(); changed = true;
    }
    if (server.hasArg("hue"))  { COULEUR_HUE   = static_cast<uint8_t>(server.arg("hue").toInt()); changed = true; }
    if (server.hasArg("spd"))  { vitesseEffets  = constrain(server.arg("spd").toInt(), 1, 100);    changed = true; }
    if (server.hasArg("lum"))  {
        luminosite = static_cast<uint8_t>(constrain(server.arg("lum").toInt(), 0, 255));
        FastLED.setBrightness(luminosite); changed = true;
    }
    if (server.hasArg("eff")) {
        const uint8_t e = static_cast<uint8_t>(server.arg("eff").toInt());
        if (e < effects::COUNT) {
            effetManuel = e; effetActuel = e;
            effetAutomatique = false; dernierChangement = millis();
            changed = true;
        }
    }
    if (server.hasArg("auto")) {
        effetAutomatique = (server.arg("auto") == "1");
        if (effetAutomatique) dernierChangement = millis();
        changed = true;
    }
    if (server.hasArg("reset")) {
        effetAutomatique = true; texteEnCours = true; modeDessin = false;
        dernierChangement = millis(); changed = true;
    }
    if (server.hasArg("topo")) {
        const uint8_t t = static_cast<uint8_t>(constrain(server.arg("topo").toInt(), 0, 3));
        pendingClearAll = true;
        applyTopologie(static_cast<Topologie>(t));
        gfxText.resize(MATRIX_W, MATRIX_H);
        textX = MATRIX_W;
        memset(leds,          0, sizeof(leds));
        memset(finalBuffer,   0, sizeof(finalBuffer));
        memset(hardwareBuffer, 0, sizeof(hardwareBuffer));
        memset(textMask,      0, sizeof(textMask));
        effects::reinit();
        measureText();
        Preferences prefs;
        if (prefs.begin("matrix", false)) { prefs.putUChar("topo", t); prefs.end(); }
        changed = true;
    }

    xSemaphoreGive(matrixMutex);
    if (server.hasArg("topo"))
        Serial.printf("[TOPO] %d (%dx%d, %d LEDs)\n",
            static_cast<uint8_t>(topoActuelle), MATRIX_W, MATRIX_H, NUM_LEDS);
    server.send(changed ? 200 : 400, "text/plain", changed ? "OK" : "No Argument");
}

void handleStatus() {
    String json;
    json.reserve(512);

    xSemaphoreTake(matrixMutex, portMAX_DELAY);
    json += "{\"eff\":";  json += effetActuel;
    json += ",\"font\":"; json += policeActuelle;
    json += ",\"auto\":"; json += effetAutomatique ? "true" : "false";
    json += ",\"hue\":";  json += COULEUR_HUE;
    json += ",\"spd\":";  json += vitesseEffets;
    json += ",\"lum\":";  json += luminosite;
    json += ",\"topo\":"; json += static_cast<uint8_t>(topoActuelle);
    json += ",\"matW\":"; json += MATRIX_W;
    json += ",\"matH\":"; json += MATRIX_H;
    xSemaphoreGive(matrixMutex);

    json += ",\"effects\":[";
    for (uint8_t i = 0; i < effects::COUNT; i++) {
        json += '"'; json += effects::name(i); json += '"';
        if (i < effects::COUNT - 1) json += ',';
    }
    json += "],\"fonts\":[";
    for (uint8_t i = 0; i < NUM_FONTS; i++) {
        json += '"'; json += FONTS[i].name; json += '"';
        if (i < NUM_FONTS - 1) json += ',';
    }
    json += "]}";
    server.send(200, "application/json", json);
}


// --- composite frame ---
// Fusionne l'effet de fond (leds[]) avec le texte ou le dessin.
// Algorithme en deux passes :
//   1. Outline : pour chaque pixel de texte actif, on peint ses 8 voisins en couleur
//      de contour — cela garantit la lisibilité quelle que soit la couleur du fond.
//   2. Texte : on écrase les pixels du masque avec la couleur du texte, par-dessus le contour.
// L'ordre des passes est intentionnel : l'outline d'abord, le texte ensuite.

void compositeFrame() {
    if (!texteEnCours && !modeDessin) {
        // Pas de texte ni de dessin : copie directe de leds[] dans finalBuffer[]
        memcpy(finalBuffer, leds, NUM_LEDS * sizeof(CRGB));
        return;
    }

    updateTextColors();
    memcpy(finalBuffer, leds, NUM_LEDS * sizeof(CRGB));

    // Passe 1 — Contour (outline) : dilate chaque pixel de texte sur ses voisins
    for (uint16_t y = 0; y < MATRIX_H; y++) {
        const uint16_t row = y * MATRIX_W;
        const uint16_t above = (y > 0)            ? row - MATRIX_W : row;
        const uint16_t below = (y < MATRIX_H - 1) ? row + MATRIX_W : row;
        for (uint16_t x = 0; x < MATRIX_W; x++) {
            if (!textMask[row + x]) continue;
            if (x > 0) {
                finalBuffer[row + x - 1] = cachedOutlineColor;
                finalBuffer[above + x - 1] = cachedOutlineColor;
                finalBuffer[below + x - 1] = cachedOutlineColor;
            }
            if (x < MATRIX_W - 1u) {
                finalBuffer[row + x + 1] = cachedOutlineColor;
                finalBuffer[above + x + 1] = cachedOutlineColor;
                finalBuffer[below + x + 1] = cachedOutlineColor;
            }
            finalBuffer[above + x] = cachedOutlineColor;
            finalBuffer[below + x] = cachedOutlineColor;
        }
    }
    // Passe 2 — Texte par-dessus le contour
    for (uint16_t i = 0; i < NUM_LEDS; i++)
        if (textMask[i]) finalBuffer[i] = cachedTextColor;
}


// --- microphone ---

void sampleAudio() {
    static float lpf      = 2048.0f;
    static float envelope = 0.0f;
    static uint16_t decay = 0;

    float minF = 4095.0f, maxF = 0.0f;
    for (uint8_t i = 0; i < 128; i++) {
        lpf += 0.08f * (static_cast<float>(analogRead(MIC_PIN)) - lpf);
        if (lpf < minF) minF = lpf;
        if (lpf > maxF) maxF = lpf;
        delayMicroseconds(30);
    }
    uint16_t amplitude = static_cast<uint16_t>(maxF - minF);
    if (amplitude < 20) amplitude = 0;

    if (amplitude > envelope) envelope = 0.15f * amplitude + 0.85f * envelope;
    else                      envelope *= 0.94f;

    const uint8_t level = static_cast<uint8_t>(constrain(map(static_cast<uint16_t>(envelope), 0, 350, 0, 255), 0, 255));
    if (level > audioLevel) { audioLevel = level; decay = 0; }
    else if (decay++ > 2)   { audioLevel = static_cast<uint8_t>(audioLevel * 0.88f); decay = 0; }
}


// --- flush display ---
// Réordonne finalBuffer[] (espace logique, ligne par ligne) vers hardwareBuffer[]
// (espace physique, ordre de câblage serpentin) via XY(), puis envoie les données
// aux LEDs. On ne peut pas écrire directement dans hardwareBuffer[] pendant les
// effets, car XY() est couteuse et les effets supposent un adressage XY simple.

void flushToDisplay() {
    memset(hardwareBuffer, 0, MAX_LEDS * sizeof(CRGB));  // efface les LEDs hors topologie active
    xSemaphoreTake(matrixMutex, portMAX_DELAY);
    for (uint16_t y = 0; y < MATRIX_H; y++)
        for (uint16_t x = 0; x < MATRIX_W; x++) {
            const uint16_t phys = XY(static_cast<uint8_t>(x), static_cast<uint8_t>(y));
            if (phys < MAX_LEDS)
                hardwareBuffer[phys] = finalBuffer[y * MATRIX_W + x];
        }
    xSemaphoreGive(matrixMutex);
    FastLED.show();  // déclenche l'envoi DMA vers les WS2812B (bloquant ~1.3 ms pour 256 LEDs)
}


// --- setup ---

void ledTask(void*);

// La tâche web tourne sur le cœur 0 et se charge uniquement de traiter les requêtes HTTP.
// La séparation sur deux cœurs évite que la latence réseau ne perturbe le rendu LED.
void webServerTask(void*) {
    server.begin();
    Serial.println("[RESEAU] Serveur Web demarre sur Coeur 0");
    for (;;) { server.handleClient(); vTaskDelay(pdMS_TO_TICKS(10)); }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    esp_log_level_set("nvs",         ESP_LOG_NONE);
    esp_log_level_set("Preferences", ESP_LOG_NONE);

    Preferences prefs;
#ifdef WOKWI_SIM
    uint8_t savedTopo = 2;  // forcer 1x4 sur Wokwi (diagram.json = 64x16)
#else
    uint8_t savedTopo = 1;
    if (prefs.begin("matrix", true)) { savedTopo = prefs.getUChar("topo", 1); prefs.end(); }
    else { Serial.println("[NVS] Pas de flash persistant (normal sur Wokwi), topo = 1"); }
#endif

    esp_log_level_set("Preferences", ESP_LOG_ERROR);
    applyTopologie(static_cast<Topologie>(savedTopo));

    gfxText.setTextWrap(false);
    gfxText.resize(MATRIX_W, MATRIX_H);
    matrixMutex = xSemaphoreCreateMutex();

    WiFi.mode(WIFI_AP);
    WiFi.softAP("La-Ruche-Matrix");
    delay(100);
    Serial.print("[RESEAU] AP: "); Serial.println(WiFi.softAPIP());
    Serial.printf("[RESEAU] Topo: %d (%dx%d, %d LEDs)\n", savedTopo, MATRIX_W, MATRIX_H, NUM_LEDS);

    server.on("/",       handleRoot);
    server.on("/msg",    handleMsg);
    server.on("/draw",   HTTP_POST, handleDraw);
    server.on("/frame",  HTTP_GET,  handleFrame);
    server.on("/status", handleStatus);
    server.on("/serial", []() { server.send(200, "text/html", "<style>body{background:#0d0d0f;color:#a0ffa0;font-family:monospace;padding:16px;}</style>" + webSerialBuffer); });
    server.on("/ping",   []() { server.send(200, "text/plain", "Pong!"); });
    server.onNotFound(  []() { server.send(404, "text/plain", "Not found"); });

    // Épinglage sur des cœurs différents : le cœur 0 gère le Wi-Fi/TCP (déjà utilisé par le SDK),
    // le cœur 1 est dédié au rendu LED pour garantir un framerate stable sans jank.
    // LedTask a une stack plus grande car FastLED et Adafruit_GFX sont gourmands en pile.
    xTaskCreatePinnedToCore(webServerTask, "WebTask", 16384, nullptr, 1, nullptr, 0);
    xTaskCreatePinnedToCore(ledTask,       "LedTask", 32768, nullptr, 1, nullptr, 1);

    FastLED.addLeds<WS2812B, PIN_DATA, GRB>(hardwareBuffer, MAX_LEDS);
    FastLED.setMaxPowerInVoltsAndMilliamps(5, 500);
    FastLED.setBrightness(luminosite);

    effects::init();
    textX = MATRIX_W;
    measureText();
    texteEnCours = true;
}


// --- tâche LED (cœur 1) ---
// Boucle principale du rendu. FreeRTOS permet de la faire tourner sur le cœur 1
// en parallèle du serveur web sur le cœur 0, sans preemption ni ralentissement mutuel.
// Le mutex protège la section critique où on lit/écrit les buffers partagés.

void ledTask(void*) {
    Serial.println("[LED] Tache LED demarree sur Coeur 1");
    for (;;) {
        // Un changement de topologie demande d'éteindre toutes les LEDs immédiatement
        // pour éviter d'afficher des artefacts pendant la transition.
        if (pendingClearAll) {
            pendingClearAll = false;
            xSemaphoreTake(matrixMutex, portMAX_DELAY);
            memset(hardwareBuffer, 0, sizeof(hardwareBuffer));
            xSemaphoreGive(matrixMutex);
            FastLED.show();
        }

        const uint32_t now = millis();
        if (now - lastFrameTime >= FRAME_DELAY) {
            lastFrameTime = now;

            if (MIC_ENABLED) sampleAudio();

            xSemaphoreTake(matrixMutex, portMAX_DELAY);

            // Rotation automatique des effets toutes les DUREE_EFFET_MS millisecondes
            if (effetAutomatique && (now - dernierChangement > DUREE_EFFET_MS)) {
                effetActuel    = (effetActuel + 1) % effects::COUNT;
                policeActuelle = static_cast<uint8_t>(random(NUM_FONTS));
                measureText();
                dernierChangement = now;
                Serial.printf("[AUTO] %s | %s\n", effects::name(effetActuel), FONTS[policeActuelle].name);
            }

            // L'effet écrit dans leds[] en coordonnées logiques (pas dans hardwareBuffer)
            effects::run(effetActuel);

            if (texteEnCours && !modeDessin) {
                // Conversion vitesse [1-100] → intervalle de scroll [120-15 ms/pixel]
                const uint32_t scrollInterval = static_cast<uint32_t>(map(vitesseEffets, 1, 100, 120, 15));
                // Rattrapage des frames manquées : on avance textX autant de fois que nécessaire
                while (now - lastScrollUpdate > scrollInterval) { lastScrollUpdate += scrollInterval; textX--; }
                if (textX < -textWidth) textX = MATRIX_W;  // reboucle le défilement

                // Adafruit_GFX dessine dans textMask[] via drawPixel() surchargé (cf. MatrixTextGFX)
                memset(textMask, 0, NUM_LEDS);
                gfxText.setTextColor(1);
                gfxText.setFont(FONTS[policeActuelle].font);

                int16_t baseline = FONTS[policeActuelle].baselineOffset;
                if (topoActuelle == Topologie::T2x2) {
                    // Sur une matrice carrée 32×32, on centre verticalement le texte
                    const int16_t textH = FONTS[policeActuelle].baselineOffset;
                    baseline = (MATRIX_H / 2) - (textH / 2) + textH;
                    if (baseline > MATRIX_H) baseline = MATRIX_H - 1;
                }
                gfxText.setCursor(textX, baseline);
                gfxText.print(messageGlobal);
            }

            compositeFrame();      // superpose texte/fond -> finalBuffer[]
            xSemaphoreGive(matrixMutex);
            flushToDisplay();      // réordonne + envoie aux LEDs (hors mutex car hardwareBuffer est privé à cette tâche)
        }

        vTaskDelay(pdMS_TO_TICKS(1));  // cède le CPU 1 ms pour ne pas affamer les autres tâches
    }
}

void loop() { vTaskDelay(pdMS_TO_TICKS(1000)); }
