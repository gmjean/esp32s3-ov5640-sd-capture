#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <ctype.h>

// =========================
// PINAGEM DA SUA PLACA
// =========================
#define PWDN_GPIO_NUM    -1
#define RESET_GPIO_NUM   -1
#define XCLK_GPIO_NUM    15
#define SIOD_GPIO_NUM    4   // SDA (SCCB)
#define SIOC_GPIO_NUM    5   // SCL (SCCB)

#define Y2_GPIO_NUM      11  // D0
#define Y3_GPIO_NUM      9   // D1
#define Y4_GPIO_NUM      8   // D2
#define Y5_GPIO_NUM      10  // D3
#define Y6_GPIO_NUM      12  // D4
#define Y7_GPIO_NUM      18  // D5
#define Y8_GPIO_NUM      17  // D6
#define Y9_GPIO_NUM      16  // D7

#define VSYNC_GPIO_NUM   6
#define HREF_GPIO_NUM    7
#define PCLK_GPIO_NUM    13

// =========================
// PINAGEM SD (1-bit SD_MMC)
// =========================
#define SD_CLK_GPIO      39
#define SD_CMD_GPIO      38
#define SD_D0_GPIO       40

// =========================
// PERFIL DE CAPTURA
// =========================
// Perfil "idle" (= estável comprovado)
#define IDLE_FRAMESIZE       FRAMESIZE_SVGA   // 800x600 baseline estável
#define IDLE_JPEG_QUALITY    15               // ~15 foi estável pra você
#define IDLE_FB_COUNT        2                // fb_count = 2
// Perfil "foto boa" (= máxima qualidade que queremos testar)
#define CAPTURE_FRAMESIZE    FRAMESIZE_UXGA   // 1600x1200
#define CAPTURE_JPEG_QUALITY 10               // qualidade alta (menor número = menos compressão)

// =========================
// ESTADO GLOBAL
// =========================
static unsigned int imageCounter = 0;
static bool sdMounted = false;

// -------------------------
// Procura último índice já usado em /DCIM
// -------------------------
unsigned int findLastImageNumber() {
    unsigned int maxNum = 0;
    File root = SD_MMC.open("/DCIM");
    if (!root || !root.isDirectory()) {
        return 0;
    }
    File f = root.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            String name = f.name(); // pode vir tipo "/DCIM/IMG_00001_..."
            int idx = name.indexOf("IMG_");
            if (idx >= 0 && idx + 9 <= (int)name.length()) {
                // Espera "IMG_00001"
                String numStr = name.substring(idx + 4, idx + 9);
                bool allDigits = true;
                for (uint8_t i = 0; i < numStr.length(); i++) {
                    if (!isDigit(numStr[i])) { allDigits = false; break; }
                }
                if (allDigits) {
                    unsigned int n = numStr.toInt();
                    if (n > maxNum) maxNum = n;
                }
            }
        }
        f.close();
        f = root.openNextFile();
    }
    root.close();
    return maxNum;
}

// -------------------------
// Inicializa câmera no modo "idle estável"
// -------------------------
bool initCameraIdle() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;

    // MUITO IMPORTANTE: manter 24 MHz
    config.xclk_freq_hz = 24000000;

    config.pixel_format = PIXFORMAT_JPEG;          // sempre JPEG
    config.frame_size   = IDLE_FRAMESIZE;          // SVGA baseline
    config.jpeg_quality = IDLE_JPEG_QUALITY;       // ~15 baseline
    config.fb_count     = IDLE_FB_COUNT;           // 2 buffers
    config.fb_location  = CAMERA_FB_IN_PSRAM;      // usar PSRAM
    config.grab_mode    = CAMERA_GRAB_LATEST;      // pegar frame mais recente

    // Checar PSRAM
    if (!psramFound()) {
        // fallback se PSRAM não for detectada (mas seu módulo tem PSRAM)
        config.fb_location = CAMERA_FB_IN_DRAM;
        config.fb_count    = 1;
    }

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("ERRO initCameraIdle(): esp_camera_init falhou 0x%x\n", err);
        return false;
    }

    // Ajustes do sensor iguais aos que você já confirmou estáveis
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_brightness(s, 1);
        s->set_saturation(s, 0);
        // Mantemos auto-exposure / auto white balance ligados
        // Dica prática vista em OV5640 no S3:
        // às vezes precisa ~1s depois do primeiro init pra estabilizar cor/exposição. :contentReference[oaicite:10]{index=10}
    }

    return true;
}

// -------------------------
// Inicializa / remonta SD, cria /DCIM e atualiza contador
// -------------------------
bool ensureSDMounted() {
    if (sdMounted) {
        return true;
    }

    SD_MMC.setPins(SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO);

    // Ordem confirmada: câmera já inicializada -> agora montar SD
    // true = modo 1-bit, false = sem auto format, SDMMC_FREQ_DEFAULT ~20MHz típico
    if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT)) {
        Serial.println("[SD] ERRO: não montou SD_MMC");
        return false;
    }

    // Confirma que tem cartão
    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("[SD] ERRO: nenhum cartão detectado");
        return false;
    }

    // Cria diretório DCIM se não existir
    if (!SD_MMC.exists("/DCIM")) {
        if (!SD_MMC.mkdir("/DCIM")) {
            Serial.println("[SD] ERRO: não conseguiu criar /DCIM");
            return false;
        }
    }

    // Atualiza contador sequencial pra não sobrescrever
    imageCounter = findLastImageNumber();
    Serial.printf("[SD] OK: montado. Última imagem %05u\n", imageCounter);

    sdMounted = true;
    return true;
}

