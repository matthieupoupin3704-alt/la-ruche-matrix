# La Ruche Matrix — Panneau LED interactif pour soirées

*Par Matthieu POUPIN & Marceau GUIGUI — La Ruche, option Culture Maker @ Eirlab / ENSEIRB-MATMECA*

---

## Introduction

Au départ, on voulait juste faire un projet sympa pour notre association de DJing et lights à l'école. Une platine ? Trop classique. Un vague truc qui clignote ? Pas assez. On a finalement atterri sur quelque chose qu'on n'avait jamais vu suspendu à un pont de lumière : un panneau LED matriciel que n'importe qui dans la salle peut contrôler depuis son téléphone.

Le principe est simple — 1024 LEDs RGB organisées en 4 dalles 16×16, accrochées à une structure motorisée qui se fixe sur le pont de lumière, et un ESP32-S3 qui gère tout ça via un réseau Wi-Fi local. Pas d'appli à installer, pas d'internet nécessaire : on se connecte au panneau comme à une box, on ouvre un navigateur, et on peut afficher du texte, choisir parmi 32 animations ou dessiner pixel par pixel en temps réel.

Le projet a été réalisé dans le cadre de l'option Culture Maker à l'Eirlab, le fablab de l'ENSEIRB-MATMECA. Budget total : environ 150-160€, dont une bonne moitié part dans la mécanique.

![Photo du panneau allumé en soirée]
*→ placeholder photo : panneau allumé, vue de face*

---

## Étape 1 — Le matériel

### Électronique

| Composant | Qté | Notes |
|-----------|-----|-------|
| ESP32-S3-DevKitC-1 | 1 | Le cerveau du projet. Wi-Fi intégré, USB natif |
| Dalles LED WS2812B 16×16 | 4 | LEDs adressables individuellement, format carré |
| Carte Megatronics v3.3 | 1 | Récupération — gère l'alimentation et les moteurs |
| Alimentation 19.5V (transfo PC modifié) | 1 | Embout modifié pour s'adapter à la Megatronics |
| Condensateur 1000µF 10V | 1 | Absorbe les pics de courant des LEDs |
| Résistance 330Ω | 1 | Protection sur le fil data entre ESP et panneaux |
| Fils, connecteurs, visserie | — | |

### Mécanique

La structure s'inspire directement d'une imprimante 3D cartésienne. Elle permet deux mouvements indépendants pour positionner le panneau dans l'espace.

| Composant | Qté | Prix unitaire |
|-----------|-----|--------------|
| Supports d'arbre SH16/SK16 | 4 | 6,11 € |
| Supports d'arbre SH8/SK8 | 4 | 4,26 € |
| Roulements linéaires SC16UU (∅16mm) | 4 | 15,27 € |
| Roulements linéaires SC8UU (∅8mm) | 2 | 4,95 € |
| Arbre de précision 16mm h6 — 500mm | 2 | 9,64 € |
| Arbre de précision 8mm h6 — 500mm | 2 | 5,91 € |
| Tige filetée trapézoïdale TR10x4P2 — 500mm | 2 | 5,85 € |
| Écrou trapézoïdal 10x4P2 bronze/alu | 2 | 31,63 € |
| Accouplement flexible D20L25 (5/10mm) | 2 | 6,60 € |
| Courroie GT2 ouverte 6mm (~1000mm) | 4 | 7,15 € |
| Poulie GT2 20 dents (alésage 5mm) | 2 | 2,27 € |
| Moteurs pas-à-pas 42SH0034D (NEMA 17) | 2 | récupération |
| Pièces imprimées PLA | — | fichiers STL dans le repo |
| Profilés aluminium + visserie | — | renforcement structure |

### Outils nécessaires
- Imprimante 3D (PLA)
- Fer à souder
- Multimètre
- VSCode + extension PlatformIO

---

## Étape 2 — La structure mécanique

La structure est pensée pour s'accrocher à un pont de lumière standard. Elle reprend le principe d'une imprimante 3D : deux axes de déplacement indépendants permettent de repositionner le panneau sans décrocher quoi que ce soit.

**Axe vertical** — un moteur NEMA 17 entraîne une tige filetée trapézoïdale TR10x4P2. L'écrou bronze solidaire du chariot monte ou descend quand la tige tourne. C'est lent, précis, et irréversible sans moteur — parfait pour maintenir une position sans consommer de courant.

