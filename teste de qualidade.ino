/**********************************************************************
  ESP32-S3 CAM + OV5640 — benchmark de captura e gravação no SD
  - Sem Wi-Fi
  - Comandos via Serial (115200):
      RUN   -> roda todos os testes de qualidade/tamanho
      SAVE  -> captura 1 foto com a 1ª config da lista
  - Para cada teste imprime:
      frame_size, jpeg_quality, tempo de captura, tempo total, bytes, caminho
**********************************************************************/

#include "esp_camera.h"
#include "SD_MMC.h"

// ====== usar mesmo pinout que já funcionou ======
#define CAMERA_MODEL_ESP32S3_EYE
#include "camera_pins.h"

// clock e buffers da câmera
static const int XCLK_HZ   = 24000000;  // 24 MHz estável pra OV5640
static int       FB_COUNT  = 2;         // 2 frame buffers na PSRAM

// ====== SD_MMC (modo 1-bit) ======
// tente A primeiro; se falhar montar, comente A e descomente B
#define SD_PRESET_A 1
// #define SD_PRESET_B 1

#if defined(SD_PRESET_A)
// Preset A (possível mapeamento da sua placa)
int SD_CLK = 39;
int SD_CMD = 38;
int SD_D0  = 40;
#elif defined(SD_PRESET_B)
// Preset B (alternativo)
int SD_CLK = 12;
int SD_CMD = 11;
int SD_D0  = 13;
#else
#error "Escolha SD_PRESET_A ou SD_PRESET_B no topo."
#endif

// freq SDMMC mais segura (evita erro 0x108)
static const int SD_FREQ = SDMMC_FREQ_DEFAULT;   // ~20 MHz

// ---------- Estrutura de teste ----------
// Vamos testar várias combinações de resolução e qualidade
// OBS: FRAMESIZE_* vêm de esp_camera.h
typedef struct {
  framesize_t frame_size;
  int         jpeg_quality;
  const char* name;
} capture_profile_t;

// você pode ajustar / adicionar perfis aqui:
capture_profile_t profiles[] = {
  { FRAMESIZE_SVGA, 14, "SVGA_q14" },   // 800x600, qualidade média
  { FRAMESIZE_SVGA, 10, "SVGA_q10" },   // mesma resolução, mais qualidade (arquivo maior)
  { FRAMESIZE_XGA,  14, "XGA_q14"  },   // 1024x768
  { FRAMESIZE_XGA,  10, "XGA_q10"  },
  { FRAMESIZE_UXGA, 14, "UXGA_q14" },   // 1600x1200 (pode pesar)
  { FRAMESIZE_UXGA, 10, "UXGA_q10" }
};
const int NUM_PROFILES = sizeof(profiles)/sizeof(profiles[0]);

// ---------- helpers ----------

bool mount_sdcard() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0); // 1-bit
  // mount_point="/sdcard", 1-bit=true, format_if_fail=false, freq=SD_FREQ
  if (!SD_MMC.begin("/sdcard", true, false, SD_FREQ)) {
    Serial.println("SD_MMC.begin falhou");
    return false;
  }
  uint64_t sizeMB = SD_MMC.cardSize() / (1024ULL*1024ULL);
  Serial.printf("SD montado: %llu MB (1-bit) CLK=%d CMD=%d D0=%d FREQ=%d\n",
                sizeMB, SD_CLK, SD_CMD, SD_D0, SD_FREQ);
  return true;
}

bool save_fb_to_sd(camera_fb_t *fb, String &outPath) {
  if (!fb || fb->len == 0) return false;

  SD_MMC.mkdir("/DCIM");

  outPath = "/DCIM/IMG_" + String((uint32_t)millis()) + ".jpg";
  File f = SD_MMC.open(outPath, FILE_WRITE);
  if (!f) return false;

  size_t written = f.write(fb->buf, fb->len);
  f.close();
  return (written == fb->len);
}

// muda config do sensor/câmera em runtime
bool apply_profile(framesize_t frame_size, int jpeg_quality) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s) {
    Serial.println("apply_profile: sensor NULL");
    return false;
  }

  // set resolução
  if (s->set_framesize(s, frame_size) != 0) {
    Serial.println("apply_profile: set_framesize falhou");
    return false;
  }

  // set qualidade JPEG
  if (s->set_quality) {
    s->set_quality(s, jpeg_quality); // alguns drivers usam set_quality
  } else {
    // fallback, se set_quality não existir (raro no S3 cam lib atual)
  }

  // pequenos ajustes padrão (igual antes)
  s->set_vflip(s, 1);
  s->set_brightness(s, 1);
  s->set_saturation(s, 0);

  // dar um pequeno tempo pro sensor ajustar
  delay(50);
  return true;
}

