# Magnetic Transmitting Loop Antenna Controller (ML Controller)

**Contrôleur d'Antenne Boucle Magnétique d'Émission**

* **Auteur :** Loftur E. Jonasson (TF3LJ / VE2AO / VE2LJX)
* **Version du firmware :** 5.00-ESP32S3
* **Plateformes supportées :** **Waveshare ESP32-S3-TOUCH-LCD-7** (ESP32-S3 Dual-Core LX7), Teensy 4.1 & Teensy 3.1 / 3.2
* **Licence :** GNU General Public License v3.0 (GPLv3)

---

## 📖 Description Générale

Ce projet contient le code source du **contrôleur automatique d'antenne boucle magnétique d'émission** (*Magnetic Transmitting Loop Antenna Controller*).

Une antenne boucle magnétique possédant une bande passante très étroite, chaque changement de fréquence nécessite un réajustement précis de son condensateur d'accord. Ce contrôleur automatise l'accord de l'antenne en entraînant un moteur pas-à-pas connecté au condensateur variable (sous vide, papillon, etc.), synchronisé en temps réel avec le transcepteur radioamateur via son interface CAT et assisté par une mesure automatique du ROS (*SWR Autotune*).

---

## ✨ Fonctionnalités Principales

- **Commande Moteur Pas-à-Pas de Précision :**
  - Support de drivers bipolaires : Allegro A4975 (paire) ou modules type StepStick/Pololu (TI DRV8825, Allegro A4988).
  - Support du contrôle à distance du moteur pas-à-pas via bus série **RS485** (`#define RS485STEPPER 1`).
  - Gestion du microstepping variable (pas entier, 1/2, 1/4, 1/8 de pas) et vitesse adaptative selon la distance.
  - Compensation automatique du jeu mécanique (*Backlash compensation*).
  - Détection optionnelle de fins de course (*End-stop switches*) ou gestion intelligente pour condensateur sous vide et papillon.

- **Intégration Radio / Protocoles CAT (Suivi Automatique de Fréquence) :**
  - Connexion série UART (TTL ou RS232 inversé, 1200 à 115200 bps) avec plus de 17 configurations de radios :
    - **ICOM :** CI-V Auto / Poll
    - **Kenwood :** TS-440, TS-870, TS-480, TS-590, TS-2000
    - **Yaesu :** FT-100, FT-747, FT-817/847/857/897, FT-920, FT-990, FT-1000MP / Mk-V, FT-450 / 950 / 1200 / 2000 / 3000 / 5000
    - **Elecraft :** K3, KX3 (Auto / Poll)
    - **TenTec :** Modes binaire et ASCII
    - **Mode Pseudo-VFO :** Permet une utilisation autonome sans radio connectée, où l'encodeur rotatif simule le VFO de la radio.
  - Prise en charge de 4 profils radio sauvegardés en EEPROM.

