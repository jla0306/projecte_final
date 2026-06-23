# Projecte Final — Consola Retro UPC

**Assignatura:** Processadors Digitals — Enginyeria de Sistemes Audiovisuals  
**Escola:** ESEIAAT — UPC  
**Autors:** Julio Lázaro Alcobendas i Gerard Rodríguez Gonzalez  
**Data:** Juny 2026  
**Repositori GitHub:** [https://github.com/gedrar/Projecte-Final](https://github.com/gedrar/Projecte-Final)

---

## 1. Introducció

Aquest projecte consisteix en el disseny i implementació d'una **consola de videojocs retro portàtil** basada en el microcontrolador **ESP32-S3**, programada en C++ amb el framework Arduino sota PlatformIO amb VS Code.

El sistema integra set minijocs clàssics —Tetris, Space Invaders, Snake, Pong, Pac-Man, Breakout i Frogger— amb àudio real reproduït des d'una targeta microSD, pantalla TFT de 1,8 polzades, control per joystick analògic i dos botons (SELECT i BACK). Les puntuacions màximes es desen de forma persistent mitjançant la biblioteca `Preferences` de l'ESP32 (memòria flash interna NVS), sense necessitat d'accedir a la SD per a puntuacions.

Des del punt de vista tècnic, el projecte aborda diverses problemàtiques de processadors digitals: **arquitectura dual-core** del ESP32-S3 amb FreeRTOS, on la música de fons corre al Core 0 mentre el joc s'executa al Core 1; **sincronització** entre tasques mitjançant un mutex i semàfor binari per a l'accés concurrent a la microSD; **reproducció d'àudio PCM** via el perifèric I2S amb l'amplificador digital MAX98357A; i gestió de dos busos SPI, un per al TFT i un altre dedicat per a la SD.

El material es va adquirir per duplicat perquè cada membre de l'equip pogués treballar en paral·lel i disposar d'un kit de recanvi en cas de fallada de component.

---

## 2. Components de maquinari

### 2.1 Microcontrolador

**Espressif ESP32-S3-DevKitC-1** — placa de desenvolupament oficial amb el SoC ESP32-S3, 240 MHz dual-core Xtensa LX7, 8 MB de Flash, Wi-Fi i BLE. S'utilitza en mode Arduino via PlatformIO.

### 2.2 Pantalla TFT

**Mòdul LCD TFT 1,8" IPS 7 pins SPI — controlador ST7735, resolució 128×160 px**

### 2.3 Amplificador I2S i altaveu

**Mòdul MAX98357A — amplificador I2S Classe D, 3 W**

**Altaveu quadrat 8Ω 2W**

El MAX98357A rep l'àudio en format I2S des del ESP32-S3 i l'amplifica directament per a l'altaveu. El pin `SD` del mòdul es connecta a 3V3 per activar-lo de forma permanent.

### 2.4 Mòdul lector microSD

**Mòdul lector microSD SPI amb convertidor de nivell integrat**

Connectat a un bus SPI dedicat i independent del TFT (GPIO 35/36/37/39).

### 2.5 Joystick analògic

**Mòdul Joystick Mini XY doble eix — PSP style, 3V–5V**

### 2.6 Botons

**5 uds PBS-110 botó rodó momentani ON/OFF amb cable presoldado 20 cm**

Dos botons utilitzen el pullup intern del ESP32-S3 (`INPUT_PULLUP`). No requereixen resistència externa.

### 2.7 Connectors i protoboard

- **Cables Dupont F-F, M-M i F-M de 20 cm** (40 pins cada pack × 2)
- **Protoboard** — S'utilitza per distribuir els raïls de 3V3 i GND. Connectem un cable 3V3 i un GND des de la ESP32-S3 a la protoboard, de manera que tota la corrent i massa es redistribueixen des d'aquí cap a la resta de components. Això és necessari perquè la ESP32-S3 no disposa de suficients pins de 3V3 per alimentar tots els perifèrics del projecte directament, i a més ens aporta comoditat en el cablejat i evita sobrecarregar els pins d'alimentació de la placa.

### 2.8 Pressupost total

| Component | Preu unit. | Quantitat | Total |
|---|---|---|---|
| Pantalla TFT 1,8" ST7735 | 4,09 € | 2 | 8,18 € |
| Amplificador MAX98357A | 2,81 € | 2 | 5,62 € |
| Altaveu 8Ω 2W | 0,99 € | 1 | 0,99 € |
| Mòdul lector microSD | 0,80 € | 2 | 1,60 € |
| Joystick analògic XY | 3,29 € | 1 | 3,29 € |
| Botons PBS-110 (pack 5) | 3,59 € | 2 | 7,18 € |
| Cables Dupont + protoboard | ~5,00 € | — | ~5,00 € |
| **Total** | | | **~31,86 €** |

> L'ESP32-S3 no s'inclou al pressupost perquè és material de pràctiques de l'assignatura.

---

## 3. Notes tècniques importants

### 3.1 Formatat de la microSD

Per al correcte funcionament de la microSD amb la biblioteca SdFat, **és imprescindible formatar la targeta amb el programa oficial [SD Card Formatter](https://www.sdcard.org/downloads/formatter/)** de la SD Association. El formatat que fa Windows per defecte no és suficientment correcte i pot causar errors d'inicialització o corrupció de fitxers. SD Card Formatter garanteix el formatat FAT32 o exFAT correcte segons la capacitat de la targeta.

### 3.2 Format dels arxius d'àudio

Els arxius WAV han estat preparats amb **Audacity** en format **estèreo a 22050 Hz**, PCM 16-bit. S'ha triat específicament aquest format per evitar delays i errors de reproducció en temps real: una freqüència de mostreig massa alta o un format incorrecte pot causar bloqueigs en la tasca d'àudio o artifacts sonors durant l'execució dels jocs.

### 3.3 Noms dels pins de la pantalla TFT al codi

Els pins de la pantalla TFT al fitxer `main.cpp` (`SPI_SCK`, `SPI_MISO`, `SPI_MOSI`) no coincideixen amb els noms del pinout (`CLK`, `SDA`). Això és degut a que la biblioteca `#include <SPI.h>` requereix que els pins del bus SPI estiguin definits amb els noms estàndard del protocol SPI (`SCK`, `MISO`, `MOSI`), mentre que el pinout del mòdul TFT usa la nomenclatura pròpia del fabricant. La correspondència és: `SDA` = `MOSI` (GPIO 11), `CLK` = `SCK` (GPIO 12).

### 3.4 Alimentació del mòdul lector microSD: soldadura al regulador

L'ESP32-S3 no disposa de sortida de 5V als seus pins d'alimentació, però el regulador de tensió integrat al mòdul lector microSD requereix una tensió d'entrada superior als 3,3V per operar correctament. Per solucionar-ho, hem hagut de **soldar un cable directament al regulador del mòdul**, de manera que li proporcionem el voltatge necessari. Al mateix temps, hem mantingut la connexió de 3V3 des del raïl de la protoboard al pin VCC del mòdul, tal com indica el pinout. Aquesta combinació permet que el regulador intern del mòdul rebi prou tensió per funcionar, mentre que la lògica del mòdul s'alimenta correctament a 3,3V.

---

## 4. Taula de connexions (pin_out)

> Connectar un cable 3V3 i un GND de la ESP32-S3 a la protoboard per alimentar-la. No hi ha suficients pins d'alimentació a la placa per connectar tots els components directament.

| Component | Pin del component | GPIO ESP32-S3 / Alimentació |
|---|---|---|
| **Pantalla TFT** | VCC | Raïl 3V3 |
| | GND | Raïl GND |
| | SDA | GPIO 11 |
| | CLK | GPIO 12 |
| | CS | GPIO 10 |
| | RS | GPIO 9 |
| | RST | GPIO 14 |
| **Amplificador I2S MAX98357A** | VIN | Raïl 3V3 |
| | GND | Raïl GND |
| | SD | Raïl 3V3 (activació permanent) |
| | LRC | GPIO 15 (I2S WS) |
| | BCLK | GPIO 16 (I2S BCK) |
| | DIN | GPIO 17 (I2S DOUT) |
| | OUT+ / OUT− | Borns de l'altaveu (8Ω 2W) |
| **Lector microSD** | VCC | Raïl 3V3 |
| | GND | Raïl GND |
| | CS | GPIO 39 |
| | MOSI | GPIO 35 |
| | SCK | GPIO 36 |
| | MISO | GPIO 37 |
| **Joystick analògic** | VCC | Raïl 3V3 |
| | GND | Raïl GND |
| | VRx (X) | GPIO 4 (JOY_X) |
| | VRy (Y) | GPIO 5 (JOY_Y) |
| **Botó SELECT** | Pin 1 | GPIO 18 (INPUT_PULLUP) |
| | Pin 2 | Raïl GND |
| **Botó BACK** | Pin 1 | GPIO 19 (INPUT_PULLUP) |
| | Pin 2 | Raïl GND |

---

## 5. Arxius de so (microSD)

Els fitxers WAV han de col·locar-se a l'**arrel** de la targeta microSD. Format: PCM 16-bit, estèreo, 22050 Hz (preparat amb Audacity).

| Fitxer | Esdeveniment |
|---|---|
| `intro.wav` | Música pantalla d'inici |
| `click.wav` | So de navegació / confirmació al menú |
| `Game_Selection.wav` | So de selecció de joc |
| `gameover.wav` | Pantalla de game over genèrica |
| `launch.wav` | Inici de partida |
| `powerup.wav` | Power-up genèric |
| `score.wav` | Puntuació / event genèric |
| `PacMan_Leitmotiv.wav` | Melodia inici Pac-Man |
| `PacMan_Waka.wav` | So menjar punt |
| `PacMan_Powerup.wav` | Power pellet Pac-Man |
| `PacMan_GameOver.wav` | Mort Pac-Man |
| `PacMan_Intermission.wav` | Animació entre fases |
| `Frogger_Point.wav` | Arribada a meta del Frogger |
| `Frogger_Death.wav` | So de mort del Frogger |
| `Pong_point_lost.wav` | Punt perdut al Pong |
| `Pong_wall_bounce.wav` | Rebot de la pilota a la paret del Pong |
| `Tetris_leitmotiv.wav` | Melodia principal del Tetris (Korobeiniki) |

---

## 6. Configuració PlatformIO (`platformio.ini`)

```ini
[env:esp32-s3-devkitc-1]
platform = espressif32 @ 6.6.0
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200
lib_deps =
    bodmer/TFT_eSPI @ ^2.5.43
    greiman/SdFat @ ^2.3.1
build_flags =
    -D ARDUINO_USB_MODE=1
    -D ARDUINO_USB_CDC_ON_BOOT=0
    -D USER_SETUP_LOADED=1
    -D ST7735_DRIVER=1
    -D TFT_WIDTH=128
    -D TFT_HEIGHT=160
    -D ST7735_BLACKTAB=1
    -D TFT_MOSI=11
    -D TFT_MISO=13
    -D TFT_SCLK=12
    -D TFT_CS=10
    -D TFT_DC=9
    -D TFT_RST=14
    -D TFT_SPI_PORT=2
    -D LOAD_GLCD=1
    -D LOAD_FONT2=1
    -D LOAD_FONT4=1
    -D SPI_FREQUENCY=27000000
```

**Dependències:**
- `TFT_eSPI` — driver de pantalla ST7735 configurat via build flags sense fitxer `User_Setup.h` extern.
- `SdFat` — accés a la microSD amb suport FAT32 i exFAT, API `SdFs` / `FsFile`.

---

## 7. Arquitectura del programari: Dual-Core, Mutex i Semàfor

### 7.1 Arquitectura Dual-Core del ESP32-S3

L'ESP32-S3 disposa de **dos nuclis Xtensa LX7** que operen a 240 MHz i poden executar tasques en paral·lel de forma autèntica. FreeRTOS, el sistema operatiu en temps real integrat al framework Arduino per a ESP32, permet assignar tasques específiques a cada nucli:

```
┌─────────────────────────────┐    ┌─────────────────────────────┐
│         CORE 1              │    │         CORE 0              │
│   (Arduino loop principal)  │    │   (Tasques FreeRTOS àudio)  │
├─────────────────────────────┤    ├─────────────────────────────┤
│  setup() / loop()           │    │  bgmTask()                  │
│  Menú principal             │    │  └─ Llegeix WAV de la SD    │
│  Lògica dels 7 jocs         │    │  └─ Reprodueix en bucle     │
│  Renderitzat TFT            │    │       per I2S               │
│  playSound() → playSFX()    │    │                             │
│  llegirHighScore()          │    │  SFX_Task()                 │
│  guardarHighScore()         │    │  └─ Rep nom de fitxer WAV   │
└─────────────────────────────┘    │  └─ Reprodueix efecte       │
            │                      └──────────────────────────────┘
            │                                    │
            └──────── sdMutex ───────────────────┘
                  (Protegeix accés concurrent a la SD)
```

La tasca de música de fons i la tasca de SFX s'inicien al `setup()`:

```cpp
xTaskCreatePinnedToCore(bgmTask,  "BGM_Task", 4096, NULL, 1, NULL, 0); // Core 0
xTaskCreatePinnedToCore(SFX_Task, "SFX_Task", 4096, NULL, 2, NULL, 1); // Core 1
```

Això permet que la música soni de forma contínua mentre el joc actualitza la pantalla i gestiona l'entrada del joystick, tot en paral·lel.

### 7.2 El Problema de la Concurrència

Amb dues tasques d'àudio (`bgmTask` al Core 0 i `SFX_Task` al Core 1) accedint ambdues a la microSD via el mateix bus SPI, es produiria una condició de carrera sense control: les dues transferències SPI es mesclarien en el bus, corrompent les dades d'àudio i penjant el sistema.

### 7.3 Semàfor i Mutex: la Solució

Un **semàfor** és un mecanisme de sincronització entre tasques que controla l'accés a un recurs compartit. Un **mutex** (mutual exclusion) és un semàfor binari especial: només la tasca que l'ha adquirit pot alliberar-lo, evitant alliberaments accidentals.

Al projecte s'utilitza un mutex de FreeRTOS per protegir l'accés a la SD:

```cpp
SemaphoreHandle_t sdMutex = NULL;

// Al setup():
sdMutex = xSemaphoreCreateMutex();
```

Abans de qualsevol operació amb la SD, la tasca ha d'adquirir el mutex:

```cpp
if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    // --- Accés exclusiu a la SD ---
    FsFile file = sd.open(filename);
    xSemaphoreGive(sdMutex); // Alliberar immediatament després d'obrir
    // ... llegir dades i enviar per I2S (no usa el bus SPI) ...
    xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100));
    file.close();
    xSemaphoreGive(sdMutex);
}
```

Si una tasca té el mutex i l'altra intenta adquirir-lo, la segona **s'atura i espera** fins que la primera l'alliberi o fins al timeout. D'aquesta manera l'accés al bus SPI de la SD és sempre mutualment exclusiu.

### 7.4 Cancel·lació d'Àudio i Cua de Missatges

La comunicació entre Core 1 (joc, que vol reproduir un SFX) i la tasca SFX_Task es fa via una **cua de FreeRTOS**:

```cpp
audioQueue = xQueueCreate(5, sizeof(const char*));
```

Quan el joc vol un efecte de so, simplement envia el nom del fitxer a la cua sense bloquejar-se:

```cpp
void playSFX(const char* filename) {
    cancelAudio = true;       // Talla el so en curs
    xQueueReset(audioQueue);  // Buida la cua
    xQueueSend(audioQueue, &filename, 0); // Envia el nou so
}
```

La variable `volatile bool cancelAudio` actua com a senyal d'interrupció entre nuclis: la tasca SFX la comprova en cada chunk de lectura i atura la reproducció immediatament si és `true`.

---

## 8. Codi font complet (`main.cpp`)

```cpp
#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s.h>
#include <Preferences.h>

// ============================================================================
// CONFIGURACIÓ DE PINS
// ============================================================================
// Pins bus SPI de la SD (bus dedicat, independent del TFT)
#define SCK_PIN  36
#define MISO_PIN 37
#define MOSI_PIN 35
#define CS_PIN   39

// Controls i Entrades
#define JOY_X      4
#define JOY_Y      5
#define BTN_SELECT 18
#define BTN_BACK   19

// Pins I2S (Àudio)
#define I2S_LRC  15
#define I2S_BCLK 16
#define I2S_DOUT 17
#define I2S_NUM  (i2s_port_t)0

// ============================================================================
// OBJECTES GLOBALS I ESTRUCTURES
// ============================================================================
TFT_eSPI    tft = TFT_eSPI();
Preferences preferences;

// Bus SPI dedicat per a la SD (FSPI — en ESP32-S3 no existeix HSPI)
// NOTA: els pins del TFT al codi (SPI_SCK=12, SPI_MOSI=11) no coincideixen
// amb els noms del pin_out (CLK, SDA) perquè la biblioteca <SPI.h> requereix
// la nomenclatura estàndard del protocol SPI.
SPIClass spiSD(FSPI);

struct Point { int x, y; };

const int   TOTAL_GAMES = 7;
const char* gameNames[] = {
    "1. TETRIS", "2. INVADERS", "3. LA SERP", "4. PONG",
    "5. PAC-MAN", "6. BREAKOUT", "7. FROGGER"
};
int  seleccionat = 0;
bool updateMenu  = true;

enum EstatConsola { ESTAT_INICI, ESTAT_MENU, ESTAT_JUGANT };
EstatConsola estatActual = ESTAT_INICI;

unsigned long tempsAnteriorParpelleig = 0;
const long    intervalParpelleig      = 500;
bool          textVisible             = true;

// Variables àudio
QueueHandle_t bgmQueue    = NULL;
QueueHandle_t sfxQueue    = NULL;
volatile bool isSfxPlaying = false;
volatile bool bgmStop      = false;
volatile bool cancelAudio  = false;
QueueHandle_t audioQueue   = NULL;
File          bgmFile;

// Mutex per protegir l'accés concurrent a la SD entre bgmTask i SFX_Task
SemaphoreHandle_t sdMutex = NULL;

// Declaració funcions jocs
void playTetris();
void playSpaceInvaders();
void playSnake();
void playPong();
void playPacman();
void playBreakout();
void playFrogger();

// ============================================================================
// GESTIÓ DE PUNTUACIONS (Preferences — memòria flash interna, no usa la SD)
// ============================================================================
int llegirHighScore(const char* joc) {
    preferences.begin("retroconsole", true);
    int hs = preferences.getInt(joc, 0);
    preferences.end();
    return hs;
}

void guardarHighScore(const char* joc, int hs) {
    preferences.begin("retroconsole", false);
    preferences.putInt(joc, hs);
    preferences.end();
}

// ============================================================================
// MOTOR D'ÀUDIO I2S
// Els WAVs estan preparats amb Audacity: estèreo, 22050 Hz, PCM 16-bit.
// Aquest format evita delays i errors de reproducció en temps real.
// ============================================================================
void initI2S() {
    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate          = 22050,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format       = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 256,
        .use_apll             = false,
        .tx_desc_auto_clear   = true
    };
    i2s_pin_config_t pins = {
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM, &pins);
    i2s_zero_dma_buffer(I2S_NUM);
}

// Tasca SFX — rep noms de fitxer per la cua i els reprodueix
// Usa sdMutex per protegir l'accés a la SD
void SFX_Task(void* pvParameters) {
    while (1) {
        const char* filename;
        if (xQueueReceive(audioQueue, &filename, portMAX_DELAY)) {
            cancelAudio = false;
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                File file = SD.open(filename);
                xSemaphoreGive(sdMutex);
                if (file) {
                    uint8_t buffer[1024]; size_t bytesWritten;
                    while (file.available() && !cancelAudio) {
                        size_t bytesRead = file.read(buffer, sizeof(buffer));
                        if (bytesRead == 0) break;
                        i2s_write(I2S_NUM, buffer, bytesRead, &bytesWritten, portMAX_DELAY);
                    }
                    file.close();
                    i2s_zero_dma_buffer(I2S_NUM);
                } else {
                    Serial.printf("No s'ha trobat l'audio: %s\n", filename);
                }
            }
        }
    }
}

// playSFX: no bloqueja el joc; envia el nom del fitxer a la cua i torna
void playSFX(const char* filename) {
    cancelAudio = true;
    if (audioQueue != NULL) {
        xQueueReset(audioQueue);
        xQueueSend(audioQueue, &filename, 0);
    }
}

// Tasca BGM — Core 0, reprodueix música de fons en bucle
void bgmTask(void* pv) {
    char path[32];
    for (;;) {
        if (xQueueReceive(bgmQueue, path, portMAX_DELAY) != pdTRUE) continue;
        if (path[0] == '\0') continue;
        bgmStop = false;
        while (!bgmStop) {
            bgmFile = SD.open(path);
            if (!bgmFile) break;
            uint8_t hdr[44];
            if (bgmFile.read(hdr, 44) != 44) { bgmFile.close(); break; }
            uint32_t sr  = hdr[24]|(hdr[25]<<8)|(hdr[26]<<16)|(hdr[27]<<24);
            uint16_t ch  = hdr[22]|(hdr[23]<<8);
            uint16_t bps = hdr[34]|(hdr[35]<<8);
            i2s_channel_fmt_t fmt = (ch==1)?I2S_CHANNEL_FMT_ONLY_LEFT:I2S_CHANNEL_FMT_RIGHT_LEFT;
            i2s_set_clk(I2S_NUM, sr, (i2s_bits_per_sample_t)bps, (i2s_channel_t)fmt);
            uint8_t buf[512]; size_t wr = 0;
            while (bgmFile.available() && !bgmStop) {
                int n = bgmFile.read(buf, sizeof(buf));
                if (n <= 0) break;
                if (!isSfxPlaying) i2s_write(I2S_NUM, buf, (size_t)n, &wr, portMAX_DELAY);
                else vTaskDelay(pdMS_TO_TICKS(5));
            }
            bgmFile.close();
            if (bgmStop) break;
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

void playBGM(const char* filepath) {
    bgmStop = true;
    vTaskDelay(pdMS_TO_TICKS(80));
    char path[32];
    strncpy(path, filepath, sizeof(path)-1); path[sizeof(path)-1]='\0';
    xQueueOverwrite(bgmQueue, path);
}
void stopBGM() { bgmStop = true; }

// Traductor playSound(id) → playSFX("/fitxer.wav")
void playSound(int id) {
    switch(id) {
        case 1: playSFX("/click.wav");    break;
        case 2: playSFX("/score.wav");    break;
        case 3: playSFX("/launch.wav");   break;
        case 4: playSFX("/gameover.wav"); break;
        case 5: playSFX("/powerup.wav");  break;
        case 6: playSFX("/score.wav");    break;
        default: playSFX("/click.wav");   break;
    }
}

// ============================================================================
// HELPER: sprites bitmap 16-bit
// ============================================================================
void drawSprite(int x, int y, const uint16_t* sprite, int w, int h, uint16_t color, bool erase = false) {
    for (int i = 0; i < h; i++) {
        uint16_t row = sprite[i];
        for (int j = 0; j < w; j++)
            if (row & (1 << (15 - j)))
                tft.drawPixel(x + j, y + i, erase ? TFT_BLACK : color);
    }
}

// ============================================================================
// INTERFÍCIE: SPLASH I MENÚ
// ============================================================================
void mostrarSplashScreen() {
    tft.fillScreen(TFT_BLACK);
    tft.drawRect(2, 2, 156, 124, TFT_WHITE);
    tft.setTextSize(2);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.setCursor(30, 15); tft.print("RETRO UPC");
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextSize(1);
    tft.setCursor(55, 35); tft.print("S3 CORE");
    tft.fillTriangle(80,50, 60,80, 100,80, TFT_RED);
    tft.fillTriangle(60,65, 46,80, 60,80, TFT_BLUE);
    tft.fillTriangle(100,65, 114,80, 100,80, TFT_BLUE);
    tft.fillCircle(80, 85, 5, TFT_YELLOW);
}

void mostrarMenu() {
    tft.fillScreen(TFT_BLACK);
    tft.fillRect(0, 0, 160, 16, TFT_NAVY);
    tft.setTextColor(TFT_WHITE);
    tft.drawString(" ARCADIA OS S3 ", 12, 2, 1);
}

void dibuixarLlistaJocs() {
    tft.fillRect(0, 16, 160, 112, TFT_BLACK);
    tft.setTextSize(1);
    for (int i = 0; i < TOTAL_GAMES; i++) {
        if (i == seleccionat) { tft.setTextColor(TFT_BLACK, TFT_YELLOW); }
        else { tft.setTextColor(TFT_WHITE, TFT_BLACK); }
        tft.drawString(gameNames[i], 10, 25 + (i * 12));
    }
}

void aturarMusicaSegur() { bgmStop = true; delay(50); }

void reproduirMusicaSegur(const char* canco) {
    bgmStop = true; delay(50);
    char track[32]; strncpy(track, canco, sizeof(track));
    xQueueOverwrite(bgmQueue, &track);
    bgmStop = false;
}

// ============================================================================
// SETUP
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    // TFT — init() és el mètode correcte per a TFT_eSPI, no begin()
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    pinMode(BTN_SELECT, INPUT_PULLUP);
    pinMode(BTN_BACK,   INPUT_PULLUP);

    mostrarSplashScreen();
    estatActual = ESTAT_INICI;

    // SD — bus SPI dedicat (FSPI). IMPORTANT: formatar amb SD Card Formatter,
    // no amb Windows, per garantir FAT32/exFAT correcte.
    // FSPI és el nom correcte en ESP32-S3; HSPI no existeix en aquest xip.
    spiSD.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
    if (!SD.begin(CS_PIN, spiSD, 1000000)) {
        Serial.println("Error: No es pot inicialitzar la SD.");
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.setCursor(10, 60); tft.print("ERROR SD!");
    } else {
        Serial.println("SD Inicialitzada correctament!");
    }

    initI2S();

    // Mutex per protegir accés concurrent a la SD des de bgmTask i SFX_Task
    sdMutex    = xSemaphoreCreateMutex();
    bgmQueue   = xQueueCreate(1, 32);
    sfxQueue   = xQueueCreate(5, 32);
    audioQueue = xQueueCreate(5, sizeof(const char*));

    xTaskCreatePinnedToCore(bgmTask,  "BGM_Task", 4096, NULL, 1, NULL, 0); // Core 0
    xTaskCreatePinnedToCore(SFX_Task, "SFX_Task", 4096, NULL, 2, NULL, 1); // Core 1

    reproduirMusicaSegur("/intro.wav");
}

// ============================================================================
// LOOP PRINCIPAL
// ============================================================================
void loop() {
    unsigned long tempsActual = millis();
    int joyY;

    switch (estatActual) {
        case ESTAT_INICI:
            if (tempsActual - tempsAnteriorParpelleig >= intervalParpelleig) {
                tempsAnteriorParpelleig = tempsActual;
                textVisible = !textVisible;
                tft.setTextSize(1);
                if (textVisible) {
                    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
                    tft.setCursor(17, 105);
                    tft.print("Prem SELECT per Start");
                } else {
                    tft.fillRect(15, 105, 135, 10, TFT_BLACK);
                }
            }
            if (digitalRead(BTN_SELECT) == LOW) {
                delay(50); while(digitalRead(BTN_SELECT) == LOW); delay(50);
                aturarMusicaSegur();
                mostrarMenu(); dibuixarLlistaJocs();
                estatActual = ESTAT_MENU;
            }
            break;

        case ESTAT_MENU:
            if (digitalRead(BTN_BACK) == LOW) {
                delay(50); while(digitalRead(BTN_BACK) == LOW); delay(50);
                mostrarSplashScreen();
                reproduirMusicaSegur("/intro.wav");
                estatActual = ESTAT_INICI;
            }
            joyY = analogRead(JOY_Y);
            if (joyY > 3000) { delay(150); seleccionat=(seleccionat+1)%TOTAL_GAMES; dibuixarLlistaJocs(); }
            else if (joyY < 1000) { delay(150); seleccionat=(seleccionat-1+TOTAL_GAMES)%TOTAL_GAMES; dibuixarLlistaJocs(); }
            if (digitalRead(BTN_SELECT) == LOW) {
                delay(50); while(digitalRead(BTN_SELECT) == LOW); delay(50);
                estatActual = ESTAT_JUGANT;
                if      (seleccionat == 0) playTetris();
                else if (seleccionat == 1) playSpaceInvaders();
                else if (seleccionat == 2) playSnake();
                else if (seleccionat == 3) playPong();
                else if (seleccionat == 4) playPacman();
                else if (seleccionat == 5) playBreakout();
                else if (seleccionat == 6) playFrogger();
                mostrarMenu(); dibuixarLlistaJocs();
                estatActual = ESTAT_MENU;
            }
            break;

        case ESTAT_JUGANT:
            break;
    }
}

// Els 7 jocs complets es troben al fitxer del repositori GitHub.
// Aquí es mostra l'estructura i les funcions de la infraestructura.
```