// roda UM teste e imprime métrica
void run_single_test(const capture_profile_t &prof) {
  if (!SD_MMC.cardType()) {
    Serial.println("ERRO: SD nao montado, cancelando teste");
    return;
  }

  Serial.printf("\n[TEST %s] frame=%d quality=%d\n",
                prof.name, prof.frame_size, prof.jpeg_quality);

  if (!apply_profile(prof.frame_size, prof.jpeg_quality)) {
    Serial.println("  apply_profile falhou -> SKIP");
    return;
  }

  uint32_t t0_cap = micros();
  camera_fb_t *fb = esp_camera_fb_get();
  uint32_t t1_cap = micros();

  if (!fb) {
    Serial.println("  ERRO: esp_camera_fb_get() falhou (NOFB)");
    return;
  }

  uint32_t capture_us = t1_cap - t0_cap;

  String path;
  uint32_t t0_save = micros();
  bool ok = save_fb_to_sd(fb, path);
  uint32_t t1_save = micros();

  uint32_t total_us = t1_save - t0_cap;
  uint32_t save_us  = t1_save - t0_save;
  uint32_t bytes    = fb->len;

  esp_camera_fb_return(fb);

  if (!ok) {
    Serial.println("  ERRO ao salvar no SD");
  } else {
    Serial.printf("  OK salvo: %s\n", path.c_str());
    Serial.printf("  tamanho: %u bytes\n", (unsigned)bytes);
    Serial.printf("  tempos: capture=%u us, save=%u us, total=%u us\n",
                  (unsigned)capture_us,
                  (unsigned)save_us,
                  (unsigned)total_us);
  }
}

// roda TODOS os testes
void run_all_tests() {
  Serial.println("\n===== BENCH START =====");
  for (int i = 0; i < NUM_PROFILES; i++) {
    run_single_test(profiles[i]);
    delay(200); // respiro entre testes
  }
  Serial.println("===== BENCH END =====\n");
}

// captura 1 foto usando o primeiro profile da lista (atalho do comando SAVE)
void run_one_capture() {
  if (!SD_MMC.cardType()) {
    Serial.println("ERRO: SD nao montado");
    return;
  }
  // Usa o profiles[0]
  const capture_profile_t &p = profiles[0];
  Serial.printf("\n[SAVE cmd] usando %s\n", p.name);

  if (!apply_profile(p.frame_size, p.jpeg_quality)) {
    Serial.println("  apply_profile falhou");
    return;
  }

  uint32_t t0_cap = micros();
  camera_fb_t *fb = esp_camera_fb_get();
  uint32_t t1_cap = micros();

  if (!fb) {
    Serial.println("  ERRO: NOFB");
    return;
  }

  uint32_t capture_us = t1_cap - t0_cap;

  String path;
  uint32_t t0_save = micros();
  bool ok = save_fb_to_sd(fb, path);
  uint32_t t1_save = micros();

  uint32_t total_us = t1_save - t0_cap;
  uint32_t save_us  = t1_save - t0_save;
  uint32_t bytes    = fb->len;

  esp_camera_fb_return(fb);

  if (!ok) {
    Serial.println("  ERRO salvando");
  } else {
    Serial.printf("  OK salvo: %s\n", path.c_str());
    Serial.printf("  tamanho: %u bytes\n", (unsigned)bytes);
    Serial.printf("  tempos: capture=%u us, save=%u us, total=%u us\n",
                  (unsigned)capture_us,
                  (unsigned)save_us,
                  (unsigned)total_us);
  }
}

// ---------- setup ----------
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("[BOOT] init camera e SD...");

  // configura câmera base
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;

  c.pin_d0 = Y2_GPIO_NUM;
  c.pin_d1 = Y3_GPIO_NUM;
  c.pin_d2 = Y4_GPIO_NUM;
  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;
  c.pin_d5 = Y7_GPIO_NUM;
  c.pin_d6 = Y8_GPIO_NUM;
  c.pin_d7 = Y9_GPIO_NUM;

  c.pin_xclk  = XCLK_HZ ? XCLK_GPIO_NUM : XCLK_GPIO_NUM; // só pra deixar claro
  c.pin_pclk  = PCLK_GPIO_NUM;
  c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href  = HREF_GPIO_NUM;

  c.pin_sccb_sda = SIOD_GPIO_NUM;
  c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn  = PWDN_GPIO_NUM;
  c.pin_reset = RESET_GPIO_NUM;

  c.xclk_freq_hz = XCLK_HZ;
  c.pixel_format = PIXFORMAT_JPEG;
  // frame_size/jpeg_quality iniciais não importam MUITO porque
  // vamos mudar via apply_profile() nos testes, mas precisamos dar algo válido:
  c.frame_size   = FRAMESIZE_SVGA;
  c.jpeg_quality = 14;
  c.fb_count     = FB_COUNT;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;

  esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("ERRO: esp_camera_init falhou 0x%x\n", err);
    while (true) {
      delay(2000);
    }
  }
  Serial.println("Camera OK.");

  // pequeno ajuste inicial
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, 1);
    s->set_brightness(s, 1);
    s->set_saturation(s, 0);
  }

  // monta SD
  if (!mount_sdcard()) {
    Serial.println("ATENCAO: SD NAO MONTADO. Troque SD_PRESET_A <-> SD_PRESET_B e regrave.");
  }

  Serial.println();
  Serial.println("Pronto.");
  Serial.println("Comandos Serial:");
  Serial.println("  RUN  -> roda benchmark em varios tamanhos/qualidades");
  Serial.println("  SAVE -> tira UMA foto usando o primeiro perfil");
}

// ---------- loop ----------
void loop() {
  static String cmd;

  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\n' || ch == '\r') {
      cmd.trim();
      if (cmd.length() > 0) {
        if (cmd.equalsIgnoreCase("RUN")) {
          run_all_tests();
        } else if (cmd.equalsIgnoreCase("SAVE")) {
          run_one_capture();
        } else {
          Serial.println("Comando invalido. Use RUN ou SAVE.");
        }
        cmd = "";
      }
    } else {
      cmd += ch;
    }
  }

  delay(10);
}