- **Wattmètre, ROS-mètre et Accord Automatique (*SWR Autotune*) :**
  - Mesure de la puissance directe/réfléchie et du ROS (SWR) :
    - Option détecteur logarithmique AD8307 (haute dynamique).
    - Option pont à diodes (Tandem Match ou pont de Bruene).
  - **SWR Autotune :** Recherche automatique du creux de ROS (*SWR Dip Hunt*) en commandant automatiquement la radio (passage temporaire en mode AM, baisse de la puissance d'émission, activation PTT, recherche du ROS minimum, puis restauration des réglages initiaux).

- **Gestion des Mémoires EEPROM & Multi-antennes :**
  - Stockage de jusqu'à 200 couples (Fréquence, Position du moteur).
  - Interpolation linéaire entre les points de calibration pour un accord fluide sur toutes les bandes.
  - Prise en charge de la gestion de 2 ou 3 antennes sélectionnées automatiquement selon la fréquence ou manuellement.
  - Commutation de condensateurs fixes ou de prises de couplage via sorties numériques (bandswitching).

- **Interface Utilisateur & Affichage :**
  - Écran LCD 20x4 (HD44780 ou OLED) piloté par la bibliothèque à haut débit `LiquidCrystalFast`.
  - Bargraphe haute résolution sur LCD simulé type LP-100 avec indicateur de puissance crête (*Peak Hold*).
  - Économiseur d'écran paramétrable avec message personnalisé ou fréquence.
  - Navigation complète par menu via encodeur rotatif et boutons poussoirs.

- **Interface USB & Contrôle à Distance :**
  - Port USB virtuel (série) pour configuration, sauvegarde/restauration des mémoires (`$memoryget`, `$memoryset`), contrôle du SWR tune, télémétrie en continu (`$pcont`), et débogage.

---

## 🛠️ Architecture Matérielle (Hardware) & Pinout ESP32-S3

* **Microcontrôleur :** ESP32-S3 (Dual-Core Xtensa LX7 à 240 MHz, Flash 8MB, PSRAM 8MB) sur carte **Waveshare ESP32-S3-TOUCH-LCD-7**.
* **Affichage & Tactile :** Écran TFT 7 pouces (résolution **800x480**) avec dalle tactile capacitive controller **GT911** sur bus I2C.
* **Mesure RF (Puissance et SWR) :** Convertisseur ADC 12 bits **AD7991** sur le bus I2C (`I2C_SDA_PIN = GPIO8`, `I2C_SCL_PIN = GPIO9`).
* **Interface RS485 Multi-Antennes :** Port UART Hardware `Serial2` (`TX = GPIO15`, `RX = GPIO16`) à 9600 bps pour le contrôle du moteur pas-à-pas distant et la sélection d'antenne.
* **Interface Radio CAT :** Port UART Hardware `Serial1` (`TX = GPIO43`, `RX = GPIO44`).
* **Sorties numériques Teensy (PTT, sélecteurs d'antenne, bits de bande/profil, alarme ROS) :** non disponibles sur cette carte. Le tableau de broches des versions précédentes de ce document était erroné : `GPIO10`, `14`, `21`, `47`, `48` sont utilisées par le bus RGB de l'écran et `GPIO11`, `12`, `13` par la carte SD. Ces sorties sont exclues du code ESP32 (`#if !ESP32`). Pour un PTT ou des relais, utiliser le bus RS485 ou un module d'extension sur des broches libres (`GPIO6` ne figure dans aucun tableau d'interface de la carte, à vérifier sur le schéma ; `GPIO15/16` sont déjà prises par RS485).
* **Commandes Utilisateur Tactiles :** Boutons et molette virtuels sur l'écran tactile (remplaçant l'encodeur rotatif et les boutons poussoirs).

---

## 🔌 Adaptation pour Teensy 4.1 (Modifications apportées au code)

Pour assurer une compatibilité complète avec la carte **Teensy 4.1** sans casser la rétrocompatibilité avec les cartes Teensy 3.1 et 3.2, les adaptations suivantes ont été implémentées :

1. **Réinitialisation Logicielle (`SOFT_RESET()`) dans `ML.h` :**
   - Sur Teensy 3.x, le reset logiciel utilisait l'adresse fixe du registre SCB_AIRCR Kinetis (`0xE000ED0C`).
   - Sur Teensy 4.1 (ARM Cortex-M7 i.MX RT1062), `SCB_AIRCR = 0x05FA0004` est utilisé pour déclencher un `SYSRESETREQ` propre via les macros CMSIS.

2. **Configuration du Port Série ICOM CI-V (`ML_TRX.ino`) :**
   - Le bus ICOM CI-V nécessite une ligne TX en drain ouvert (*Open Drain*) et un récepteur RX avec résistance de rappel (*Pullup*).
   - Sur Teensy 3.x, cela était réalisé en manipulant directement le registre `portConfigRegister(...)` avec `PORT_PCR_ODE`, `PORT_PCR_PE` et `PORT_PCR_PS`.
   - Sur Teensy 4.1, les registres d'E/S (IOMUXC) étant différents, la configuration utilise `pinMode(Uart_TXD, OUTPUT_OPENDRAIN)` et `pinMode(Uart_RXD, INPUT_PULLUP)`.

3. **Inclusions et Gestion de l'I2C (`ML_v410.ino` et `ML_PSWR.ino`) :**
   - La bibliothèque `i2c_t3.h` est spécifique aux microcontrôleurs Kinetis (Teensy 3.x/LC).
   - Sur Teensy 4.1, l'inclusion bascule sur la bibliothèque standard `<Wire.h>`, avec une initialisation `Wire1.begin()` et `Wire1.setClock(400000)`.
   - La méthode de lecture I2C `Wire1.readByte()` spécifique à `i2c_t3` a été remplacée par la méthode standard `Wire1.read()`.