// -------------------------
// Captura foto em alta qualidade e salva no SD
// - Ajusta sensor pra UXGA/Q10
// - Espera estabilizar
// - Captura, salva, loga
// - Volta pro modo idle
// -------------------------
void captureAndSaveOnePhoto() {
    if (!ensureSDMounted()) {
        Serial.println("SAVE abortado: SD não montado.");
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (!s) {
        Serial.println("SAVE abortado: sensor_t nulo.");
        return;
    }

    // Sobe para perfil de captura pesada
    s->set_framesize(s, CAPTURE_FRAMESIZE);       // UXGA
    s->set_quality(s,   CAPTURE_JPEG_QUALITY);    // Q10 ~ melhor imagem
    // Pequena pausa pro AEC/AWB se adaptar
    delay(200);

    // Mede tempos
    unsigned long t0 = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    unsigned long t1 = millis();

    if (!fb) {
        Serial.println("SAVE ERRO: esp_camera_fb_get() retornou NULL");
        // Volta pro perfil idle mesmo assim
        s->set_framesize(s, IDLE_FRAMESIZE);
        s->set_quality(s,   IDLE_JPEG_QUALITY);
        return;
    }

    // Gera nome do arquivo
    imageCounter++;
    char imgPath[96];
    snprintf(
        imgPath,
        sizeof(imgPath),
        "/DCIM/IMG_%05u_UXGA_Q%02d_FB2.jpg",
        imageCounter,
        CAPTURE_JPEG_QUALITY
    );

    File jpgFile = SD_MMC.open(imgPath, FILE_WRITE);
    if (!jpgFile) {
        Serial.println("SAVE ERRO: falha ao criar arquivo JPG no SD");
        imageCounter--; // reverte índice
        esp_camera_fb_return(fb);
        s->set_framesize(s, IDLE_FRAMESIZE);
        s->set_quality(s,   IDLE_JPEG_QUALITY);
        return;
    }

    size_t written = jpgFile.write(fb->buf, fb->len);
    jpgFile.close();

    unsigned long t2 = millis();

    if (written != fb->len) {
        Serial.println("SAVE ERRO: escrita incompleta no SD, removendo arquivo...");
        SD_MMC.remove(imgPath);
        imageCounter--; // reverte índice
        esp_camera_fb_return(fb);
        s->set_framesize(s, IDLE_FRAMESIZE);
        s->set_quality(s,   IDLE_JPEG_QUALITY);
        return;
    }

    // Log CSV
    File logFile = SD_MMC.open("/DCIM/LOG.CSV", FILE_APPEND);
    if (logFile) {
        // CSV: index,arquivo,bytes,capture_ms,save_ms,total_ms,ok
        unsigned long capture_ms = (t1 - t0);
        unsigned long save_ms    = (t2 - t1);
        unsigned long total_ms   = (t2 - t0);
        logFile.printf(
            "%05u,%s,%u,%lu,%lu,%lu,OK\n",
            imageCounter,
            imgPath + 6, // tira "/DCIM/"
            (unsigned int)fb->len,
            capture_ms,
            save_ms,
            total_ms
        );
        logFile.close();
    } else {
        Serial.println("AVISO: não conseguiu abrir /DCIM/LOG.CSV para append");
    }

    Serial.printf(
        "SAVE OK: %s (%u bytes). capture=%lums save=%lums total=%lums\n",
        imgPath,
        (unsigned int)fb->len,
        (t1 - t0),
        (t2 - t1),
        (t2 - t0)
    );

    // Libera o frame buffer
    // Isso é obrigatório sempre depois de usar fb, senão você perde buffers e começa a ver erros de overflow tipo 'cam_hal: FB-OVF'. :contentReference[oaicite:11]{index=11}
    esp_camera_fb_return(fb);

    // Volta pro modo idle (mais leve e estável)
    s->set_framesize(s, IDLE_FRAMESIZE);     // SVGA
    s->set_quality(s,   IDLE_JPEG_QUALITY);  // ~15
}

// -------------------------
// SETUP
// -------------------------
void setup() {
    Serial.begin(115200);
    Serial.println();
    Serial.println("Boot... inicializando camera baseline SVGA/Q15...");

    if (!initCameraIdle()) {
        Serial.println("FALHA CRÍTICA: câmera não inicializou.");
        Serial.println("Possíveis causas:");
        Serial.println("- Driver/board atual não tem suporte OV5640 (erro 0x106).");
        Serial.println("- Flat cable frouxo.");
        // Fica travado porque sem câmera nada funciona
        while (true) { delay(1000); }
    }

    Serial.println("Câmera OK. Agora montando SD...");
    if (!ensureSDMounted()) {
        Serial.println("AVISO: SD não montado ainda. Você ainda pode mandar SAVE e eu vou tentar montar de novo.");
    }

    Serial.println("Pronto. Digite SAVE na Serial para tirar uma foto em UXGA/Q10 e salvar no SD.");
}

// -------------------------
// LOOP
// -------------------------
void loop() {
    // Escuta comandos na Serial
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd.equalsIgnoreCase("SAVE")) {
            Serial.println("Comando SAVE recebido. Capturando foto...");
            captureAndSaveOnePhoto();
        } else {
            Serial.println("Comando não reconhecido. Use SAVE.");
        }
    }

    delay(10);
}
