#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <ctype.h>

/*
  ESP32-S3 WROOM-1-N16R8 + OV5640 (FPC) + microSD (SD_MMC 1-bit)
  - Resolução sempre UXGA (1600x1200)
  - XCLK fixo em 24 MHz (NÃO ALTERAR)
  - Buffers em PSRAM, fb_count = 2
  - SD_MMC nos pinos 39 (CLK), 38 (CMD), 40 (D0)
  - Comando na Serial: SAVE -> captura foto UXGA e salva em /DCIM
  - Log em /DCIM/LOG.CSV
  - Ajustes de exposição (auto ou manual) e white balance
*/

// =========================
// PINAGEM DA SUA PLACA (CÂMERA)
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
// PERFIL: SEMPRE UXGA
// =========================
#define IDLE_FRAMESIZE        FRAMESIZE_UXGA     // 1600x1200 sempre
#define IDLE_JPEG_QUALITY     12                 // qualidade no idle (12 = equilíbrio)
#define IDLE_FB_COUNT         2

#define CAPTURE_FRAMESIZE     FRAMESIZE_UXGA     // captura também UXGA
#define CAPTURE_JPEG_QUALITY  10                 // presets: 10 (máx detalhe) | 12 (equilíbrio) | 15 (compacto)

// =========================
// EXPOSIÇÃO & WHITE BALANCE
// =========================
// Use AUTO = 1 para deixar o AEC (auto-exposure) ativo.
// Se precisar escurecer a imagem de forma consistente, teste AUTO = 0 e ajuste MANUAL_AEC_VALUE (menor = mais escuro).
#define EXPOSURE_MODE_AUTO    1      // 1 = AEC ligado | 0 = exposição manual
#define MANUAL_AEC_VALUE      260    // 0..1200 (menor = mais escuro); só usado se EXPOSURE_MODE_AUTO=0
#define AE_SETTLE_MS          400    // tempo para AEC/AWB estabilizarem após mudança

// White balance: Auto (USE_WB_AUTO=1) ou modo fixo (Sunny/Cloudy/Office/Home)
#define USE_WB_AUTO           1      // 1 = AWB auto | 0 = usar modo fixo abaixo
#define WB_MODE_FIXED         1      // 1=Sunny, 2=Cloudy, 3=Office, 4=Home (se USE_WB_AUTO=0)

// =========================
// ESTADO GLOBAL
// =========================
static unsigned int imageCounter = 0;
static bool sdMounted = false;

// -------------------------
// Utilitário: aplica ajustes comuns de sensor (exposição/WB).
// -------------------------
static void applySensorTuning(sensor_t* s) {
  if (!s) return;

  // Seu baseline estável (ajuste fino aqui se quiser)
  s->set_vflip(s, 1);
  s->set_brightness(s, 0);     // 0 (neutro). Se quiser “mais escuro” sem mexer em AEC, pode usar -1
  s->set_saturation(s, 0);

  // AEC (exposição)
  s->set_aec2(s, 1);           // algoritmo de exposição melhorado
#if EXPOSURE_MODE_AUTO
  s->set_exposure_ctrl(s, 1);  // AEC ON
#else
  s->set_exposure_ctrl(s, 0);  // AEC OFF (manual)
  s->set_aec_value(s, MANUAL_AEC_VALUE); // menor = mais escuro
#endif

  // AWB (white balance)
#if USE_WB_AUTO
  s->set_whitebal(s, 1);       // AWB ON
  s->set_awb_gain(s, 1);
#else
  s->set_whitebal(s, 1);       // mantém AWB infra ligado
  s->set_awb_gain(s, 1);
  s->set_wb_mode(s, WB_MODE_FIXED); // 1=Sunny,2=Cloudy,3=Office,4=Home
#endif
}

// -------------------------
// Procura último índice já usado em /DCIM
// -------------------------
unsigned int findLastImageNumber() {
  unsigned int maxNum = 0;
  File root = SD_MMC.open("/DCIM");
  if (!root || !root.isDirectory()) return 0;

  for (;;) {
    File f = root.openNextFile();
    if (!f) break;
    if (!f.isDirectory()) {
      const char* nm = f.name();
      if (nm) {
        const char* p = strstr(nm, "IMG_");
        if (p && strlen(p) >= 9) { // "IMG_" + 5 dígitos
          char buf[6] = {0};
          memcpy(buf, p + 4, 5);
          bool ok = true;
          for (int i = 0; i < 5; i++) if (!isdigit((unsigned char)buf[i])) { ok = false; break; }
          if (ok) { unsigned n = (unsigned)atoi(buf); if (n > maxNum) maxNum = n; }
        }
      }
    }
    f.close();
  }
  root.close();
  return maxNum;
}