4. **Broches d'E/S et Repérage des Pins (`ML.h`) :**
   - Sur Teensy 3.1/3.2, les pins 24 à 33 correspondaient à des pastilles à souder sous la carte. Sur Teensy 4.1, ce sont des broches standard situées sur les connecteurs de bordure.

---

## 📁 Structure des Fichiers du Projet

| Fichier | Description |
| :--- | :--- |
| `ML.h` | Fichier d'en-tête principal : définitions matérielles, options de compilation (`#define`), structures de données, `SOFT_RESET()` et constantes. |
| `ML_v410.ino` | Fichier principal : initialisation (`setup()`), boucle principale (`loop()`), tâches périodiques et suivi de fréquence. |
| `ML_GFX.ino` | **ESP32-S3 :** pilote CH422G, panneau RGB, imitation du LCD 20x4, boutons tactiles GT911 et fenêtre SD. |
| `ML_SD.ino` | **ESP32-S3 :** sauvegarde / restauration sur carte SD (tâche d'arrière-plan, autosave, restauration au démarrage). |
| `ML_Font5x7.h` | Police 5x7 utilisée pour le rendu du LCD (Adafruit GFX, licence BSD). |
| `ML_Display.ino` | Gestion de l'écran LCD 20x4, du buffer virtuel, de l'économiseur d'écran et des bargraphes de puissance/ROS. |
| `ML_Menu.ino` | Arborescence et gestion du menu de configuration utilisateur (réglages moteur, radios, calibration, mémoires). |
| `ML_PSWR.ino` | Échantillonnage ADC, calculs de la puissance directe/réfléchie (mW, W), du PEP, du ROS et étalonnage (AD8307 / diodes). |
| `ML_Pos_Mgmnt.ino` | Gestion des mémoires de position, tri des présélections, interpolation fréquence/position et démultiplication variable. |
| `ML_SWRtune.ino` | Algorithme de recherche automatique du creux de ROS (*SWR Autotune*), contrôle automatique du transcepteur en PTT/AM. |
| `ML_TRX.ino` | Pilote de communication CAT pour les différents transcepteurs (ICOM, Kenwood, Yaesu, Elecraft, TenTec, Pseudo-VFO) avec support Open Drain pour Teensy 3.x et 4.x. |
| `ML_USB.ino` | Interprète de commandes série USB pour le contrôle distant, l'export/import des mémoires et le débogage. |
| `ML__Switches_and_Stepper.ino` | Gestion matérielle des boutons poussoirs (anti-rebond, détection appui court/long), de la commande du moteur pas-à-pas et du protocole RS485. |
| `_EEPROMAnything.h` | Fonctions génériques (templates) de lecture/écriture de structures en EEPROM. |
| `gpl.txt` | Texte complet de la licence publique générale GNU (GPLv3). |

---

## ⚙️ Configuration (`ML.h`)

Toutes les options matérielles et logicielles sont configurables dans le fichier `ML.h` avant la compilation :

```c
// Choix du mode de contrôle du moteur pas-à-pas (sélectionner un seul driver à 1) :
#define DRV8825STEPPER     0    // 1 pour module DRV8825 / A4988
#define A4975STEPPER       0    // 1 pour 2x Allegro A4975
#define RS485STEPPER       1    // 1 pour contrôle à distance via bus RS485 (Serial2, 9600 bps)

// Détecteur logarithmique AD8307 (0 = Non, 1 = Oui)
#define AD8307_INSTALLED   0

// Activation de l'autotune SWR et du wattmètre (0 = Désactivé, 1 = Activé)
#define PSWR_AUTOTUNE      0

// Gestion des fins de course (1 = Condensateur sous vide sans fin de course, 2 = Avec fin de course, 3 = Papillon)
#define ENDSTOP_OPT        1

// Radio par défaut à l'allumage (0 = ICOM CI-V Auto, 1 = ICOM Poll, 4 = Kenwood TS-480/2000, 7 = Yaesu FT-8x7, 13 = Elecraft K3, 17 = Pseudo-VFO...)
#define DEFAULT_RADIO      1
```

---

## 📡 Protocole de Commande RS485 Multi-Antennes

Lorsque l'option `#define RS485STEPPER 1` est activée dans `ML.h`, le contrôleur communique via le bus série RS485 (`Serial2` à 9600 bps) avec adressage de l'antenne active `<ant>` (0, 1 ou 2) :

| Commande | Paramètres | Description |
| :--- | :--- | :--- |
| `$SINIT <ant>` | `<ant>` (0-2) | Initialisation du contrôleur d'antenne RS485 pour l'antenne cible. |
| `$SON <ant>` | `<ant>` (0-2) | Activation de l'alimentation moteur de l'antenne spécifiée. |
| `$SOF <ant>` | `<ant>` (0-2) | Coupure d'alimentation moteur pour économie d'énergie. |
| `$SINC <ant> <res>` | `<ant> <res>` | Déplacement sens horaire pour l'antenne cible avec résolution `res` (0 = 1/8 micropas, 3 = pas entier). |
| `$SDEC <ant> <res>` | `<ant> <res>` | Déplacement sens anti-horaire pour l'antenne cible avec résolution `res`. |
| `$SMOV <ant>` | `<ant>` (0-2) | Exécution du déplacement d'un pas sur l'antenne spécifiée. |
| `$SANT <ant>` | `<ant>` (0-2) | Sélection / Commutation de l'antenne active sur le bus RS485. |

---

## 🖥️ Écran tactile 800x480

L'écran LCD 20x4 à cristaux liquides du montage d'origine est imité (matrice de points HD44780, caractères 5x8, bargraphes de puissance/ROS compris) dans la partie haute de l'écran. Il affiche exactement le contenu du tampon `virt_lcd[]`, donc tous les menus et affichages existants apparaissent tels quels. Thème `LCD_THEME` : `0` = blanc sur bleu, `1` = noir sur vert-jaune.

Sous l'afficheur, deux rangées de boutons tactiles :

| Bouton | Action |
| :--- | :--- |
| `<<` `<` `>` `>>` | Molette : un pas (`<` `>`) ou 8 pas (`<<` `>>`), répétition en maintenant. Dans le menu : élément précédent / suivant. |
| `MENU` | Appui court = *Enter* dans le menu. **Appui long (1 s) = ouvre le menu de configuration.** Hors menu, l'appui court ne fait rien au moteur (plus de recalibrage accidentel). |
| `UP` / `DOWN` | Comme les boutons d'origine : réglage manuel, présélection suivante/précédente si la radio est hors ligne, sens de recherche pendant le *SWR Autotune*, défilement du menu. |
| `TUNE` | Lance le SWR Autotune (grisé si `PSWR_AUTOTUNE = 0`). |
| `RECAL` | Recalibrage de la position du moteur (grisé si `RECALIBRATE = 0`). |
| `ANT n` | Bascule d'antenne en mode manuel à deux banques (grisé si le changement est automatique par fréquence). |
| `SD` | Ouvre la fenêtre de la carte SD (voir ci-dessous). Le voyant est vert (prête), orange (occupée) ou rouge (absente / erreur). |

Le tactile GT911 est lu une fois toutes les 20 ms. Si le contrôleur cesse de répondre pendant qu'un bouton est enfoncé, le bouton est relâché au bout de 200 ms (le moteur ne peut pas rester bloqué en marche).

---

## 💾 Sauvegarde sur carte SD

Le fichier `/ML_v500/backup.txt` (texte lisible, avec somme de contrôle) contient les réglages du contrôleur, toutes les présélections fréquence/position et l'état courant. L'ancienne version est conservée en `backup.bak`. L'écriture se fait dans une tâche séparée : le pas-à-pas n'est pas perturbé par la lenteur de la carte.

* **Manuel :** bouton `SD` > `SAUVEGARDER` ou `RESTAURER` (avec confirmation, puis redémarrage). La restauration reprend les réglages et les présélections ; la position courante du moteur est conservée.
* **Automatique (`SD_AUTOSAVE`) :** sauvegarde 10 s après le dernier changement des présélections ou réglages, et seulement si le moteur est à l'arrêt. Une première sauvegarde est créée si la carte n'en contient pas. Après un échec, nouvel essai au bout de 30 s.
* **Au démarrage (`SD_AUTORESTORE_BLANK`) :** si l'EEPROM est entièrement vierge (carte neuve, flash effacée) et qu'une sauvegarde valide existe, elle est restaurée, position moteur comprise. Un `$memorywipe` volontaire n'est pas annulé, car il ne vide pas l'EEPROM.
* **USB :** `$sdsave`, `$sdload`, `$sdstatus`.

Détails matériels : le chip select de la carte est la sortie EXIO4 du CH422G (pas une broche GPIO). `GPIO6` sert de CS factice à la bibliothèque SD (modifiable par `SD_DUMMY_CS_PIN` dans `ML.h`). Formater la carte en FAT32.

---

## 🔨 Compilation et Installation

### Waveshare ESP32-S3-Touch-LCD-7

* Bibliothèque à installer : **GFX Library for Arduino** (moononournation), version **1.6.x**. `SD`, `SPI`, `Wire`, `EEPROM` viennent avec le noyau ESP32.
* Réglages de l'IDE : carte *ESP32S3 Dev Module*, **PSRAM : OPI PSRAM** (obligatoire, le tampon d'image de 768 Ko y réside), **USB CDC On Boot : Enabled** (sinon `Serial` partage `GPIO43/44` avec le CAT de `Serial1`), taille de flash et schéma de partitions conformes à votre carte (schéma par défaut avec partition `eeprom`).
* Le bus RGB est réglé sur les valeurs officielles Waveshare (16 MHz, marges 4/8/8). Si l'image tremble ou dérive, réduire `GFX_PCLK_HZ` dans `ML.h`, ou mettre `GFX_BOUNCE_PX` à 0.
* `EEPROM.begin()` reçoit maintenant `EEPROM_TOTAL_BYTES` (2048) : 512 octets ne suffisaient pas pour 200 présélections.