**Axe horizontal** — le deuxième moteur déplace un chariot sur des arbres de précision via une courroie GT2. Le panneau peut ainsi translater latéralement sur toute la largeur de la structure.

Les pièces de liaison sont imprimées en PLA sur l'Eirlab. Des profilés aluminium renforcent l'ensemble pour tenir les vibrations d'une soirée. La fixation au pont se fait par des attaches vissées sur les colonnes verticales.

> L'état actuel de la structure utilise principalement des pièces PLA. L'intégration des profilés alu et des attaches pont est en cours — les fichiers CAO reflètent la version finale prévue.

![Rendu CAO vue de face]
*→ placeholder : vue de face, structure complète*

![Rendu CAO vue de côté]
*→ placeholder : vue de côté, mécanisme visible*

Les fichiers OnShape et STL sont disponibles dans le dépôt GitHub.

---

## Étape 3 — Le câblage

### Principe général

L'alimentation vient d'un transformateur PC 19.5V dont l'embout a été modifié pour s'adapter à la Megatronics. La carte fournit du 5V régulé qui alimente à la fois l'ESP32-S3 et les quatre panneaux LED. Les moteurs sont pilotés par les drivers intégrés à la Megatronics.

Les quatre panneaux WS2812B sont chaînés en série : un seul fil de données part du GPIO 13 de l'ESP, traverse les panneaux un par un, et c'est tout. Malgré la simplicité du câblage, trois détails sont non-négociables.

### Les trois détails qui changent tout

**La masse commune** — l'ESP (3.3V logique) et les panneaux (5V) doivent partager le même GND. Sans ça, le signal data est interprété de manière aléatoire par les LEDs. Tous les GND sont reliés ensemble.

**La résistance 330Ω sur data** — placée entre le GPIO 13 et l'entrée du premier panneau, au plus près de l'ESP. Elle protège la sortie de l'ESP et évite les réflexions sur le câble.

**Le condensateur 1000µF** — placé entre le 5V et le GND, directement à l'entrée du premier panneau. Les WS2812B tirent de gros pics de courant à chaque changement de couleur. Sans le condo, ces pics peuvent faire rebooter l'ESP en pleine soirée.

![Schéma de câblage]
*→ placeholder : schéma électronique (voir fichier `docs/schema_electronique_brief.md` pour la version générée)*

---

## Étape 4 — Le firmware

### Installation en 5 minutes

