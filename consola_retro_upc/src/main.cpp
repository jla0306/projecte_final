#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <SD.h>
#include <driver/i2s.h>
#include <Preferences.h>

// ============================================================================
// CONFIGURACIÓ DE PINS
// ============================================================================
// Pins exactes per al bus SPI de la SD (Bus 2)
#define SCK_PIN 36
#define MISO_PIN 37
#define MOSI_PIN 35
#define CS_PIN 39

// Controls i Entrades
#define JOY_X 4
#define JOY_Y 5
#define BTN_SELECT 18
#define BTN_BACK 19

// Pins I2S (Àudio)
#define I2S_LRC 15
#define I2S_BCLK 16
#define I2S_DOUT 17
#define I2S_NUM (i2s_port_t)0

// ============================================================================
// OBJECTES GLOBALS I ESTRUCTURES
// ============================================================================

SemaphoreHandle_t sdMutex = NULL;

TFT_eSPI tft = TFT_eSPI();
Preferences preferences;

// Creem un objecte SPI exclusiu per a la SD (Bus HSPI)
SPIClass spiSD(HSPI);

struct Point { int x, y; };

const int TOTAL_GAMES = 7;
const char* gameNames[] = {
  "1. TETRIS", "2. INVADERS", "3. LA SERP", "4. PONG", 
  "5. PAC-MAN", "6. BREAKOUT", "7. FROGGER"
};
int seleccionat = 0;
bool updateMenu = true;
// Variables per a la Pantalla d'Inici i Estats
enum EstatConsola { ESTAT_INICI, ESTAT_MENU, ESTAT_JUGANT };
EstatConsola estatActual = ESTAT_INICI;

unsigned long tempsAnteriorParpelleig = 0;
const long intervalParpelleig = 500; // temps en milisegons
bool textVisible = true;

// Variables per l'àudio (Sense spiMutex)
QueueHandle_t bgmQueue = NULL;
QueueHandle_t sfxQueue = NULL;
volatile bool isSfxPlaying = false;
volatile bool bgmStop = false;
volatile bool cancelAudio = false; 
QueueHandle_t audioQueue = NULL; // La teva cua principal d'àudio
File bgmFile;

// Declaració funcions jocs
void playTetris();
void playSpaceInvaders();
void playSnake();
void playPong();
void playPacman();
void playBreakout();
void playFrogger();

// ============================================================================
// GESTIÓ DE PUNTUACIONS (Preferences)
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
// MOTOR D'ÀUDIO I2S I LECTORS WAV (Depurat)
// ============================================================================
void initI2S() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 22050,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };
    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    i2s_driver_install(I2S_NUM, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM, &pins);
    i2s_zero_dma_buffer(I2S_NUM);
}
void SFX_Task(void *pvParameters) {
    while (1) {
        const char* filename;
        if (xQueueReceive(audioQueue, &filename, portMAX_DELAY)) {
            cancelAudio = false;
            isSfxPlaying = true; // <-- AVISEN A LA MÚSICA
            
            xSemaphoreTake(sdMutex, portMAX_DELAY);
            File file = SD.open(filename);
            xSemaphoreGive(sdMutex);
            
            if (file) {
                uint8_t buffer[1024];
                while (!cancelAudio) { // <-- Bucle protegit
                    xSemaphoreTake(sdMutex, portMAX_DELAY);
                    bool avail = file.available();
                    size_t bytesRead = 0;
                    if(avail) bytesRead = file.read(buffer, sizeof(buffer));
                    xSemaphoreGive(sdMutex);
                    
                    if (!avail || bytesRead == 0) break;
                    
                    size_t bytesWritten;
                    i2s_write(I2S_NUM, buffer, bytesRead, &bytesWritten, portMAX_DELAY);
                }
                
                xSemaphoreTake(sdMutex, portMAX_DELAY);
                file.close();
                xSemaphoreGive(sdMutex);
                
                i2s_zero_dma_buffer(I2S_NUM);
            }
            isSfxPlaying = false; // <-- VIA LLIURE A LA MÚSICA
        }
    }
}

// Ara playSFX és super ràpid: només envia el nom de l'arxiu i torna al joc a l'instant!
void playSFX(const char* filename) {
    cancelAudio = true; // 1. Donem l'ordre de tallar la música a l'altre nucli
    if (audioQueue != NULL) { 
        xQueueReset(audioQueue); // 2. Buidem qualsevol so encallat a la cua
        xQueueSend(audioQueue, &filename, 0); // 3. Enviem la nova cançó (segur)
    }
}