### Teensy

1. **Prérequis matériels & logiciels :**
   - [Arduino IDE](https://www.arduino.cc/en/software) (version 1.8.x ou 2.x) avec l'extension [Teensyduino](https://www.pjrc.com/teensy/td_download.html).
   - Carte sélectionnée : **Teensy 4.1** (ou **Teensy 3.2/3.1**).

2. **Bibliothèques requises :**
   - `LiquidCrystalFast`
   - `Metro`
   - `Encoder`
   - `ADC`
   - `EEPROM`
   - `Wire` (Teensy 4.1) / `i2c_t3` (Teensy 3.x)

3. **Procédure de compilation :**
   - Ouvrir `ML_v410.ino` dans l'Arduino IDE.
   - Ajuster les paramètres souhaités dans `ML.h` (Type de driver moteur, type de radio, options SWR).
   - Sélectionner la carte : *Outils > Type de carte > Teensy 4.1*.
   - Cliquer sur **Vérifier / Compiler** puis télverser sur le Teensy via USB.

---

## 💬 Commandes USB (Série)

Le contrôleur expose un port série virtuel USB permettant le contrôle à distance. Toutes les commandes commencent par `$`.

### Commandes Générales & Fréquence
- `$frqget` : Renvoie la fréquence active en Hz.
- `$frqset <Hz>` : Définit la fréquence active (ex: `$frqset 14100000` pour 14.1 MHz).
- `$version` : Affiche la version et la date du firmware.
- `$help` : Affiche la liste d'aide des commandes disponibles.

### Gestion de la Mémoire EEPROM
- `$memoryget` : Exporte toutes les présélections enregistrées (`Index Fréquence Position`).
- `$memoryset <Index> <Fréquence_Hz> <Position>` : Enregistre une présélection.
- `$memoryclear` : Efface toutes les présélections fréquence/position.
- `$memorywipe` : Reconstitution complète de l'EEPROM (remise à zéro d'usine).
- `$sdsave` / `$sdload` / `$sdstatus` : (ESP32-S3) sauvegarde, restauration et état de la carte SD.

### SWR & Autotune
- `$swrtune` : Lance une procédure automatique d'accord SWR.
- `$swrtuneup` / `$swrtunedown` : Lance un accord SWR vers le haut ou vers le bas.
- `$swrtunestatus` : Renvoie le statut de l'accord (En cours, Succès, Échec, Pas de puissance).
- `$toggleautotune` : Active/désactive l'accord automatique sur ROS élevé.
- `$recalibrate` : Effectue un réalignement de position (recalibration).

### Télémétrie Wattmètre / ROS-mètre
- `$ppoll` : Rapport instantané unique de puissance et ROS.
- `$pinst`, `$ppk`, `$ppep`, `$plong` : Rapports sous forme lisible (instant, crête 100ms, PEP 1s, détaillé).
- `$pcont` : Mode de rapport continu à 10 Hz via USB.

---

## 📜 Licence

Ce projet est distribué sous la licence **GNU General Public License v3.0**. Consultez le fichier [`gpl.txt`](./gpl.txt) pour plus de détails.