1. Installer [VSCode](https://code.visualstudio.com/) + l'extension **PlatformIO**
2. Cloner le dépôt : `git clone https://github.com/LaRuche-ENSEIRB/la-ruche-matrix`
3. Ouvrir le dossier dans VSCode
4. Brancher l'ESP32-S3 en USB-C
5. Dans PlatformIO : cliquer sur **Upload** (environnement `esp32dev`)

PlatformIO télécharge les dépendances, compile et flashe automatiquement. Première mise en route : moins de 5 minutes.

### Comment c'est organisé

Le code tient en deux fichiers :

**`src/main.cpp`** gère l'infrastructure : le serveur web embarqué, le rendu du texte défilant, la persistance des réglages, et deux tâches qui tournent en parallèle sur les deux cœurs de l'ESP32.

**`include/effects.h`** contient les 32 animations. Chaque effet est une fonction indépendante qui écrit dans un tableau de pixels. Ajouter un effet revient à écrire une nouvelle fonction et l'enregistrer dans un tableau — c'est fait pour être modifiable.

### Deux cœurs, deux tâches

L'ESP32 a deux cœurs, et on s'en sert vraiment :

```
Cœur 0 — WebTask        Cœur 1 — LedTask
──────────────────       ─────────────────
Répond aux requêtes      Calcule les effets
HTTP du téléphone   ←→   Compose le texte
                         Envoie aux LEDs
              (mutex partagé)
```

Le Wi-Fi et les LEDs tournent en parallèle. Un mutex (verrou logiciel) évite que les deux tâches écrivent dans les mêmes données en même temps — sans ça, on aurait des corruptions visuelles aléatoires.

### Le pipeline d'image

Entre le calcul d'un effet et l'affichage physique, le signal passe par trois étapes :

```
leds[]          ← les effets calculent ici
     ↓ compositeFrame()
finalBuffer[]   ← le texte est superposé par-dessus
     ↓ flushToDisplay()
hardwareBuffer[]← pixels réordonnés selon le câblage physique
     ↓ FastLED.show()
LEDs
```

Cette séparation en étages permet de changer un effet sans toucher au rendu du texte, et de gérer le câblage en serpentin des panneaux sans que les effets aient à s'en préoccuper.

### Les topologies

Sans recompiler, depuis l'interface web, on peut passer entre quatre configurations :

| Topologie | Résolution | Panneaux |
|-----------|-----------|---------|
| 1×1 | 16×16 px | 1 panneau |
| 1×2 | 32×16 px | 2 panneaux côte à côte |
| 1×4 | 64×16 px | 4 panneaux en bande |
| 2×2 | 32×32 px | 4 panneaux en carré |

Le choix est mémorisé dans la flash de l'ESP et restauré au prochain démarrage.

---

## Étape 5 — Première utilisation

### Connexion

1. Allumer le panneau
2. Sur le téléphone : Wi-Fi → **`La-Ruche-Matrix`** (pas de mot de passe)
3. Navigateur → `192.168.4.1`

L'interface s'affiche. Elle fonctionne sur tous les navigateurs mobiles, sans installation.

![Capture interface web]
*→ placeholder : screenshot interface mobile*

### Ce qu'on peut faire

**Texte défilant** — on tape un message, on choisit une police parmi 7 (du pixel art minuscule au grand sans-serif), on envoie. Le texte défile en boucle sur fond d'animation. La couleur du texte et de son contour s'ajuste automatiquement selon la teinte choisie pour rester lisible.

**Dessin libre** — un canvas tactile permet de dessiner directement sur le panneau pixel par pixel. Il y a un crayon, une gomme, et un bouton pour afficher le logo de La Ruche.

**32 animations** — organisées en grille dans l'interface. De la pluie Matrix au couloir de Doom, en passant par Pac-Man, le Jeu de la Vie de Conway, une simulation d'ADN, Pong, un cube 3D en rotation ou encore un visualiseur audio. Chaque animation réagit aux paramètres globaux.

**Paramètres** — une roue de teinte (avec 6 préréglages rapides), un slider de vitesse et un slider de luminosité. Tout s'applique en temps réel.

**Mode automatique** — en activant le mode aléatoire, le panneau change d'animation et de police toutes les 15 secondes tout seul. Pratique pour laisser tourner en fond de soirée.

**Visualisation live** — un toggle affiche dans le navigateur ce qui est actuellement sur le panneau, en temps réel. Pratique pour voir le résultat depuis la régie.

---

## Étape 6 — Aller plus loin

### Ajouter ses propres animations

C'est la modification la plus accessible. Dans `effects.h`, toutes les animations suivent le même modèle :

```cpp
void monEffet() {
    for (uint8_t y = 0; y < MATRIX_H; y++)
        for (uint8_t x = 0; x < MATRIX_W; x++)
            _px(x, y) = CHSV(COULEUR_HUE + x * 4, 255, 200);
}
```

`_px(x, y)` écrit un pixel en coordonnées logiques. `COULEUR_HUE` est la teinte choisie par l'utilisateur. `MATRIX_W` et `MATRIX_H` s'adaptent automatiquement à la topologie active.

Il suffit ensuite d'ajouter la fonction dans le tableau `_table[]` et son nom dans `_names[]`, et d'incrémenter `COUNT` — l'interface web l'affiche automatiquement.

### Ajouter des panneaux

La structure accepte jusqu'à 4 panneaux et le firmware les gère tous. Il suffit de changer la topologie depuis l'interface (ou de modifier `savedTopo` dans le code pour changer la valeur par défaut au démarrage).

### Intégrer le contrôle des moteurs

La Megatronics expose les sorties moteurs sur des headers standards. L'ESP32 peut piloter les drivers directement via des signaux STEP/DIR, ou on peut flasher Marlin sur l'ATmega de la carte et envoyer du G-code. Les deux approches sont envisagées — la documentation sera mise à jour quand c'est en place.

---

## Résultat

![Photo du panneau en soirée, vue d'ensemble]
*→ placeholder : photo ambiance soirée*

Le projet est open source. Le code, les fichiers CAO et la BOM complète sont sur GitHub :
**→ github.com/LaRuche-ENSEIRB/la-ruche-matrix**

Si vous reproduisez ou adaptez le projet, on serait curieux de voir ce que ça donne — n'hésitez pas à partager.

*Matthieu POUPIN & Marceau GUIGUI — La Ruche @ ENSEIRB-MATMECA, Eirlab, 2025*
