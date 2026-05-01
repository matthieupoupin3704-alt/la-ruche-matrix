# Flasher Marlin sur la Megatronics v3.3

## Prérequis

- Arduino IDE 2.x **ou** PlatformIO
- Câble USB-B (le gros carré, comme pour une imprimante)
- Jumper RESET-EN en place sur la Megatronics (sinon le flash ne fonctionne pas)

## Étapes

### 1. Télécharger Marlin

```bash
git clone https://github.com/MarlinFirmware/Marlin.git
cd Marlin
git checkout release-2.1.2.2
```

### 2. Copier les fichiers de config

Copier `Configuration.h` et `Configuration_adv.h` de ce dossier dans `Marlin/Marlin/`.

```bash
cp /chemin/vers/marlin_config/Configuration.h Marlin/Marlin/
cp /chemin/vers/marlin_config/Configuration_adv.h Marlin/Marlin/
```

### 3. Flash via Arduino IDE

1. Ouvrir `Marlin/Marlin.ino` dans Arduino IDE
2. **Tools → Board** : `Arduino Mega or Mega 2560`
3. **Tools → Processor** : `ATmega2560`
4. **Tools → Port** : le port série de la Megatronics (ex. `/dev/cu.usbserial-XXXX`)
5. Cliquer **Upload**

### 4. Flash via PlatformIO

```bash
cd Marlin
# Vérifier que platformio.ini cible mega2560
pio run -e mega2560 --target upload
```

## Configuration appliquée

| Paramètre | Valeur | Raison |
|-----------|--------|--------|
| `SERIAL_PORT` | 3 | Serial3 = connecteur NXT (D14/D15) → ESP32 |
| `BAUDRATE` | 115200 | Vitesse UART ESP32 ↔ ATmega |
| `EXTRUDERS` | 0 | Pas d'extrudeur |
| `TEMP_SENSOR_0` | 0 | Pas de thermisteur |
| `TEMP_SENSOR_BED` | 0 | Pas de plateau chauffant |
| `X_BED_SIZE` | 350 | Course X en mm |
| `Y_BED_SIZE` | 440 | Course Y en mm |
| `DEFAULT_AXIS_STEPS_PER_UNIT` | X=5, Y=50 | GT2+20dents→5pas/mm ; TR10x4P2→50pas/mm |
| Endstops | désactivés | Pas d'endstops sur la structure |

## Câblage ESP32 ↔ Megatronics

La communication passe par le connecteur **NXT** de la Megatronics :

```
Megatronics NXT         Pont logique 3.3V↔5V       ESP32-S3
─────────────────       ─────────────────────       ────────
1. 5V ──────────────── HV (alimentation pont)
2. GND ─────────────── GND ──────────────────────── GND
3. D14 (TX3) ────────── HV1 → LV1 ─────────────────── GPIO44 (RX)
4. D15 (RX3) ────────── LV2 → HV2 ◄──────────────── GPIO43 (TX)
                        LV (3.3V) ◄──── 3.3V ESP32
```

**Important** : ne pas connecter directement 5V ATmega → 3.3V ESP32 sans pont logique.

## Test rapide

Une fois flashé, connecter via le port USB de la Megatronics et envoyer depuis le moniteur série :

```
G28 X Y    ; homing (sans endstops = juste reset position à 0)
G1 X175 Y220 F3000  ; aller au centre
M114       ; afficher la position courante
```