// -------------------------
// Inicializa câmera no modo UXGA "idle"
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

  config.pixel_format = PIXFORMAT_JPEG;               // sempre JPEG
  config.frame_size   = IDLE_FRAMESIZE;               // UXGA
  config.jpeg_quality = IDLE_JPEG_QUALITY;            // 12 (equilíbrio no idle)
  config.fb_count     = IDLE_FB_COUNT;                // 2 buffers
  config.fb_location  = CAMERA_FB_IN_PSRAM;           // usar PSRAM
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;       // só captura quando pedir (reduz FB-OVF em idle)

  if (!psramFound()) {
    // Seu módulo tem PSRAM; se não detectar, faz fallback (ainda assim, UXGA pode ficar limitado)
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count    = 1;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("ERRO initCameraIdle(): esp_camera_init falhou 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  applySensorTuning(s);

  // Dá um tempo pro AEC/AWB estabilizarem logo no boot
  delay(AE_SETTLE_MS);
  return true;
}

// -------------------------
// Inicializa / remonta SD, cria /DCIM e atualiza contador
// -------------------------
bool ensureSDMounted() {
  if (sdMounted) return true;

  SD_MMC.setPins(SD_CLK_GPIO, SD_CMD_GPIO, SD_D0_GPIO);
  if (!SD_MMC.begin("/sdcard", true, false, SDMMC_FREQ_DEFAULT)) {
    Serial.println("[SD] ERRO: não montou SD_MMC");
    return false;
  }
  if (SD_MMC.cardType() == CARD_NONE) {
    Serial.println("[SD] ERRO: nenhum cartão detectado");
    return false;
  }
  if (!SD_MMC.exists("/DCIM")) {
    if (!SD_MMC.mkdir("/DCIM")) {
      Serial.println("[SD] ERRO: não conseguiu criar /DCIM");
      return false;
    }
  }
  imageCounter = findLastImageNumber();
  Serial.printf("[SD] OK: montado. Última imagem %05u\n", imageCounter);

  sdMounted = true;
  return true;
}

// -------------------------
// Captura foto UXGA e salva no SD
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

  // Garantir perfil de captura (UXGA) + qualidade selecionada
  s->set_framesize(s, CAPTURE_FRAMESIZE);       // UXGA
  s->set_quality(s,   CAPTURE_JPEG_QUALITY);    // 10/12/15 conforme define
  // (Re)aplica tuning (caso tenha sido alterado antes)
  applySensorTuning(s);
  delay(AE_SETTLE_MS);

  // Tempo de captura
  unsigned long t0 = millis();
  camera_fb_t *fb = esp_camera_fb_get();
  unsigned long t1 = millis();

  if (!fb) {
    Serial.println("SAVE ERRO: esp_camera_fb_get() retornou NULL");
    return;
  }

  // Gera nome /DCIM/IMG_XXXXX_UXGA_Qnn_FB2.jpg
  imageCounter++;
  char imgPath[96];
  snprintf(imgPath, sizeof(imgPath),
           "/DCIM/IMG_%05u_UXGA_Q%02d_FB2.jpg",
           imageCounter, CAPTURE_JPEG_QUALITY);

  File jpgFile = SD_MMC.open(imgPath, FILE_WRITE);
  if (!jpgFile) {
    Serial.println("SAVE ERRO: falha ao criar arquivo JPG no SD");
    imageCounter--;
    esp_camera_fb_return(fb);
    return;
  }

  size_t written = jpgFile.write(fb->buf, fb->len);
  jpgFile.close();
  unsigned long t2 = millis();

  if (written != fb->len) {
    Serial.println("SAVE ERRO: escrita incompleta no SD, removendo arquivo...");
    SD_MMC.remove(imgPath);
    imageCounter--;
    esp_camera_fb_return(fb);
    return;
  }

  // Log CSV
  File logFile = SD_MMC.open("/DCIM/LOG.CSV", FILE_APPEND);
  if (logFile) {
    unsigned long capture_ms = (t1 - t0);
    unsigned long save_ms    = (t2 - t1);
    unsigned long total_ms   = (t2 - t0);
    // CSV: index,arquivo,bytes,capture_ms,save_ms,total_ms,ok
    logFile.printf("%05u,%s,%u,%lu,%lu,%lu,OK\n",
                   imageCounter, imgPath + 6, (unsigned int)fb->len,
                   capture_ms, save_ms, total_ms);
    logFile.close();
  } else {
    Serial.println("AVISO: não conseguiu abrir /DCIM/LOG.CSV para append");
  }

  Serial.printf("SAVE OK: %s (%u bytes). capture=%lums save=%lums total=%lums\n",
                imgPath, (unsigned int)fb->len, (t1 - t0), (t2 - t1), (t2 - t0));

  // Libera buffer imediatamente
  esp_camera_fb_return(fb);

  // Mantemos o modo idle também em UXGA (qualidade IDLE_JPEG_QUALITY)
  s->set_quality(s, IDLE_JPEG_QUALITY);
}

// -------------------------
// SETUP
// -------------------------
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("Boot... inicializando camera UXGA...");

  if (!initCameraIdle()) {
    Serial.println("FALHA CRÍTICA: câmera não inicializou.");
    Serial.println("Possíveis causas:");
    Serial.println("- Build/board sem suporte a OV5640 (erro 0x106).");
    Serial.println("- Flat cable frouxo.");
    while (true) { delay(1000); }
  }

  Serial.println("Câmera OK. Agora montando SD...");
  if (!ensureSDMounted()) {
    Serial.println("AVISO: SD não montado ainda. Você pode mandar SAVE que eu tento montar de novo.");
  }

  Serial.printf("Pronto. Digite SAVE na Serial para tirar foto UXGA (Q%02d) e salvar no SD.\n",
                CAPTURE_JPEG_QUALITY);
}

// -------------------------
// LOOP
// -------------------------
void loop() {
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