void bgmTask(void* pv) {
    char path[32];
    for (;;) {
        if (xQueueReceive(bgmQueue, path, portMAX_DELAY) != pdTRUE) continue;
        if (path[0] == '\0') continue;
        bgmStop = false;
        while (!bgmStop) {
            xSemaphoreTake(sdMutex, portMAX_DELAY);
            bgmFile = SD.open(path);
            xSemaphoreGive(sdMutex);
            
            if (!bgmFile) { vTaskDelay(pdMS_TO_TICKS(100)); break; }
            
            // --- LECTOR INTEL·LIGENT (Amb Mutex) ---
            uint8_t hdr[44];
            xSemaphoreTake(sdMutex, portMAX_DELAY);
            int readBytes = bgmFile.read(hdr, 44);
            xSemaphoreGive(sdMutex);

            if (readBytes != 44) {
                xSemaphoreTake(sdMutex, portMAX_DELAY);
                bgmFile.close();
                xSemaphoreGive(sdMutex);
                break;
            }

            uint32_t sr = hdr[24] | (hdr[25] << 8) | (hdr[26] << 16) | (hdr[27] << 24);
            uint16_t ch = hdr[22] | (hdr[23] << 8);
            uint16_t bps = hdr[34] | (hdr[35] << 8);
            i2s_channel_fmt_t fmt = (ch == 1) ? I2S_CHANNEL_FMT_ONLY_LEFT : I2S_CHANNEL_FMT_RIGHT_LEFT;

            i2s_set_clk(I2S_NUM, sr, (i2s_bits_per_sample_t)bps, (i2s_channel_t)fmt);
            // ---------------------------------------
            
            uint8_t buf[512];
            size_t wr = 0;
            while (!bgmStop) {
                xSemaphoreTake(sdMutex, portMAX_DELAY);
                bool avail = bgmFile.available();
                int n = 0;
                if(avail) n = bgmFile.read(buf, sizeof(buf));
                xSemaphoreGive(sdMutex);
                
                if (!avail || n <= 0) break;
                
                // --- EVITAR DISTORSIÓ ---
                if (!isSfxPlaying) {
                    i2s_write(I2S_NUM, buf, (size_t)n, &wr, portMAX_DELAY);
                } else {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            
            xSemaphoreTake(sdMutex, portMAX_DELAY);
            bgmFile.close();
            xSemaphoreGive(sdMutex);
            
            if (bgmStop) break;
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }
}

void playBGM(const char* filepath) {
    bgmStop = true;
    vTaskDelay(pdMS_TO_TICKS(80)); 
    char path[32];
    strncpy(path, filepath, sizeof(path)-1);
    path[sizeof(path)-1] = '\0';
    xQueueOverwrite(bgmQueue, path);
}

void stopBGM() {
    bgmStop = true;
}

// ============================================================================
// INTERFÍCIE D'INICI I MENÚ
// ============================================================================
void mostrarSplashScreen() {
  tft.fillScreen(TFT_BLACK);

  // Marc exterior per a 160x128 (Mode Apaïsat)
  tft.drawRect(2, 2, 156, 124, TFT_WHITE);

  // Títol centrat
  tft.setTextSize(2);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  tft.setCursor(30, 15);
  tft.print("RETRO-UPC");

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextSize(1);
  tft.setCursor(55, 35);
  tft.print("S3 CORE");

  // Dibuixets retro (Nau espacial al centre)
  tft.fillTriangle(80, 50, 60, 80, 100, 80, TFT_RED);     // Cos
  tft.fillTriangle(60, 65, 46, 80, 60, 80, TFT_BLUE);     // Ala esquerra
  tft.fillTriangle(100, 65, 114, 80, 100, 80, TFT_BLUE);  // Ala dreta
  tft.fillCircle(80, 85, 5, TFT_YELLOW);                  // Propulsor
}

void mostrarMenu() {
  tft.fillScreen(TFT_BLACK);
  tft.fillRect(0, 0, 160, 16, TFT_NAVY);
  tft.setTextColor(TFT_WHITE);
  tft.drawString(" ARCADIA OS S3 ", 12, 2, 1);
}
void dibuixarLlistaJocs() {
  // Netejar només la llista (evitem esborrar el títol de dalt)
  tft.fillRect(0, 16, 160, 112, TFT_BLACK);
  
  tft.setTextSize(1);
  int startY = 25; // On comencem a llistar
  
  for (int i = 0; i < TOTAL_GAMES; i++) {
    if (i == seleccionat) {
      tft.setTextColor(TFT_BLACK, TFT_YELLOW); // El joc marcat surt groc
      tft.drawString(gameNames[i], 10, startY + (i * 12));
    } else {
      tft.setTextColor(TFT_WHITE, TFT_BLACK);
      tft.drawString(gameNames[i], 10, startY + (i * 12));
    }
  }
}
// ============================================================================
// FUNCIONS D'AJUDA I COMPATIBILITAT (Afegit per solucionar errors)
// ============================================================================

// 1. Funció de dibuix de Sprites que faltava
void drawSprite(int x, int y, const uint16_t* sprite, int w, int h, uint16_t color, bool erase = false) {
    for(int i = 0; i < h; i++) {
        uint16_t row = sprite[i];
        for(int j = 0; j < w; j++) {
            if(row & (1 << (15 - j))) {
                tft.drawPixel(x + j, y + i, erase ? TFT_BLACK : color);
            }
        }
    }
}

// 2. Traductor automàtic dels antics playSound(id) cap al nou playSFX("/arxiu.wav")
void playSound(int id) {
    switch(id) {
        case 1: playSFX("/click.wav"); break;     // Moviment / Tret
        case 2: playSFX("/score.wav"); break;     // Puntuació
        case 3: playSFX("/launch.wav"); break;    // Iniciar
        case 4: playSFX("/gameover.wav"); break;  // Mort / Error
        case 5: playSFX("/powerup.wav"); break;   // Vida extra / Powerup
        case 6: playSFX("/score.wav"); break;     // Fantasma espantat Pac-Man
        default: playSFX("/launch.wav"); break;
    }
}
// ============================================================================
// SETUP I BUCLE PRINCIPAL (Depurat)
// ============================================================================
void setup() {
    Serial.begin(115200);
    delay(500);

    // 1. Pantalla TFT (SEMPRE abans de dibuixar res!)
    tft.begin();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    // 2. Configurar botons (Vital per poder prémer START i BACK)
    pinMode(BTN_SELECT, INPUT_PULLUP);
    pinMode(BTN_BACK, INPUT_PULLUP);

    // 3. Carregar la pantalla d'inici per primer cop (Ara sí que es veurà)
    mostrarSplashScreen();
    estatActual = ESTAT_INICI;

    // 4. Targeta SD (Bus SPI 2 Separat - Freqüència adaptada al teu mòdul)
    spiSD.begin(SCK_PIN, MISO_PIN, MOSI_PIN, CS_PIN);
    
    if (!SD.begin(CS_PIN, spiSD, 1000000)) {
        Serial.println("Error: No es pot inicialitzar la SD.");
    } else {
        Serial.println("SD Inicialitzada correctament!");
    }

    // 5. Àudio I2S
    initI2S();
    sdMutex = xSemaphoreCreateMutex();
    bgmQueue = xQueueCreate(1, 32);
    sfxQueue = xQueueCreate(5, 32);
    audioQueue = xQueueCreate(5, sizeof(const char*));
    // 6. Arrencar tasques d'àudio (Molt important perquè no es bloquegi el joc!)
    xTaskCreatePinnedToCore(bgmTask, "BGM_Task", 4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(SFX_Task, "SFX_Task", 4096, NULL, 2, NULL, 0);
    // 7. Reproduir música de la pantalla d'inici
    char cancoInici[32] = "/intro.wav"; 
    xQueueSend(bgmQueue, &cancoInici, portMAX_DELAY);
}
void aturarMusicaSegur() {
  bgmStop = true; // Demanem a la tasca de fons que s'aturi
  delay(50);      // DONEM 50ms AL NUCLI 0 PERQUÈ HO LLEGEIXI I TANQUI L'ARXIU!
}

void reproduirMusicaSegur(const char* canco) {
  bgmStop = true; 
  delay(50);      // Assegurem que qualsevol àudio anterior està mort
  
  char track[32];
  strncpy(track, canco, sizeof(track));
  
  // xQueueOverwrite MAI es bloqueja. Si la cua estava plena, simplement aixafa 
  // l'ordre vella amb la nova. Adeu Softlocks!
  xQueueOverwrite(bgmQueue, &track); 
  
  bgmStop = false; // Llum verda a l'àudio just després d'enviar a la cua
}
void loop() {
  unsigned long tempsActual = millis();
  int joyY; // Correcció: Declarem la variable fora del switch per evitar l'error

  switch (estatActual) {
    case ESTAT_INICI:
      // 1. Efecte Parpelleig Ajustat a la pantalla
      if (tempsActual - tempsAnteriorParpelleig >= intervalParpelleig) {
        tempsAnteriorParpelleig = tempsActual;
        textVisible = !textVisible;

        tft.setTextSize(1);
        if (textVisible) {
          tft.setTextColor(TFT_YELLOW, TFT_BLACK);
          tft.setCursor(17, 105);
          tft.print("Prem SELECT per Start"); 
        } else {
          tft.fillRect(15, 105, 135, 10, TFT_BLACK); // Esborra només el text
        }
      }
      
      // 2. Entrar al menú amb SELECT i aturar la música
      if (digitalRead(BTN_SELECT) == LOW) {
        delay(50); // Anti-rebots
        while(digitalRead(BTN_SELECT) == LOW); // <-- ESPERA QUE AIXEQUIS EL DIT!
        delay(50); 
        
        aturarMusicaSegur();

        mostrarMenu();
        dibuixarLlistaJocs();
        estatActual = ESTAT_MENU;
      }
      break; // <--- AQUEST BREAK FALTAVA AL TEU CODI! VITAL!

    case ESTAT_MENU:
      // A) Tornar enrere amb BACK
      if (digitalRead(BTN_BACK) == LOW) {
        delay(50);
        while(digitalRead(BTN_BACK) == LOW); // <-- Espera que deixis anar el botó BACK
        delay(50);
        
        mostrarSplashScreen();
        reproduirMusicaSegur("/intro.wav");
        estatActual = ESTAT_INICI;
      }

      // B) Navegar amb el Joystick a l'eix Y
      static bool joystickAlliberat = true;
      joyY = analogRead(JOY_Y); // Assignant el valor aquí
      if (joyY > 3000 && joystickAlliberat) { 
        seleccionat++;
        if (seleccionat >= TOTAL_GAMES) seleccionat = 0;
        updateMenu = true;
        joystickAlliberat = false; // Bloquegem fins que tornis al centre
        playSFX("/Game_Selection.wav");
    } 
    else if (joyY < 1000 && joystickAlliberat) { // amunt
        seleccionat--;
        if (seleccionat < 0) seleccionat = TOTAL_GAMES - 1;
        updateMenu = true;
        joystickAlliberat = false; // Bloquegem fins que tornis al centre
        playSFX("/Game_Selection.wav");
    } 
    else if (joyY >= 1000 && joyY <= 3000) { // Centrat
        joystickAlliberat = true; // El tornem a activar
    }
    if (updateMenu) {
        dibuixarLlistaJocs();
        updateMenu = false; // Tanquem l'aixeta fins al proper moviment
    }

      // C) Entrar al Joc Seleccionat
      if (digitalRead(BTN_SELECT) == LOW) {
        delay(50);
        while(digitalRead(BTN_SELECT) == LOW); // <-- ESPERA QUE AIXEQUIS EL DIT!
        delay(50);
        
        estatActual = ESTAT_JUGANT; 
        
        if (seleccionat == 0) playTetris();
        else if (seleccionat == 1) playSpaceInvaders();
        else if (seleccionat == 2) playSnake();
        else if (seleccionat == 3) playPong();
        else if (seleccionat == 4) playPacman();
        else if (seleccionat == 5) playBreakout();
        else if (seleccionat == 6) playFrogger();
        
        // Retorn al menú en sortir del joc
        mostrarMenu();
        dibuixarLlistaJocs();
        estatActual = ESTAT_MENU;
      }
      break;

    case ESTAT_JUGANT:
      // Mentrestant l'ESP32 s'executa dins de les funcions del joc.
      break;
  }
}
// ============================================================================
// LÒGICA DELS JOCS (Plantilles preparades)
// ============================================================================

void playTetris() {
    bool playing = true;
    int highScore = llegirHighScore("hs_tetris");
    int score = 0;
    const int COLS = 10, ROWS = 15, BLOCK_SZ = 8, OFFSET_X = 40, OFFSET_Y = 4;
    int board[ROWS][COLS] = {0};
    uint16_t colors[8] = {TFT_BLACK, TFT_CYAN, TFT_BLUE, TFT_ORANGE, TFT_YELLOW, TFT_GREEN, TFT_PURPLE, TFT_RED};

    const int shapes[7][4][2] = {
        {{0,0},{0,1},{0,2},{0,3}}, {{0,0},{0,1},{0,2},{1,2}}, {{0,0},{1,0},{1,1},{1,2}}, 
        {{0,0},{0,1},{1,0},{1,1}}, {{0,1},{0,2},{1,0},{1,1}}, {{0,0},{0,1},{0,2},{1,1}}, {{0,0},{0,1},{1,1},{1,2}} 
    };

    int currentPiece[4][2]; int pieceColor = 1; int px = 3, py = 0;
    int nextPieceType = random(0, 7);
    unsigned long lastFallTime = millis(), lastMoveTime = millis();
    int fallSpeed = 700; bool needsDraw = true; 
    bool joyUpPrev = false;

    // --- 1. ACTIVEM LA MÚSICA BGM ---
    playBGM("/Tetris_leitmotiv.wav");

    auto spawnPiece = [&]() {
        int type = nextPieceType; 
        nextPieceType = random(0, 7); 
        pieceColor = type + 1;
        for(int i=0; i<4; i++) { 
            currentPiece[i][0] = shapes[type][i][0]; 
            currentPiece[i][1] = shapes[type][i][1]; 
        }
        px = 3; py = 0;
    };

    auto checkCollision = [&](int cx, int cy, int piece[4][2]) {
        for(int i=0; i<4; i++) {
            int nx = cx + piece[i][1]; 
            int ny = cy + piece[i][0];
            if(nx < 0 || nx >= COLS || ny >= ROWS) return true;
            if(ny >= 0 && board[ny][nx] != 0) return true;
        } 
        return false;
    };

    tft.fillScreen(TFT_BLACK);
    tft.drawRect(OFFSET_X - 2, OFFSET_Y - 2, COLS * BLOCK_SZ + 3, ROWS * BLOCK_SZ + 3, TFT_WHITE);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("HI", 5, 20, 1); 
    tft.drawString(String(highScore), 5, 30, 1);
    spawnPiece();

    while(playing) {
        if (digitalRead(BTN_BACK) == LOW) { aturarMusicaSegur(); playing = false; delay(250); break; }
        
        unsigned long now = millis(); 
        int joyX = analogRead(JOY_X); 
        int joyY = analogRead(JOY_Y);
        bool joyUp = (joyY < 1000);

        // --- SENSE SONS DE MOVIMENT DE PEÇA ---
        if(now - lastMoveTime > 200) {
            if(joyX < 1000) { 
                if(!checkCollision(px - 1, py, currentPiece)) { 
                    px--; needsDraw = true; 
                } 
                lastMoveTime = now; 
            }
            if(joyX > 3000) { 
                if(!checkCollision(px + 1, py, currentPiece)) { 
                    px++; needsDraw = true; 
                } 
                lastMoveTime = now; 
            }
        }

        if (joyUp && !joyUpPrev) {
            int tempPiece[4][2];
            for(int i=0; i<4; i++) { 
                tempPiece[i][0] = currentPiece[i][1]; 
                tempPiece[i][1] = -currentPiece[i][0]; 
            }
            if(!checkCollision(px, py, tempPiece)) {
                for(int i=0; i<4; i++) { 
                    currentPiece[i][0] = tempPiece[i][0]; 
                    currentPiece[i][1] = tempPiece[i][1]; 
                }
                needsDraw = true;
            }
        }
        joyUpPrev = joyUp;

        int currentSpeed = (joyY > 3000) ? 60 : fallSpeed; 
        if(now - lastFallTime > currentSpeed) {
            if(!checkCollision(px, py + 1, currentPiece)) { 
                py++; 
                needsDraw = true; 
            } else {
                // --- 2. PEÇA ANCORADA (TOCA FONS) ---
                playSound(3);

                for(int i=0; i<4; i++) { 
                    if(py + currentPiece[i][0] >= 0) {
                        board[py + currentPiece[i][0]][px + currentPiece[i][1]] = pieceColor; 
                    }
                }
                
                int lines = 0;
                for(int r = ROWS-1; r >= 0; r--) {
                    bool full = true;
                    for(int c = 0; c < COLS; c++) {
                        if(board[r][c] == 0) full = false;
                    }
                    if(full) {
                        lines++;
                        for(int y = r; y > 0; y--) {
                            for(int c = 0; c < COLS; c++) {
                                board[y][c] = board[y-1][c];
                            }
                        }
                        for(int c = 0; c < COLS; c++) board[0][c] = 0; 
                        r++; 
                    }
                }

                if(lines > 0) { 
                    score += lines * 10; 
                    fallSpeed = max(100, 700 - (score * 3)); 
                    
                    // --- 3. LÍNIA COMPLETADA ---
                    // Sonarà l'arxiu d'èxit de puntuació
                    playSFX("/score.wav"); 
                } 

                spawnPiece();
                if(checkCollision(px, py, currentPiece)) {
                    if(score > highScore) guardarHighScore("hs_tetris", score);
                    tft.fillScreen(TFT_RED); 
                    tft.setTextColor(TFT_WHITE); 
                    tft.drawString("GAME OVER", 40, 55, 2);
                    aturarMusicaSegur();
                    playSFX("/gameover.wav"); // El teu game over original
                    delay(2000); 
                    playing = false;
                } 
                needsDraw = true;
            } 
            lastFallTime = now;
        }

        if (needsDraw) {
            tft.fillRect(OFFSET_X, OFFSET_Y, COLS*BLOCK_SZ, ROWS*BLOCK_SZ, TFT_BLACK);
            for(int r=0; r<ROWS; r++) {
                for(int c=0; c<COLS; c++) {
                    if(board[r][c] > 0) {
                        tft.fillRect(OFFSET_X + c*BLOCK_SZ, OFFSET_Y + r*BLOCK_SZ, BLOCK_SZ-1, BLOCK_SZ-1, colors[board[r][c]]);
                    }
                }
            }
            for(int i=0; i<4; i++) {
                if(py + currentPiece[i][0] >= 0) {
                    tft.fillRect(OFFSET_X + (px + currentPiece[i][1])*BLOCK_SZ, OFFSET_Y + (py + currentPiece[i][0])*BLOCK_SZ, BLOCK_SZ-1, BLOCK_SZ-1, colors[pieceColor]);
                }
            }
            tft.fillRect(125, 20, 4*BLOCK_SZ, 4*BLOCK_SZ, TFT_BLACK);
            tft.setTextColor(TFT_WHITE, TFT_BLACK); 
            tft.drawString("NEXT", 125, 5, 1);
            for(int i=0; i<4; i++) {
                tft.fillRect(125 + shapes[nextPieceType][i][1]*BLOCK_SZ, 20 + shapes[nextPieceType][i][0]*BLOCK_SZ, BLOCK_SZ-1, BLOCK_SZ-1, colors[nextPieceType + 1]);
            }
            tft.setTextColor(TFT_WHITE, TFT_BLACK); 
            tft.drawString("SCR", 5, 50, 1); 
            tft.drawString(String(score), 5, 60, 1);
            needsDraw = false;
        } 
        delay(10);
    }
}

void playSpaceInvaders() {
    bool playing = true;
    int highScore = llegirHighScore("hs_invaders");
    int score = 0, lives = 3, stage = 1;

    const uint16_t spr_tank[8] = {
        0b0000000100000000, 0b0000001110000000, 0b0000001110000000, 0b0000111111100000,
        0b0011111111111000, 0b0111111111111100, 0b1111111111111110, 0b1111111111111110
    }; 
    const uint16_t spr_squid[2][8] = {
        {0b0001100000000000, 0b0011110000000000, 0b0111111000000000, 0b1101101100000000,
         0b1111111100000000, 0b0010010000000000, 0b0101101000000000, 0b1000000100000000},
        {0b0001100000000000, 0b0011110000000000, 0b0111111000000000, 0b1101101100000000,
         0b1111111100000000, 0b0010010000000000, 0b0100001000000000, 0b0010010000000000}
    }; 
    const uint16_t spr_crab[2][8] = {
        {0b0010000010000000, 0b0001000100000000, 0b0011111110000000, 0b0110111011000000,
         0b1111111111100000, 0b1011111110100000, 0b1010000010100000, 0b0001101100000000},
        {0b0010000010000000, 0b0101000101000000, 0b1011111110100000, 0b1110111011100000,
         0b1111111111100000, 0b0011111110000000, 0b0010000010000000, 0b0100000001000000}
    }; 
    const uint16_t spr_octo[2][8] = {
        {0b0000111100000000, 0b0011111111000000, 0b1111111111110000, 0b1110011001110000,
         0b1111111111110000, 0b0011100111000000, 0b0110011001100000, 0b1100000000110000},
        {0b0000111100000000, 0b0011111111000000, 0b1111111111110000, 0b1110011001110000,
         0b1111111111110000, 0b0001100110000000, 0b0011011011000000, 0b0000000000000000}
    }; 
    const uint16_t spr_ufo[7] = {
        0b0000011111100000, 0b0001111111111000, 0b0011111111111100, 0b0110110110110110,
        0b1111111111111111, 0b0011111001111100, 0b0001000000001000
    }; 
    const uint16_t spr_shield[10] = {
        0b0000111111000000, 0b0001111111100000, 0b0011111111110000, 0b0111111111111000,
        0b1111111111111100, 0b1111111111111100, 0b1111111111111100, 0b1111000000111100,
        0b1110000000011100, 0b1110000000011100
    }; 

    bool shieldData[4][10][14];
    auto initShields = [&]() {
        for(int s=0; s<4; s++) {
            for(int i=0; i<10; i++) {
                for(int j=0; j<14; j++) {
                    shieldData[s][i][j] = (spr_shield[i] & (1 << (15 - j))) ? true : false;
                }
            }
        }
    };
    
    auto drawShields = [&]() {
        for(int s=0; s<4; s++) {
            int sx = 15 + s*35; int sy = 95;
            for(int i=0; i<10; i++) {
                for(int j=0; j<14; j++) {
                    if(shieldData[s][i][j]) tft.drawPixel(sx+j, sy+i, TFT_GREEN);
                }
            }
        }
    };
    
    auto damageShield = [&](int bx, int by) {
        for(int s=0; s<4; s++) {
            int sx = 15 + s*35; int sy = 95;
            if(bx >= sx && bx < sx+14 && by >= sy && by < sy+10) {
                int lx = bx - sx, ly = by - sy;
                if(shieldData[s][ly][lx]) {
                    for(int dy=-2; dy<=2; dy++) {
                        for(int dx=-2; dx<=2; dx++) {
                            int nx = lx+dx, ny = ly+dy;
                            if(nx>=0 && nx<14 && ny>=0 && ny<10 && shieldData[s][ny][nx]) {
                                shieldData[s][ny][nx] = false; 
                                tft.drawPixel(sx+nx, sy+ny, TFT_BLACK);
                            }
                        }
                    } 
                    return true;
                }
            }
        } 
        return false;
    };

    int playerX = 70, oldPlayerX = 70; const int playerY = 115;
    int laserX = 0, laserY = 0; bool laserActive = false;
    struct EnemyBullet { int x, y; bool active; }; EnemyBullet enemyBullets[3]; 

    struct Invader { int x, y; int type; bool alive; }; Invader aliens[5][8];
    int alienMoveDir = 2, animFrame = 0, aliveAliens = 40; unsigned long lastAlienMove = 0;
    float ufoX = 0; const int ufoY = 10; bool ufoActive = false; int ufoDir = 1; unsigned long lastUfoSpawn = millis();

    auto initStage = [&]() {
        tft.fillScreen(TFT_BLACK);
        aliveAliens = 40; alienMoveDir = 2; ufoActive = false; laserActive = false;
        for(int i=0; i<3; i++) enemyBullets[i].active = false;
        for(int r=0; r<5; r++) {
            for(int c=0; c<8; c++) {
                aliens[r][c] = {12 + c*16, 22 + r*12, (r==0)?0:((r<3)?1:2), true};
                int w = (aliens[r][c].type == 0)?8:((aliens[r][c].type == 1)?11:12);
                drawSprite(aliens[r][c].x, aliens[r][c].y, (aliens[r][c].type == 0)?spr_squid[0]:((aliens[r][c].type==1)?spr_crab[0]:spr_octo[0]), w, 8, TFT_WHITE);
            }
        }
        if (stage == 1) initShields(); 
        drawShields();
        drawSprite(playerX, playerY, spr_tank, 15, 8, TFT_GREEN);
        tft.setTextColor(TFT_GREEN, TFT_BLACK); 
        tft.drawString("SCR:" + String(score), 2, 2, 1);
        tft.drawString("LV:" + String(lives), 65, 2, 1); 
        tft.drawString("HI:" + String(highScore), 110, 2, 1);
        lastAlienMove = millis();
    };

    initStage();

    while(playing) {
        if (digitalRead(BTN_BACK) == LOW) { playing = false; delay(250); break; }
        
        int joyX = analogRead(JOY_X); 
        oldPlayerX = playerX;
        if(joyX < 1000 && playerX > 5) playerX -= 2; 
        if(joyX > 3000 && playerX < 140) playerX += 2;
        
        if(oldPlayerX != playerX) {
            drawSprite(oldPlayerX, playerY, spr_tank, 15, 8, TFT_BLACK, true);
            drawSprite(playerX, playerY, spr_tank, 15, 8, TFT_GREEN, false);
        }

        if(digitalRead(BTN_SELECT) == LOW && !laserActive) { 
            laserX = playerX + 7; laserY = playerY - 4; laserActive = true; playSound(3); 
        }

        if(laserActive) {
            tft.drawLine(laserX, laserY, laserX, laserY+4, TFT_BLACK); 
            laserY -= 4; 
            if(laserY < 10) {
                laserActive = false;
            } else {
                tft.drawLine(laserX, laserY, laserX, laserY+4, TFT_YELLOW); 
                if(damageShield(laserX, laserY)) { 
                    tft.drawLine(laserX, laserY, laserX, laserY+4, TFT_BLACK); 
                    laserActive = false; 
                    playSound(3); 
                }
                
                if(laserActive) {
                    for(int r=0; r<5; r++) {
                        for(int c=0; c<8; c++) {
                            if(aliens[r][c].alive) {
                                int w = (aliens[r][c].type == 0)?8:((aliens[r][c].type == 1)?11:12);
                                if(laserX >= aliens[r][c].x && laserX < aliens[r][c].x + w && laserY >= aliens[r][c].y && laserY <= aliens[r][c].y + 8) {
                                    aliens[r][c].alive = false; 
                                    laserActive = false; 
                                    aliveAliens--;
                                    tft.drawLine(laserX, laserY, laserX, laserY+4, TFT_BLACK);
                                    tft.fillRect(aliens[r][c].x, aliens[r][c].y, 12, 8, TFT_BLACK); 
                                    score += (aliens[r][c].type == 0) ? 30 : ((aliens[r][c].type == 1) ? 20 : 10);
                                    playSound(2); 
                                    tft.fillRect(2, 2, 60, 10, TFT_BLACK); 
                                    tft.drawString("SCR:" + String(score), 2, 2, 1);
                                    break;
                                }
                            }
                        } 
                        if(!laserActive) break;
                    }
                }
                if(laserActive && ufoActive) {
                    if(laserX >= (int)ufoX && laserX <= (int)ufoX + 16 && laserY >= ufoY && laserY <= ufoY + 7) {
                        tft.drawLine(laserX, laserY, laserX, laserY+4, TFT_BLACK); 
                        drawSprite((int)ufoX, ufoY, spr_ufo, 16, 7, TFT_BLACK, true); 
                        ufoActive = false; 
                        laserActive = false;
                        int ufoScores[] = {50, 100, 150, 300}; 
                        score += ufoScores[random(0,4)]; 
                        playSound(2);
                        tft.fillRect(2, 2, 60, 10, TFT_BLACK); 
                        tft.drawString("SCR:" + String(score), 2, 2, 1);
                    }
                }
            }
        }

        if(!ufoActive && millis() - lastUfoSpawn > random(10000, 25000)) { 
            ufoActive = true; 
            ufoDir = (random(0,2)==0) ? 1 : -1; 
            ufoX = (ufoDir == 1) ? -16 : 160; 
             playSound(6);
        }
        
        if(ufoActive) {
            drawSprite((int)ufoX, ufoY, spr_ufo, 16, 7, TFT_BLACK, true); 
            ufoX += ufoDir * 1.2;
            if(ufoX > 165 || ufoX < -20) { 
                ufoActive = false; 
                lastUfoSpawn = millis(); 
            } else {
                drawSprite((int)ufoX, ufoY, spr_ufo, 16, 7, TFT_RED, false);
            }
        }

        int moveDelay = max(60, aliveAliens * 20 - stage*10); 
        if(millis() - lastAlienMove > (unsigned long)moveDelay) {
            int minX = 999, maxX = -999, maxY = 0;
            for(int r=0; r<5; r++) {
                for(int c=0; c<8; c++) {
                    if(aliens[r][c].alive) {
                        if(aliens[r][c].x < minX) minX = aliens[r][c].x;
                        int w = (aliens[r][c].type == 0)?8:((aliens[r][c].type == 1)?11:12);
                        if(aliens[r][c].x + w > maxX) maxX = aliens[r][c].x + w;
                        if(aliens[r][c].y > maxY) maxY = aliens[r][c].y;
                    }
                }
            }

            if (aliveAliens == 0) { 
                stage++; 
                playSound(2); 
                delay(1000); 
                initStage(); 
                continue; 
            }

            bool drop = false;
            if(alienMoveDir > 0 && maxX + alienMoveDir > 156) drop = true;
            if(alienMoveDir < 0 && minX + alienMoveDir < 4) drop = true;

            animFrame = 1 - animFrame;
            for(int r=0; r<5; r++) {
                for(int c=0; c<8; c++) {
                    if(aliens[r][c].alive) {
                        int w = (aliens[r][c].type == 0)?8:((aliens[r][c].type == 1)?11:12);
                        tft.fillRect(aliens[r][c].x, aliens[r][c].y, 12, 8, TFT_BLACK);
                        if(drop) aliens[r][c].y += 4; 
                        else aliens[r][c].x += alienMoveDir;
                        
                        drawSprite(aliens[r][c].x, aliens[r][c].y, (aliens[r][c].type==0)?spr_squid[animFrame]:((aliens[r][c].type==1)?spr_crab[animFrame]:spr_octo[animFrame]), w, 8, TFT_WHITE, false);
                    }
                }
            }
            if(drop) alienMoveDir = -alienMoveDir; 
            lastAlienMove = millis(); 
            
            if(maxY + 8 >= playerY) lives = 0; 
        }

        if(random(0, max(40, 100 - stage*10)) == 0) {
            for(int i=0; i<3; i++) {
                if(!enemyBullets[i].active) {
                    int bottomAliens[8]; int colsCount = 0;
                    for(int c=0; c<8; c++) {
                        bottomAliens[c] = -1;
                        for(int r=4; r>=0; r--) {
                            if(aliens[r][c].alive) { bottomAliens[c] = r; colsCount++; break; }
                        }
                    }
                    if(colsCount > 0) {
                        int randCol; 
                        do { randCol = random(0,8); } while(bottomAliens[randCol] == -1);
                        int r = bottomAliens[randCol]; 
                        int w = (aliens[r][randCol].type == 0)?8:((aliens[r][randCol].type == 1)?11:12);
                        enemyBullets[i].x = aliens[r][randCol].x + w/2; 
                        enemyBullets[i].y = aliens[r][randCol].y + 8; 
                        enemyBullets[i].active = true;
                    } 
                    break;
                }
            }
        }

        for(int i=0; i<3; i++) {
            if(enemyBullets[i].active) {
                tft.fillRect(enemyBullets[i].x, enemyBullets[i].y, 2, 6, TFT_BLACK); 
                enemyBullets[i].y += 3;
                if(enemyBullets[i].y > 128) {
                    enemyBullets[i].active = false;
                } else {
                    tft.fillRect(enemyBullets[i].x, enemyBullets[i].y, 2, 6, TFT_ORANGE);
                    if(damageShield(enemyBullets[i].x, enemyBullets[i].y + 6)) {
                        tft.fillRect(enemyBullets[i].x, enemyBullets[i].y, 2, 6, TFT_BLACK); 
                        enemyBullets[i].active = false;
                    }
                    else if(enemyBullets[i].x >= playerX && enemyBullets[i].x <= playerX+15 && enemyBullets[i].y+6 >= playerY) {
                        lives--; 
                         //playSound(4);
                        enemyBullets[i].active = false; 
                        tft.fillRect(enemyBullets[i].x, enemyBullets[i].y, 2, 6, TFT_BLACK);
                        
                        if(laserActive) { 
                            tft.drawLine(laserX, laserY, laserX, laserY+4, TFT_BLACK); 
                            laserActive = false; 
                        }
                        
                        drawSprite(playerX, playerY, spr_tank, 15, 8, TFT_RED, false);
                        tft.fillRect(65, 2, 40, 10, TFT_BLACK); 
                        tft.drawString("LV:" + String(lives), 65, 2, 1);
                        delay(1000);
                        
                        if(lives > 0) {
                            drawSprite(playerX, playerY, spr_tank, 15, 8, TFT_BLACK, true); 
                            playerX = 70; 
                            laserActive = false;
                            drawSprite(playerX, playerY, spr_tank, 15, 8, TFT_GREEN, false);
                            for(int b=0; b<3; b++) { 
                                tft.fillRect(enemyBullets[b].x, enemyBullets[b].y, 2, 6, TFT_BLACK); 
                                enemyBullets[b].active = false; 
                            }
                        }
                    }
                }
            }
        }

        if(lives <= 0) {
            if(score > highScore) guardarHighScore("hs_invaders", score);
            tft.fillScreen(TFT_RED); 
            tft.setTextColor(TFT_WHITE); 
            tft.drawString("GAME OVER", 40, 55, 2);
            playSound(4); 
            delay(2000); 
            playing = false;
        } 
        delay(30);
    }
}

void playSnake() {
    bool playing = true;
    int highScore = llegirHighScore("hs_snake");
    int score = 0;

    const int BLOCK_SIZE = 8, MAX_LEN = 150; 
    Point snake[MAX_LEN]; int snakeLen = 4;
    int dirX = BLOCK_SIZE, dirY = 0; 
    int lastDirX = BLOCK_SIZE, lastDirY = 0;
    const int MIN_X = 8, MAX_X = 144, MIN_Y = 24, MAX_Y = 120;
    Point food; unsigned long lastMoveTime = millis(); int gameSpeed = 250;

    const uint16_t spr_apple[8] = {
        0b0000110000000000, 0b0001001000000000, 0b0011111100000000, 0b0111111000000000,
        0b1111111100000000, 0b1111111100000000, 0b0111111000000000, 0b0011110000000000
    };
    const uint16_t spr_snake_body[8] = {
        0b0011110000000000, 0b0111111000000000, 0b1111111100000000, 0b1111111100000000,
        0b1111111100000000, 0b1111111100000000, 0b0111111000000000, 0b0011110000000000
    };
    const uint16_t spr_head_up[8] = {
        0b0011110000000000, 0b0110011000000000, 0b1110011100000000, 0b1111111100000000,
        0b1111111100000000, 0b0111111000000000, 0b0011110000000000, 0b0000000000000000
    };
    const uint16_t spr_head_down[8] = {
        0b0000000000000000, 0b0011110000000000, 0b0111111000000000, 0b1111111100000000,
        0b1111111100000000, 0b1110011100000000, 0b0110011000000000, 0b0011110000000000
    };
    const uint16_t spr_head_left[8] = {
        0b0001110000000000, 0b0011111000000000, 0b0110111100000000, 0b0101111100000000,
        0b0101111100000000, 0b0110111100000000, 0b0011111000000000, 0b0001110000000000
    };
    const uint16_t spr_head_right[8] = {
        0b0011100000000000, 0b0111110000000000, 0b1111011000000000, 0b1111101000000000,
        0b1111101000000000, 0b1111011000000000, 0b0111110000000000, 0b0011100000000000
    };

    auto spawnFood = [&]() {
        bool valid = false;
        while (!valid) {
            food.x = random(MIN_X / BLOCK_SIZE, MAX_X / BLOCK_SIZE) * BLOCK_SIZE;
            food.y = random(MIN_Y / BLOCK_SIZE, MAX_Y / BLOCK_SIZE) * BLOCK_SIZE;
            valid = true;
            for (int i = 0; i < snakeLen; i++) { 
                if (snake[i].x == food.x && snake[i].y == food.y) { valid = false; break; } 
            }
        } 
        drawSprite(food.x, food.y, spr_apple, 8, 8, TFT_RED);
    };

    tft.fillScreen(TFT_BLACK);
    tft.drawRect(MIN_X - 2, MIN_Y - 2, MAX_X - MIN_X + BLOCK_SIZE + 4, MAX_Y - MIN_Y + BLOCK_SIZE + 4, TFT_BLUE);
    tft.setTextColor(TFT_WHITE);
    tft.drawString("SNAKE", 5, 5, 1); 
    tft.drawString("HI:" + String(highScore), 110, 5, 1);

    for(int i = 0; i < snakeLen; i++) {
        snake[i].x = 80 - i*BLOCK_SIZE; snake[i].y = 64;
        drawSprite(snake[i].x, snake[i].y, spr_snake_body, 8, 8, TFT_GREEN);
    } 
    spawnFood();

    while (playing) {
        if (digitalRead(BTN_BACK) == LOW) { playing = false; delay(250); break; }

        int joyX = analogRead(JOY_X); 
        int joyY = analogRead(JOY_Y);

        if (joyX > 3000 && lastDirX == 0) { dirX = BLOCK_SIZE; dirY = 0; }
        else if (joyX < 1000 && lastDirX == 0) { dirX = -BLOCK_SIZE; dirY = 0; }
        else if (joyY > 3000 && lastDirY == 0) { dirX = 0; dirY = BLOCK_SIZE; }
        else if (joyY < 1000 && lastDirY == 0) { dirX = 0; dirY = -BLOCK_SIZE; }

        if (millis() - lastMoveTime > (unsigned long)gameSpeed) {
            lastDirX = dirX;  //Actualitzem la ultima posicio
            lastDirY = dirY;
            Point nextHead = { snake[0].x + dirX, snake[0].y + dirY };

            if (nextHead.x < MIN_X || nextHead.x > MAX_X || nextHead.y < MIN_Y || nextHead.y > MAX_Y) playing = false;
            for (int i = 0; i < snakeLen; i++) { 
                if (nextHead.x == snake[i].x && nextHead.y == snake[i].y) playing = false; 
            }

            if (!playing) {
                if (score > highScore) guardarHighScore("hs_snake", score);
                playSound(4); 
                tft.fillScreen(TFT_RED); 
                tft.setTextColor(TFT_WHITE);
                tft.drawString("GAME OVER", 40, 55, 2); 
                delay(2000); break;
            }

            if (abs(nextHead.x - food.x) < BLOCK_SIZE/2 && abs(nextHead.y - food.y) < BLOCK_SIZE/2) {
                score++; 
                if (snakeLen < MAX_LEN) snakeLen++;
                gameSpeed = max(80, 250 - (score * 5)); 
                playSound(3); 
                spawnFood();
                tft.fillRect(60, 5, 40, 10, TFT_BLACK); 
                tft.drawString(String(score), 60, 5, 1);
            } else { 
                tft.fillRect(snake[snakeLen-1].x, snake[snakeLen-1].y, BLOCK_SIZE, BLOCK_SIZE, TFT_BLACK); 
            }

            for (int i = snakeLen - 1; i > 0; i--) snake[i] = snake[i - 1];
            snake[0] = nextHead;

            drawSprite(snake[1].x, snake[1].y, spr_snake_body, 8, 8, TFT_GREEN);
            if(dirY < 0) drawSprite(snake[0].x, snake[0].y, spr_head_up, 8, 8, TFT_GREEN);
            else if(dirY > 0) drawSprite(snake[0].x, snake[0].y, spr_head_down, 8, 8, TFT_GREEN);
            else if(dirX < 0) drawSprite(snake[0].x, snake[0].y, spr_head_left, 8, 8, TFT_GREEN);
            else drawSprite(snake[0].x, snake[0].y, spr_head_right, 8, 8, TFT_GREEN);

            lastMoveTime = millis();
        } 
        delay(10);
    }
}

void playPong() {
    bool playing = true;
    int p1Score = 0, cpuScore = 0;
    const int FIELD_TOP = 16, FIELD_BOTTOM = 120, PADDLE_H = 20, PADDLE_W = 4, GOAL_SCORE = 5; 
    int p1Y = 50, oldP1Y = 50;   
    float cpuY = 50.0, oldCpuY = 50.0; 
    float ballX = 80.0, ballY = 64.0, oldBallX = 80.0, oldBallY = 64.0, ballDX = 1.8, ballDY = 1.5; 
    bool ballActive = false; 

    auto resetBall = [&]() {
        tft.fillRect((int)oldBallX, (int)oldBallY, 4, 4, TFT_BLACK); 
        ballX = 80.0; ballY = 64.0; 
        oldBallX = ballX; oldBallY = ballY; 
        ballDX = (random(0, 2) == 0 ? -1.8 : 1.8); ballDY = (random(0, 2) == 0 ? -1.5 : 1.5); 
        cpuY = ballY - (PADDLE_H / 2.0); 
        oldCpuY = cpuY;
        ballActive = false;
    };

    tft.fillScreen(TFT_BLACK);
    tft.drawLine(80, FIELD_TOP, 80, FIELD_BOTTOM, TFT_DARKGREY);
    tft.drawLine(0, FIELD_TOP-1, 160, FIELD_TOP-1, TFT_WHITE);
    tft.drawLine(0, FIELD_BOTTOM+1, 160, FIELD_BOTTOM+1, TFT_WHITE);
    resetBall();

    while (playing) {
        if (digitalRead(BTN_BACK) == LOW) { playing = false; delay(250); break; }

        int joyY = analogRead(JOY_Y); 
        oldP1Y = p1Y;
        if (joyY < 1000) { p1Y -= 3; if (p1Y < FIELD_TOP) p1Y = FIELD_TOP; }
        if (joyY > 3000) { p1Y += 3; if (p1Y > FIELD_BOTTOM - PADDLE_H) p1Y = FIELD_BOTTOM - PADDLE_H; }

        if (!ballActive) {
            if (digitalRead(BTN_SELECT) == LOW) { 
                ballActive = true; 
                playSFX("/Pong_wall_bounce.wav");; 
                delay(200); 
            }
        } else {
            oldCpuY = cpuY; 
            float targetY = ballY - (PADDLE_H / 2);
            if (cpuY < targetY) cpuY += 1.2; 
            if (cpuY > targetY) cpuY -= 1.2;
            if (cpuY < FIELD_TOP) cpuY = FIELD_TOP; 
            if (cpuY > FIELD_BOTTOM - PADDLE_H) cpuY = FIELD_BOTTOM - PADDLE_H;

            oldBallX = ballX; 
            oldBallY = ballY; 
            ballX += ballDX; 
            ballY += ballDY;

            if (ballY <= FIELD_TOP || ballY >= FIELD_BOTTOM - 4) { 
                ballDY = -ballDY; 
                playSFX("/Pong_wall_bounce.wav"); 
            }
            if (ballDX < 0 && ballX <= 5 + PADDLE_W && ballX >= 5) {
                if (ballY + 4 >= p1Y && ballY <= p1Y + PADDLE_H) { 
                    ballDX = -ballDX * 1.05; 
                    ballX = 5 + PADDLE_W; 
                    playSound(3); 
                }
            }
            if (ballDX > 0 && ballX >= 155 - PADDLE_W - 4 && ballX <= 155) {
                if (ballY + 4 >= cpuY && ballY <= cpuY + PADDLE_H) { 
                    ballDX = -ballDX * 1.05; 
                    ballX = 155 - PADDLE_W - 4; 
                    playSound(3); 
                }
            }

            bool scored = false;
            if (ballX < 0) { cpuScore++; playSFX("/Pong_point_lost.wav"); scored = true; }
            if (ballX > 160) { p1Score++; playSound(2); scored = true; }

            if (scored) {
                tft.fillRect(5, oldP1Y, PADDLE_W, PADDLE_H, TFT_BLACK); 
                tft.fillRect(5, p1Y, PADDLE_W, PADDLE_H, TFT_BLACK);
                tft.fillRect(155 - PADDLE_W, (int)oldCpuY, PADDLE_W, PADDLE_H, TFT_BLACK); 
                tft.fillRect(155 - PADDLE_W, (int)cpuY, PADDLE_W, PADDLE_H, TFT_BLACK);

                tft.fillRect(10, 2, 140, 12, TFT_BLACK); 
                tft.setTextColor(TFT_WHITE);
                tft.drawString("P1: " + String(p1Score), 20, 2, 1); 
                tft.drawString("CPU: " + String(cpuScore), 100, 2, 1);
                
                if (p1Score >= GOAL_SCORE || cpuScore >= GOAL_SCORE) {
                    tft.fillScreen(TFT_BLUE); 
                    tft.drawString(p1Score >= GOAL_SCORE ? "HAS GUANYAT!" : "GAME OVER", 40, 55, 2); 
                    delay(2000); 
                    playing = false;
                } else { 
                    resetBall(); 
                    delay(1000); 
                }
            }
        }
        
        if (oldP1Y != p1Y) tft.fillRect(5, oldP1Y, PADDLE_W, PADDLE_H, TFT_BLACK);
        if ((int)oldCpuY != (int)cpuY) tft.fillRect(155 - PADDLE_W, (int)oldCpuY, PADDLE_W, PADDLE_H, TFT_BLACK);
        if ((int)oldBallX != (int)ballX || (int)oldBallY != (int)ballY) {
            tft.fillRect((int)oldBallX, (int)oldBallY, 4, 4, TFT_BLACK);
            if((int)oldBallX >= 76 && (int)oldBallX <= 84) tft.drawLine(80, FIELD_TOP, 80, FIELD_BOTTOM, TFT_DARKGREY); 
        }
        tft.fillRect(5, p1Y, PADDLE_W, PADDLE_H, TFT_WHITE); 
        tft.fillRect(155 - PADDLE_W, (int)cpuY, PADDLE_W, PADDLE_H, TFT_WHITE);
        tft.fillRect((int)ballX, (int)ballY, 4, 4, TFT_YELLOW);
        
        delay(30); 
    }
}

void playPacman() {
    bool playing = true;
    int highScore = llegirHighScore("hs_pacman");
    int score = 0, stage = 1, lives = 3;
    
    const int ROWS = 14, COLS = 12, BLOCK_SIZE = 9;
    const int OFFSET_X = 26, OFFSET_Y = 1;

    uint8_t map[ROWS][COLS];
    const uint8_t origMap[ROWS][COLS] = {
        {1,1,1,1,1,1,1,1,1,1,1,1},
        {1,2,2,2,2,1,1,2,2,2,2,1},
        {1,3,1,1,2,1,1,2,1,1,3,1},
        {1,2,1,1,2,2,2,2,1,1,2,1},
        {1,2,2,2,2,1,1,2,2,2,2,1},
        {1,1,2,1,2,2,2,2,1,2,1,1},
        {0,0,0,1,1,0,0,1,1,0,0,0}, 
        {1,1,2,1,1,1,1,1,1,2,1,1},
        {1,2,2,2,2,1,1,2,2,2,2,1},
        {1,2,1,1,2,1,1,2,1,1,2,1},
        {1,2,2,1,2,2,2,2,1,2,2,1},
        {1,1,2,1,2,1,1,2,1,2,1,1},
        {1,2,2,2,2,1,1,2,2,2,2,1},
        {1,1,1,1,1,1,1,1,1,1,1,1}
    };

    int px, py, pDirX, pDirY, nDirX, nDirY;
    struct Ghost { int x, y, dirX, dirY; uint16_t color; }; 
    Ghost ghosts[2];
    
    int scaredTimer = 0; 
    int scatterTimer = 0; 
    unsigned long frameCount = 0;
    bool gameStarted = false; 
    
    // --- CONTROL ANTI-SATURACIÓ DE CUA ---
    unsigned long lastWakaTime = 0; 

    auto playIntermission = [&]() {
        tft.fillScreen(TFT_BLACK);
        int y = 60; 
        
        // Aquest playSFX talla instantàniament el Powerup gràcies al "cancelAudio"
        playSFX("/PacMan_Intermission.wav"); 
        unsigned long startTime = millis();
        unsigned long currentTime = 0;

        int oldPacX = 140, oldGhostX = 180;
        int oldPacX2 = -60, oldGhostX2 = -10;

        while (true) {
            currentTime = millis() - startTime;
            if (currentTime > 5200) break;

            int pacX = 135 - (180 * currentTime / 5200); 
            int ghostX = 165 - (190 * currentTime / 5200);

            if (pacX != oldPacX || ghostX != oldGhostX) {
                tft.fillRect(oldPacX - 2, y - 5, 20, 18, TFT_BLACK);
                tft.fillRect(oldGhostX - 2, y - 5, 20, 18, TFT_BLACK);
                
                tft.fillCircle(pacX + 4, y + 4, 4, TFT_YELLOW);
                if ((currentTime / 150) % 2 == 0) tft.fillTriangle(pacX + 4, y + 4, pacX - 2, y, pacX - 2, y + 8, TFT_BLACK);

                tft.fillRect(ghostX + 1, y + 2, 7, 7, TFT_RED);
                tft.fillCircle(ghostX + 4, y + 3, 3, TFT_RED);
                tft.fillRect(ghostX + 2, y + 3, 2, 2, TFT_WHITE);
                tft.fillRect(ghostX + 5, y + 3, 2, 2, TFT_WHITE);

                oldPacX = pacX; oldGhostX = ghostX;
            }
            delay(5);
        }

        tft.fillRect(oldPacX - 2, y - 5, 20, 18, TFT_BLACK);
        tft.fillRect(oldGhostX - 2, y - 5, 20, 18, TFT_BLACK);

        while ((millis() - startTime) < 5300) { delay(1); }

        while (true) {
            currentTime = millis() - startTime;
            if (currentTime > 10604) break;
            
            unsigned long part2Time = currentTime - 5300;

            int ghostX = -10 + (190 * part2Time / 5304);
            int pacX = -50 + (190 * part2Time / 5304);

            if (pacX != oldPacX2 || ghostX != oldGhostX2) {
                tft.fillRect(oldPacX2 - 15, y - 6, 35, 22, TFT_BLACK);
                tft.fillRect(oldGhostX2 - 5, y - 5, 20, 18, TFT_BLACK);

                tft.fillRect(ghostX + 1, y + 2, 7, 7, TFT_BLUE);
                tft.fillCircle(ghostX + 4, y + 3, 3, TFT_BLUE);
                tft.fillRect(ghostX + 2, y + 3, 2, 2, TFT_WHITE);
                tft.fillRect(ghostX + 5, y + 3, 2, 2, TFT_WHITE);

                tft.fillCircle(pacX + 8, y + 4, 8, TFT_YELLOW);
                if ((part2Time / 150) % 2 == 0) tft.fillTriangle(pacX + 8, y + 4, pacX + 18, y - 4, pacX + 18, y + 12, TFT_BLACK);

                oldPacX2 = pacX; oldGhostX2 = ghostX;
            }
            delay(5);
        }
    };

    auto drawUI = [&](bool full = false) {
        if (full) {
            tft.setTextColor(TFT_YELLOW, TFT_BLACK); 
            tft.drawString("1UP", 2, 5, 1);     
            tft.drawString("HI", 138, 5, 1);    
            tft.setTextColor(TFT_WHITE, TFT_BLACK); 
            tft.drawString(String(highScore), 138, 15, 1);
        }
        tft.fillRect(2, 15, 23, 10, TFT_BLACK); 
        tft.setTextColor(TFT_WHITE, TFT_BLACK); 
        tft.drawString(String(score), 2, 15, 1);
    };

    auto drawLives = [&]() {
        for (int i = 0; i < 3; i++) {
            int lx = 148; 
            int ly = 110 - (i * 15); 
            tft.fillRect(lx - 5, ly - 5, 11, 11, TFT_BLACK); 
            if (i < lives) {
                tft.fillCircle(lx, ly, 4, TFT_YELLOW);
                tft.fillTriangle(lx, ly, lx - 5, ly - 3, lx - 5, ly + 3, TFT_BLACK); 
            }
        }
    };

    auto isWall = [&](int cx, int cy) {
        if (cy == 6 && (cx < 0 || cx >= COLS)) return false; 
        if (cx < 0 || cx >= COLS || cy < 0 || cy >= ROWS) return true; 
        return map[cy][cx] == 1; 
    };

    auto redrawMapTiles = [&](int x, int y) {
        int cx = (x < 0) ? (x - BLOCK_SIZE + 1) / BLOCK_SIZE : x / BLOCK_SIZE;
        int cy = y / BLOCK_SIZE;
        
        int startC = cx - 1; 
        int endC = cx + 1; 
        int startR = max(0, cy - 1);
        int endR = min(ROWS - 1, cy + 1);

        for (int r = startR; r <= endR; r++) {
            for (int c = startC; c <= endC; c++) {
                int tx = OFFSET_X + c * BLOCK_SIZE; 
                int ty = OFFSET_Y + r * BLOCK_SIZE;
                
                tft.fillRect(tx, ty, BLOCK_SIZE, BLOCK_SIZE, TFT_BLACK);
                
                if (c >= 0 && c < COLS) {
                    if (map[r][c] == 1) {
                        tft.drawRect(tx, ty, BLOCK_SIZE, BLOCK_SIZE, TFT_BLUE);
                        tft.drawRect(tx+2, ty+2, BLOCK_SIZE-4, BLOCK_SIZE-4, 0x0010); 
                    } else if (map[r][c] == 2) {
                        tft.fillCircle(tx+BLOCK_SIZE/2, ty+BLOCK_SIZE/2, 1, 0xFDA0);
                    } else if (map[r][c] == 3) {
                        tft.fillCircle(tx+BLOCK_SIZE/2, ty+BLOCK_SIZE/2, 3, TFT_WHITE);
                    }
                }
            }
        }
    };

    auto resetMap = [&]() {
        for(int r=0; r<ROWS; r++){
            for(int c=0; c<COLS; c++){
                map[r][c] = origMap[r][c]; 
            }
        }
    };

    auto initStage = [&]() {
        tft.fillScreen(TFT_BLACK);
        for(int r=0; r<ROWS; r++){
            for(int c=0; c<COLS; c++){
                int tx = OFFSET_X + c * BLOCK_SIZE; 
                int ty = OFFSET_Y + r * BLOCK_SIZE;
                
                if (map[r][c] == 1) {
                    tft.drawRect(tx, ty, BLOCK_SIZE, BLOCK_SIZE, TFT_BLUE);
                    tft.drawRect(tx+2, ty+2, BLOCK_SIZE-4, BLOCK_SIZE-4, 0x0010); 
                } else if (map[r][c] == 2) {
                    tft.fillCircle(tx+BLOCK_SIZE/2, ty+BLOCK_SIZE/2, 1, 0xFDA0);
                } else if (map[r][c] == 3) {
                    tft.fillCircle(tx+BLOCK_SIZE/2, ty+BLOCK_SIZE/2, 3, TFT_WHITE);
                }
            }
        }
        drawUI(true);
        drawLives();
    };

    auto resetEntities = [&]() {
        px = 5 * BLOCK_SIZE; py = 10 * BLOCK_SIZE; 
        pDirX = 0; pDirY = 0; nDirX = 0; nDirY = 0;
        
        ghosts[0] = {5 * BLOCK_SIZE, 6 * BLOCK_SIZE, 1, 0, TFT_RED}; 
        ghosts[1] = {6 * BLOCK_SIZE, 6 * BLOCK_SIZE, -1, 0, TFT_PINK}; 
        
        scaredTimer = 0; 
        scatterTimer = max(30, 150 - ((stage - 1) * 20)); 
        gameStarted = false;
        
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString(" READY! ", 56, 75, 1);
        
        playSFX("/PacMan_Leitmotiv.wav");
        delay(4244); 
        
        tft.fillRect(56, 75, 50, 10, TFT_BLACK); 
        redrawMapTiles(4 * BLOCK_SIZE, 8 * BLOCK_SIZE); 
        redrawMapTiles(7 * BLOCK_SIZE, 8 * BLOCK_SIZE);
    };

    resetMap(); 
    initStage(); 
    resetEntities();

    while (playing) {
        if (digitalRead(BTN_BACK) == LOW) { playing = false; delay(250); break; }
        
        frameCount++; 
        int joyX = analogRead(JOY_X); int joyY = analogRead(JOY_Y);

        if (joyX > 3000) { nDirX = 1; nDirY = 0; } 
        else if (joyX < 1000) { nDirX = -1; nDirY = 0; }
        else if (joyY > 3000) { nDirX = 0; nDirY = 1; } 
        else if (joyY < 1000) { nDirX = 0; nDirY = -1; }

        if (nDirX != 0 || nDirY != 0) { gameStarted = true; }

        int oldPx = px, oldPy = py;
        int oldGx[2] = {ghosts[0].x, ghosts[1].x};
        int oldGy[2] = {ghosts[0].y, ghosts[1].y};

        if (px % BLOCK_SIZE == 0 && py % BLOCK_SIZE == 0) {
            int cx = (px < 0) ? (px - BLOCK_SIZE + 1) / BLOCK_SIZE : px / BLOCK_SIZE;
            int cy = py / BLOCK_SIZE;
            
            if (cx >= 0 && cx < COLS && cy >= 0 && cy < ROWS) {
                if (map[cy][cx] == 2) { 
                    map[cy][cx] = 0; score+=10; 
                    if (scaredTimer == 0 && (millis() - lastWakaTime > 345)) {
                        playSFX("/PacMan_Waka.wav"); 
                        lastWakaTime = millis();
                    }
                    drawUI(); 
                } 
                else if (map[cy][cx] == 3) { 
                    map[cy][cx] = 0; score+=50; scaredTimer = 150; 
                    playSFX("/PacMan_Powerup.wav"); 
                    drawUI(); 
                }
            }
            
            bool quedenPunts = false;
            for(int r=0; r<ROWS; r++) {
                for(int c=0; c<COLS; c++) {
                    if(map[r][c] == 2 || map[r][c] == 3) { quedenPunts = true; break; }
                }
                if(quedenPunts) break;
            }
            
            if (!quedenPunts) { 
                stage++; 
                delay(500); // 500ms dramàtics, deixant sonar el Powerup si encara sona...
                playIntermission(); // Boom! Cançó tallada en sec i comença l'intermission!
                resetMap(); 
                initStage(); resetEntities(); continue; 
            }
            
            if (nDirX != 0 || nDirY != 0) { 
                if (!isWall(cx + nDirX, cy + nDirY)) { pDirX = nDirX; pDirY = nDirY; } 
            }
            if (isWall(cx + pDirX, cy + pDirY)) { pDirX = 0; pDirY = 0; }
        }

        px += pDirX * 1; py += pDirY * 1; 
        
        if (px < -BLOCK_SIZE) {
            tft.fillRect(OFFSET_X + oldPx, OFFSET_Y + oldPy, BLOCK_SIZE, BLOCK_SIZE, TFT_BLACK); 
            px = COLS * BLOCK_SIZE; oldPx = px; 
        } 
        else if (px > COLS * BLOCK_SIZE) {
            tft.fillRect(OFFSET_X + oldPx, OFFSET_Y + oldPy, BLOCK_SIZE, BLOCK_SIZE, TFT_BLACK); 
            px = -BLOCK_SIZE; oldPx = px;
        }
        
        if (scaredTimer > 0) scaredTimer--;
        if (scatterTimer > 0 && gameStarted) scatterTimer--;

        for(int i=0; i<2; i++) {
            if (gameStarted) {
                if (ghosts[i].x % BLOCK_SIZE == 0 && ghosts[i].y % BLOCK_SIZE == 0) {
                    int cx = (ghosts[i].x < 0) ? (ghosts[i].x - BLOCK_SIZE + 1) / BLOCK_SIZE : ghosts[i].x / BLOCK_SIZE;
                    int cy = ghosts[i].y / BLOCK_SIZE;
                    int validDirs[4][2]; int numValid = 0; 
                    int dirs[4][2] = {{1,0}, {-1,0}, {0,1}, {0,-1}};
                    
                    for(int d=0; d<4; d++) {
                        if (dirs[d][0] == -ghosts[i].dirX && dirs[d][1] == -ghosts[i].dirY && (ghosts[i].dirX!=0 || ghosts[i].dirY!=0)) continue; 
                        if (!isWall(cx + dirs[d][0], cy + dirs[d][1])) { 
                            validDirs[numValid][0] = dirs[d][0]; validDirs[numValid][1] = dirs[d][1]; numValid++; 
                        }
                    }
                    
                    if (numValid > 0) {
                        int bestDist = 99999; 
                        int bestIdx = 0;
                        int pacCx = (px < 0) ? (px - BLOCK_SIZE + 1) / BLOCK_SIZE : px / BLOCK_SIZE;
                        int pacCy = py / BLOCK_SIZE;

                        int blinkyErrRate = max(0, 1 - (stage - 1));
                        int pinkyErrRate  = max(0, 3 - (stage - 1));
                        int errorChance   = (i == 0) ? blinkyErrRate : pinkyErrRate;

                        for(int v=0; v<numValid; v++) {
                            int nx = cx + validDirs[v][0]; int ny = cy + validDirs[v][1];
                            int dist;

                            if (scaredTimer > 0) {
                                dist = -(abs(nx - pacCx) + abs(ny - pacCy));
                            } else if (scatterTimer > 0) {
                                int targetX = (i == 0) ? COLS-1 : 0;
                                dist = abs(nx - targetX) + abs(ny - 0);
                            } else {
                                if (i == 1) dist = abs(nx - (pacCx + pDirX*2)) + abs(ny - (pacCy + pDirY*2));
                                else dist = abs(nx - pacCx) + abs(ny - pacCy);
                            }

                            if (scaredTimer == 0 && scatterTimer == 0 && errorChance > 0 && random(10) < errorChance) {
                                if (random(2) == 0) { bestDist = dist; bestIdx = v; } 
                            } else {
                                if (dist < bestDist) { bestDist = dist; bestIdx = v; } 
                            }
                        }
                        ghosts[i].dirX = validDirs[bestIdx][0]; ghosts[i].dirY = validDirs[bestIdx][1];
                    } else { 
                        ghosts[i].dirX = -ghosts[i].dirX; ghosts[i].dirY = -ghosts[i].dirY; 
                    }
                }
                ghosts[i].x += ghosts[i].dirX * 1; ghosts[i].y += ghosts[i].dirY * 1;
                
                if (ghosts[i].x < -BLOCK_SIZE) {
                    tft.fillRect(OFFSET_X + oldGx[i], OFFSET_Y + oldGy[i], BLOCK_SIZE, BLOCK_SIZE, TFT_BLACK);
                    ghosts[i].x = COLS * BLOCK_SIZE; oldGx[i] = ghosts[i].x;
                } 
                else if (ghosts[i].x > COLS * BLOCK_SIZE) {
                    tft.fillRect(OFFSET_X + oldGx[i], OFFSET_Y + oldGy[i], BLOCK_SIZE, BLOCK_SIZE, TFT_BLACK);
                    ghosts[i].x = -BLOCK_SIZE; oldGx[i] = ghosts[i].x;
                }
            }
        }

        if (oldPx != px || oldPy != py) redrawMapTiles(oldPx, oldPy);
        for(int i=0; i<2; i++) {
            if(oldGx[i] != ghosts[i].x || oldGy[i] != ghosts[i].y || !gameStarted) {
                redrawMapTiles(oldGx[i], oldGy[i]);
            }
        }

        for(int i=0; i<2; i++) {
            uint16_t gColor = (scaredTimer > 0) ? TFT_BLUE : ghosts[i].color;
            int dgx = OFFSET_X + ghosts[i].x; int dgy = OFFSET_Y + ghosts[i].y;
            if (dgx < OFFSET_X - BLOCK_SIZE || dgx > OFFSET_X + COLS * BLOCK_SIZE) continue;
            tft.fillRect(dgx + 1, dgy + 2, BLOCK_SIZE - 2, BLOCK_SIZE - 2, gColor);
            tft.fillCircle(dgx + BLOCK_SIZE/2, dgy + 3, BLOCK_SIZE/2 - 1, gColor);
            tft.fillRect(dgx + 2, dgy + 3, 2, 2, TFT_WHITE); tft.fillRect(dgx + 5, dgy + 3, 2, 2, TFT_WHITE);
        }

        int dpx = OFFSET_X + px; int dpy = OFFSET_Y + py;
        if (dpx >= OFFSET_X - BLOCK_SIZE && dpx <= OFFSET_X + COLS * BLOCK_SIZE) {
            tft.fillCircle(dpx + BLOCK_SIZE/2, dpy + BLOCK_SIZE/2, BLOCK_SIZE/2 - 1, TFT_YELLOW);
            if ((frameCount / 4) % 2 == 0) {
                if(pDirX > 0) tft.fillTriangle(dpx+BLOCK_SIZE/2, dpy+BLOCK_SIZE/2, dpx+BLOCK_SIZE, dpy, dpx+BLOCK_SIZE, dpy+BLOCK_SIZE, TFT_BLACK);
                else if(pDirX < 0) tft.fillTriangle(dpx+BLOCK_SIZE/2, dpy+BLOCK_SIZE/2, dpx, dpy, dpx, dpy+BLOCK_SIZE, TFT_BLACK);
                else if(pDirY > 0) tft.fillTriangle(dpx+BLOCK_SIZE/2, dpy+BLOCK_SIZE/2, dpx, dpy+BLOCK_SIZE, dpx+BLOCK_SIZE, dpy+BLOCK_SIZE, TFT_BLACK);
                else if(pDirY < 0) tft.fillTriangle(dpx+BLOCK_SIZE/2, dpy+BLOCK_SIZE/2, dpx, dpy, dpx+BLOCK_SIZE, dpy, TFT_BLACK);
            }
        }

        // --- GESTIÓ COL·LISIONS NETA SENSE ÀUDIO DE "COMEFANTASMA" ---
        for(int i=0; i<2; i++) {
            if (abs(px - ghosts[i].x) < 5 && abs(py - ghosts[i].y) < 5) {
                if (scaredTimer > 0) {
                    score += 200; 
                    redrawMapTiles(ghosts[i].x, ghosts[i].y);
                    ghosts[i].x = 6 * BLOCK_SIZE; ghosts[i].y = 6 * BLOCK_SIZE; 
                    // NO fem sonar res aquí, respectem la sirena/silenci i que l'acció continuï naturalment
                } else {
                    lives--; 
                    drawLives(); 
                    
                    playSFX("/PacMan_GameOver.wav"); 
                    
                    for(int a=1; a<=4; a++) {
                        tft.fillRect(dpx, dpy, BLOCK_SIZE, BLOCK_SIZE, TFT_BLACK);
                        tft.fillCircle(dpx + BLOCK_SIZE/2, dpy + BLOCK_SIZE/2, BLOCK_SIZE/2 - 1, TFT_YELLOW);
                        tft.fillTriangle(dpx+BLOCK_SIZE/2, dpy+BLOCK_SIZE/2, dpx+BLOCK_SIZE/2 - a*2, dpy - 2, dpx+BLOCK_SIZE/2 + a*2, dpy - 2, TFT_BLACK);
                        delay(150); 
                    }
                    tft.fillRect(dpx, dpy, BLOCK_SIZE, BLOCK_SIZE, TFT_BLACK);
                    delay(1100); 

                    if (lives <= 0) {
                        if (score > highScore) guardarHighScore("hs_pacman", score);
                        tft.fillScreen(TFT_RED); tft.setTextColor(TFT_WHITE); 
                        tft.drawString("GAME OVER", 40, 55, 2); 
                        delay(2500); playing = false;
                    } else { 
                        resetEntities(); 
                        tft.fillRect(OFFSET_X, OFFSET_Y, COLS*BLOCK_SIZE, ROWS*BLOCK_SIZE, TFT_BLACK); 
                        initStage(); 
                    }
                }
            }
        }
        
        delay(max(20, 40 - ((stage - 1) * 2)));
    }
}

void playBreakout() {
    bool playing = true;
    int score = 0, lives = 3, stage = 1;
    const int BRK_ROWS = 5, BRK_COLS = 10, BW = 14, BH = 6, GAP = 2, FIELD_X = 1, FIELD_Y = 20;
    int board[BRK_ROWS][BRK_COLS];

    int padW = 24, padH = 4, padX = 68, oldPadX = 68, oldPadW = 24; const int padY = 115;
    
    const int MAX_BALLS = 4;
    struct Ball { float x, y, oldX, oldY, dx, dy; bool active; };
    Ball balls[MAX_BALLS]; 
    bool ballActive = false; 

    struct PowerUp { float x, y; int type; bool active; };
    PowerUp pups[3]; 

    auto renderHUD = [&]() {
        tft.fillRect(0, 0, 160, 12, TFT_BLACK); 
        tft.setTextColor(TFT_WHITE, TFT_BLACK);
        tft.drawString("SCR:" + String(score), 2, 2, 1);
        tft.drawString("LVL:" + String(stage) + " LVS:" + String(lives), 75, 2, 1);
    };

    auto initStage = [&]() {
        tft.fillScreen(TFT_BLACK);
        for (int r = 0; r < BRK_ROWS; r++) {
            uint16_t rowColor = (r==0)?TFT_RED:(r==1)?TFT_ORANGE:(r==2)?TFT_YELLOW:(r==3)?TFT_GREEN:TFT_BLUE;
            for (int c = 0; c < BRK_COLS; c++) { 
                
                // 1. GENERACIÓ DE NIVELLS I PROCEDURAL (Sense indestructibles)
                if(stage == 1) {
                    if (r == 0) board[r][c] = 2; // Fila superior blindada (2 cops)
                    else board[r][c] = 1;        // Resta normals
                } 
                else if(stage == 2) board[r][c] = (r + c) % 2; 
                else if(stage == 3) board[r][c] = (r == c || r + c == BRK_COLS - 1 || r == 0 || r == BRK_ROWS -1) ? 1 : 0;
                else {
                    // Nivells 4 endavant infinits
                    int atzar = random(0, 100);
                    if (atzar < 50) board[r][c] = 1;      // 50% Normal
                    else if (atzar < 65) board[r][c] = 2; // 15% Blindat
                    else board[r][c] = 0;                 // 35% Buit
                }
                
                // Pintem els blocs
                if(board[r][c] == 2) tft.fillRect(FIELD_X + c*(BW+GAP), FIELD_Y + r*(BH+GAP), BW, BH, rowColor); 
                else if(board[r][c] == 1) tft.fillRect(FIELD_X + c*(BW+GAP), FIELD_Y + r*(BH+GAP), BW, BH, rowColor); 
            }
        }
        for(int i=0; i<3; i++) pups[i].active = false;
        for(int i=0; i<MAX_BALLS; i++) balls[i].active = false;
        renderHUD(); 
        ballActive = false;
    };

    initStage();

    while (playing) {
        if (digitalRead(BTN_BACK) == LOW) { playing = false; delay(250); break; }

        int joyX = analogRead(JOY_X); 
        oldPadX = padX; oldPadW = padW;
        if (joyX < 1000) { padX -= 3; if (padX < 0) padX = 0; }
        if (joyX > 3000) { padX += 3; if (padX > 160 - padW) padX = 160 - padW; }

        if (!ballActive) {
            balls[0].oldX = balls[0].x; balls[0].oldY = balls[0].y; 
            balls[0].x = padX + (padW / 2.0f) - 2; balls[0].y = padY - 5;
            
            if (digitalRead(BTN_SELECT) == LOW) { 
                tft.fillRect((int)balls[0].oldX, (int)balls[0].oldY, 4, 4, TFT_BLACK); 
                tft.fillRect((int)balls[0].x, (int)balls[0].y, 4, 4, TFT_BLACK); 
                balls[0].active = true; 
                balls[0].dx = (random(0,2)==0)?-1.5:1.5; 
                balls[0].dy = -2.0; 
                ballActive = true; 
                playSFX("/Pong_wall_bounce.wav"); 
                delay(200); 
            }
        }

        bool stageClear = true;
        for(int b=0; b<MAX_BALLS; b++) {
            if(!balls[b].active) continue;
            balls[b].oldX = balls[b].x; balls[b].oldY = balls[b].y;
            balls[b].x += balls[b].dx; balls[b].y += balls[b].dy;
            
            if (balls[b].x <= 0 || balls[b].x >= 156) { balls[b].dx = -balls[b].dx; balls[b].x = (balls[b].x<=0)?0:156; playSFX("/Pong_wall_bounce.wav");; }
            if (balls[b].y <= 12) { balls[b].dy = -balls[b].dy; balls[b].y = 12; playSFX("/Pong_wall_bounce.wav");; }
            if (balls[b].dy > 0 && balls[b].y + 4 >= padY && balls[b].y <= padY + padH && balls[b].x + 4 >= padX && balls[b].x <= padX + padW) {
                balls[b].dy = -balls[b].dy; balls[b].y = padY - 4; float hitPos = (balls[b].x + 2) - (padX + (padW / 2.0)); balls[b].dx = hitPos * 0.15;
                playSound(3);
            }
            
            if (balls[b].y >= FIELD_Y && balls[b].y <= FIELD_Y + BRK_ROWS*(BH+GAP)) {
                int c = (balls[b].x - FIELD_X) / (BW+GAP); int r = (balls[b].y - FIELD_Y) / (BH+GAP);
                if (r >= 0 && r < BRK_ROWS && c >= 0 && c < BRK_COLS && board[r][c] > 0) {
                    
                    int bx = FIELD_X + c * (BW + GAP);
                    int by = FIELD_Y + r * (BH + GAP);

                    // TRENQUEM EL BLOC
                    board[r][c]--; 
                    score += 10; 
                    playSound(2);
                    
                    if (board[r][c] == 1) {
                        tft.fillRect(bx, by, BW, BH, TFT_WHITE); // Bloc blindat es torna blanc
                    } else if (board[r][c] == 0) {
                        tft.fillRect(bx, by, BW, BH, TFT_BLACK); // Bloc destruït totalment
                        tft.fillRect((int)balls[b].oldX, (int)balls[b].oldY, 4, 4, TFT_BLACK);
                        tft.fillRect((int)balls[b].x, (int)balls[b].y, 4, 4, TFT_BLACK);
                        renderHUD();
                        
                        if(random(0, 100) < 15) {
                            for(int p=0; p<3; p++) {
                                if(!pups[p].active) {
                                    pups[p].x = bx + BW/2; pups[p].y = by;
                                    pups[p].type = random(0, 2); pups[p].active = true; break;
                                }
                            }
                        }
                    }
                    
                    // Lògica de rebot anti-bugs
                    if (balls[b].oldX + 4 <= bx || balls[b].oldX >= bx + BW) {
                        balls[b].dx = -balls[b].dx; 
                    } else {
                        balls[b].dy = -balls[b].dy; 
                    }
                    
                    goto endBlockCheck; 
                }
            }
        }
        endBlockCheck:;
        
        // Com que ja no hi ha indestructibles, mirem tots els blocs vius per guanyar
        for (int r = 0; r < BRK_ROWS; r++) for (int c = 0; c < BRK_COLS; c++) if (board[r][c] > 0) stageClear = false;

        for(int p=0; p<3; p++) {
            if(pups[p].active) {
                
                // Evitem que el powerup esborri els blocs sans al baixar
                if (pups[p].y > FIELD_Y + BRK_ROWS * (BH + GAP)) {
                    tft.fillRect((int)pups[p].x, (int)pups[p].y, 6, 6, TFT_BLACK);
                } else {
                    tft.drawFastHLine((int)pups[p].x, (int)pups[p].y, 6, TFT_BLACK);
                }
                
                pups[p].y += 1.0;
                
                if(pups[p].y + 6 >= padY && pups[p].y <= padY + padH && pups[p].x + 6 >= padX && pups[p].x <= padX + padW) {
                    pups[p].active = false; score += 20; renderHUD(); playSound(6);
                    tft.fillRect((int)pups[p].x, (int)pups[p].y - 1, 6, 7, TFT_BLACK); 
                    if(pups[p].type == 0 && padW < 48) { oldPadW = padW; padW += 8; } 
                    else if(pups[p].type == 1) {
                        int spawn = 0; for(int b=1; b<MAX_BALLS && spawn<2; b++) {
                            if(!balls[b].active) { 
                                balls[b].active = true; balls[b].x = padX + padW/2; balls[b].y = padY - 5; 
                                balls[b].dx = (spawn==0)?-1.5:1.5; balls[b].dy = -2.0; spawn++; 
                            }
                        }
                    }
                }
                else if(pups[p].y > 128) {
                    pups[p].active = false;
                    tft.fillRect((int)pups[p].x, (int)pups[p].y, 6, 6, TFT_BLACK); 
                }
                else tft.fillRect((int)pups[p].x, (int)pups[p].y, 6, 6, (pups[p].type == 0)?TFT_CYAN:TFT_MAGENTA);
            }
        }

        if (stageClear) { stage++; initStage(); delay(1000); }
        else {
            bool anyAlive = false;
            for(int b=0; b<MAX_BALLS; b++) {
                if(balls[b].active) {
                    if (balls[b].y > 128) {
                        tft.fillRect((int)balls[b].oldX, (int)balls[b].oldY, 4, 4, TFT_BLACK);
                        tft.fillRect((int)balls[b].x, (int)balls[b].y, 4, 4, TFT_BLACK);
                        balls[b].active = false; 
                    } else anyAlive = true;
                }
            }
            if(!anyAlive && ballActive) {
                tft.fillRect(padX, padY, padW, padH, TFT_BLACK);
                lives--; playSound(4); renderHUD(); padW = 24; oldPadW = 24;
                for(int p=0; p<3; p++) { if(pups[p].active) tft.fillRect((int)pups[p].x, (int)pups[p].y, 6, 6, TFT_BLACK); pups[p].active = false; }
                if (lives <= 0) { tft.fillScreen(TFT_RED); tft.drawString("GAME OVER", 40, 55, 2); delay(2000); playing = false; }
                else { ballActive = false; }
            }
        }

        if (oldPadX != padX || oldPadW != padW) {
            tft.fillRect(oldPadX, padY, oldPadW, padH, TFT_BLACK); oldPadW = padW;
        }
        tft.fillRect(padX, padY, padW, padH, TFT_WHITE);
        
        for(int b=0; b<MAX_BALLS; b++) {
            if (balls[b].active || (!ballActive && b == 0)) {
                if ((int)balls[b].oldX != (int)balls[b].x || (int)balls[b].oldY != (int)balls[b].y) {
                    tft.fillRect((int)balls[b].oldX, (int)balls[b].oldY, 4, 4, TFT_BLACK);
                }
                tft.fillRect((int)balls[b].x, (int)balls[b].y, 4, 4, TFT_YELLOW);
            }
        }
        delay(30); 
    }
}

void playFrogger() {
    bool playing = true;
    int score = 0, lives = 3, stage = 1;
    float frogX = 76, frogY = 110;
    int oldFX = 76, oldFY = 110;
    bool homes[5] = {false};
    int homesFilled = 0;
    bool pU = false, pD = false, pL = false, pR = false; 

    struct Lane { int y; float speed; int w; int spacing; int type; uint16_t color; float offset; };
    Lane lanes[8] = {
        {100, 0.8, 14, 50, 0, TFT_YELLOW, 0},
        {90, -1.0, 14, 60, 0, TFT_CYAN, 20},
        {80, 1.2, 14, 55, 0, TFT_WHITE, 10},
        {70, -0.9, 14, 65, 0, TFT_YELLOW, 30},
        {50, -0.8, 20, 50, 2, TFT_RED, 0},     
        {40, 1.1, 35, 70, 1, TFT_BROWN, 25},  
        {30, 1.4, 25, 80, 1, TFT_BROWN, 10},
        {20, -1.0, 20, 55, 2, TFT_RED, 40}
    };

    auto drawBackground = [&]() {
        tft.fillScreen(TFT_BLACK);
        tft.fillRect(0, 20, 160, 40, TFT_BLUE);   
        tft.fillRect(0, 60, 160, 10, TFT_PURPLE);  
        tft.fillRect(0, 110, 160, 10, TFT_PURPLE); 
        tft.fillRect(0, 10, 160, 10, TFT_GREEN);   
        
        for(int i=0; i<5; i++) {
            int hx = 12 + i*32;
            if(!homes[i]) {
                tft.fillRect(hx, 10, 12, 10, TFT_BLUE);
            } else {
                tft.fillRect(hx, 10, 12, 10, TFT_BLUE); 
                tft.fillRect(hx+4, 10+2, 4, 4, TFT_GREEN);
                tft.fillRect(hx+2, 10, 2, 2, TFT_YELLOW); 
                tft.fillRect(hx+8, 10, 2, 2, TFT_YELLOW); 
                tft.fillRect(hx+2, 10+6, 2, 2, TFT_YELLOW); 
                tft.fillRect(hx+8, 10+6, 2, 2, TFT_YELLOW); 
            }
        }
        tft.setTextColor(TFT_YELLOW, TFT_BLACK);
        tft.drawString("LV:" + String(stage) + " LVS:" + String(lives) + " SC:" + String(score), 2, 0, 1);
    };

    auto resetFrog = [&]() { frogX = 76; frogY = 110; oldFX = 76; oldFY = 110; };
    drawBackground();

    while(playing) {
        if (digitalRead(BTN_BACK) == LOW) { playing = false; delay(250); break; }
        
        int jX = analogRead(JOY_X); int jY = analogRead(JOY_Y);
        bool cU = (jY < 1000), cD = (jY > 3000), cL = (jX < 1000), cR = (jX > 3000);
        oldFX = frogX; oldFY = frogY;

        if (cU && !pU && frogY > 10) { frogY -= 10; score += 10; }
        if (cD && !pD && frogY < 110) { frogY += 10; }
        if (cL && !pL && frogX > 4) { frogX -= 10; }
        if (cR && !pR && frogX < 146) { frogX += 10; }
        
        pU = cU; pD = cD; pL = cL; pR = cR;

        for(int l=0; l<8; l++) {
            lanes[l].offset += lanes[l].speed;
            if(lanes[l].offset > lanes[l].spacing) lanes[l].offset -= lanes[l].spacing;
            if(lanes[l].offset < 0) lanes[l].offset += lanes[l].spacing;
        }

        bool dead = false, onLogOrTurtle = false; float frogRideSpeed = 0;

        if(frogY >= 70 && frogY <= 100) { 
            int lIdx = (100 - (int)frogY) / 10; Lane& l = lanes[lIdx];
            for(float x = l.offset - l.spacing; x < 160; x += l.spacing) {
                if(frogX + 6 > x && frogX + 2 < x + l.w) dead = true;
            }
        } else if(frogY >= 20 && frogY <= 50) { 
            int lIdx = 4 + (50 - (int)frogY) / 10; Lane& l = lanes[lIdx];
            for(float x = l.offset - l.spacing; x < 160; x += l.spacing) {
                if(frogX + 4 >= x && frogX + 4 <= x + l.w) { 
                    onLogOrTurtle = true; frogRideSpeed = l.speed; break; 
                }
            }
            if(!onLogOrTurtle) dead = true;
        }

        if(onLogOrTurtle) frogX += frogRideSpeed;
        if(frogX < 0 || frogX > 152) dead = true;

        if(frogY == 10) {
            int homeIdx = -1;
            for(int i=0; i<5; i++) {
                int hx = 12 + i*32; 
                if(frogX + 4 >= hx && frogX + 4 <= hx + 12) homeIdx = i;
            }
            if(homeIdx != -1 && !homes[homeIdx]) {
                homes[homeIdx] = true; homesFilled++; score += 50; playSFX("/Frogger_point.wav");
                
                drawBackground(); 

                if(homesFilled >= 5) {
                    stage++; homesFilled = 0; for(int i=0; i<5; i++) homes[i] = false;
                    for(int l=0; l<8; l++) lanes[l].speed *= 1.1; 
                    drawBackground();
                }
                resetFrog();
            } else dead = true; 
        }

        if(dead) {
            lives--; playSFX("/Frogger_Death.wav");
            if(lives <= 0) {
                tft.fillScreen(TFT_RED); tft.setTextColor(TFT_WHITE);
                tft.drawString("GAME OVER", 40, 55, 2); delay(2000); playing = false; break;
            } else { resetFrog(); drawBackground(); delay(1000); }
        } else {
            for(int l=0; l<8; l++) {
                tft.fillRect(0, lanes[l].y, 160, 10, (lanes[l].type == 0) ? TFT_BLACK : TFT_BLUE);
                for(float x = lanes[l].offset - lanes[l].spacing; x < 160; x += lanes[l].spacing) {
                    if(x + lanes[l].w > 0 && x < 160) {
                        
                        if(lanes[l].type == 2) { 
                            for(int tx=0; tx<lanes[l].w; tx+=10) {
                                tft.fillRect(x+tx+1, lanes[l].y+2, 8, 6, TFT_GREEN);
                                tft.fillCircle(x+tx+5, lanes[l].y+5, 3, TFT_RED); 
                            }
                        } else if (lanes[l].type == 1) {
                            tft.fillRect(x, lanes[l].y+1, lanes[l].w, 8, TFT_BROWN);
                            tft.fillRect(x, lanes[l].y+1, 3, 8, 0xFDA0); 
                            tft.drawFastHLine(x, lanes[l].y+3, lanes[l].w, TFT_BLACK);
                            tft.drawFastHLine(x, lanes[l].y+7, lanes[l].w, TFT_BLACK);
                        } else {
                            tft.fillRect(x+1, lanes[l].y+1, lanes[l].w-2, 8, lanes[l].color);
                            tft.fillRect(x+2, lanes[l].y, lanes[l].w-4, 10, lanes[l].color);
                            tft.fillRect(x+2, lanes[l].y, 3, 2, TFT_WHITE); 
                            tft.fillRect(x+lanes[l].w-5, lanes[l].y, 3, 2, TFT_WHITE);
                            tft.fillRect(x+2, lanes[l].y+8, 3, 2, TFT_WHITE); 
                            tft.fillRect(x+lanes[l].w-5, lanes[l].y+8, 3, 2, TFT_WHITE);
                            tft.fillRect((lanes[l].speed > 0) ? x+lanes[l].w-4 : x+2, lanes[l].y+2, 2, 6, TFT_CYAN);
                        }
                    }
                }
            }
            if(oldFY == 110 || oldFY == 60) tft.fillRect(oldFX, oldFY, 8, 8, TFT_PURPLE);
            
            tft.fillRect((int)frogX+2, (int)frogY+2, 4, 4, TFT_GREEN);
            tft.fillRect((int)frogX, (int)frogY, 2, 2, TFT_YELLOW); 
            tft.fillRect((int)frogX+6, (int)frogY, 2, 2, TFT_YELLOW); 
            tft.fillRect((int)frogX, (int)frogY+6, 2, 2, TFT_YELLOW); 
            tft.fillRect((int)frogX+6, (int)frogY+6, 2, 2, TFT_YELLOW); 
        }
        delay(30);
    }
}