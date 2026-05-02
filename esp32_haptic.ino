#include <FastLED.h>
#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SPI.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WebServer.h>
#include <jpeg_decoder.h>

// ESP-IDF Low-Level I2S
#include <driver/i2s_std.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>

// ESP8266Audio Libraries
#include "AudioFileSourceFS.h"
#include "AudioFileSourcePROGMEM.h"
#include "AudioFileSourceID3.h"
#include "AudioGeneratorMP3.h"
#include "AudioLogger.h"
#include "AudioOutput.h" // We MUST use Base and rewrite the driver completely for IDF v5

// -------------------- Pins --------------------
#define SPI_MOSI 11
#define SPI_SCLK 12
#define SPI_MISO 13
#define SD_CS    10

#define SDMMC_CLK_PIN SPI_SCLK
#define SDMMC_CMD_PIN SPI_MOSI
#define SDMMC_D0_PIN  SPI_MISO

#define I2S_BCLK 5
#define I2S_LRC  6
#define I2S_DOUT 7

// -------------------- ST7789 Display Pins (separate SPI host) --------------------
#define TFT_SCLK 18
#define TFT_MOSI 17
#define TFT_CS   16
#define TFT_DC   15
#define TFT_RST  4
#define TFT_BL   21
#define TFT_WIDTH 240
#define TFT_HEIGHT 280
#define TFT_X_OFFSET 0
#define TFT_Y_OFFSET 20
#define TFT_SPI_HZ 40000000

// -------------------- LED Pins ----------------
#define LED_PIN  40
#define NUM_LEDS 24

// -------------------- WiFi --------------------
const char* ssid = "IIIT-Guest";
const char* password = "f6s68VHJ89mC";

// -------------------- Globals --------------------
WebServer server(80);
Preferences prefs;

TaskHandle_t audioTaskHandle = NULL;
TaskHandle_t videoDecodeTaskHandle = NULL;
TaskHandle_t displayFlushTaskHandle = NULL;
TaskHandle_t webServerTaskHandle = NULL;
TaskHandle_t ledTaskHandle = NULL;

// -------------------- LED Globals -------------
CRGB leds[NUM_LEDS];
String ledMode = "off";
String ledColorHex = "#000000";
uint8_t ledBrightness = 100;
bool systemPowerOn = true;

uint8_t speakerVolumePct = 75; // Right channel (Speaker)
uint8_t exciterVolumePct = 50; // Left channel (Exciter)

bool loopEnabled = false;
String currentFilename = "";
bool isPaused = false;
volatile bool isUploading = false;
volatile bool uploadTasksSuspended = false;

enum MediaType { MEDIA_NONE, MEDIA_AUDIO, MEDIA_VIDEO };
MediaType currentMediaType = MEDIA_NONE;

enum AudioCommand {
    CMD_IDLE,
    CMD_PLAY,
    CMD_STOP,
    CMD_SEEK
};
struct AudioCommandMessage {
    AudioCommand cmd;
    char filename[192];
    bool loopRequested;
    uint32_t positionSec;
    uint32_t sessionId;
};

QueueHandle_t audioCmdQueue = NULL;
volatile bool audioAbortRequested = false;
volatile uint32_t mediaSessionId = 0;
volatile uint32_t activeSessionId = 0;
volatile bool stopRequested = false;

struct VideoPacket {
    uint32_t ptsMs;
    uint32_t dataLen;
    bool keyFrame;
    uint16_t color565;
    uint8_t frameIndex;
};

QueueHandle_t videoDisplayQueue = NULL;
QueueHandle_t videoFreeFrameQueue = NULL;
volatile bool videoPipelineActive = false;
volatile bool displayReady = false;
volatile uint32_t videoTargetFps = 12;
volatile uint32_t videoFramesDecoded = 0;
volatile uint32_t videoFramesDisplayed = 0;
volatile uint32_t videoFramesDropped = 0;
volatile uint32_t videoFramesSkipped = 0;
volatile int32_t videoAvDriftMs = 0;
volatile uint32_t videoStartMs = 0;
volatile bool videoUseFileStream = false;
volatile bool videoUseRawFrameStream = false;
volatile bool videoUseMjpegStream = false;
volatile bool videoUseHmjStream = false;
volatile bool videoStreamEof = false;
volatile uint32_t videoFrameCount = 0;
volatile uint32_t videoLastFramePtsMs = 0;
volatile uint32_t videoLastAudioClockMs = 0;
volatile uint32_t videoDecodeLastMs = 0;
volatile uint32_t videoDecodeAvgMs = 0;
volatile uint32_t videoDecodeMaxMs = 0;
volatile uint32_t videoDrawLastMs = 0;
volatile uint32_t videoDrawAvgMs = 0;
volatile uint32_t videoDrawMaxMs = 0;
uint32_t videoStreamHeaderSize = 0;

File videoStreamFile;
File videoRawFile;
File videoMjpegFile;
File videoHmjFile;
uint32_t videoRawHeaderSize = 0;
uint16_t videoRawWidth = TFT_WIDTH;
uint16_t videoRawHeight = TFT_HEIGHT;
uint16_t videoRawFps = 12;
size_t videoRawFrameSize = 0;
#define VIDEO_FRAME_BUFFER_COUNT 3
uint8_t *videoFrameBuffers[VIDEO_FRAME_BUFFER_COUNT] = {NULL, NULL, NULL};
uint8_t *videoMjpegFrameData = NULL;
size_t videoMjpegFrameCapacity = 0;
uint8_t *videoMjpegDecodeBuffer = NULL;
size_t videoMjpegDecodeBufferSize = 0;
volatile bool videoMjpegLastReadEof = false;
volatile bool videoHmjLastReadEof = false;
uint16_t videoHmjWidth = TFT_WIDTH;
uint16_t videoHmjHeight = TFT_HEIGHT;
uint16_t videoHmjFps = 12;
uint32_t videoHmjFrameCount = 0;
uint32_t videoHmjCurrentFrame = 0;
uint32_t videoHmjHeaderSize = 0;

#define MJPEG_MAX_FRAME_BYTES (300U * 1024U)
#define MJPEG_DECODE_MAX_PIXELS (320U * 320U)
#define MJPEG_READ_CHUNK_BYTES 8192
#define MJPEG_DEFAULT_FPS 12
#define HMJ_HEADER_BYTES 16
#define HMJ_FRAME_HEADER_BYTES 8
#define HMJ_MAGIC_0 'H'
#define HMJ_MAGIC_1 'M'
#define HMJ_MAGIC_2 'J'
#define HMJ_MAGIC_3 '1'
#define RGB_RAW_FIXED_FPS 15
#define MJPEG_SWAP_RGB565_BYTES 1
#define MJPEG_FLIP_X 0
#define MJPEG_FLIP_Y 0
#define MJPEG_ROTATE_90_CW 0
#define MJPEG_ROTATE_90_CCW 1
#define MJPEG_FILL_SCREEN_CROP 1
#define MJPEG_AUDIO_MAX_SKIP_FRAMES 30
#define MJPEG_DISPLAY_DROP_LAG_MS 200
#define AUDIO_CLOCK_MAX_STEP_MS 50
#define VIDEO_LAG_SKIP_THRESHOLD_MS 120
#define VIDEO_LAG_CRITICAL_MS 260

volatile bool videoCompanionAudioActive = false;
bool videoCompanionAudioLoop = false;
String videoCompanionAudioFile = "";
volatile uint32_t videoCompanionAudioStartMs = 0;
volatile bool videoSdReadInProgress = false;
uint8_t *videoCompanionMp3Data = NULL;
size_t videoCompanionMp3Size = 0;
volatile bool videoCompanionInRam = false;
uint32_t videoAudioClockMs = 0;
uint32_t videoAudioClockLastWallMs = 0;
bool videoAudioClockValid = false;
uint32_t videoSyncLogLastMs = 0;

#define VIDEO_COMPANION_MP3_MAX_BYTES (5U * 1024U * 1024U)

enum VideoStartState : uint8_t {
    VIDEO_START_IDLE = 0,
    VIDEO_START_OPENING_STREAM = 1,
    VIDEO_START_OPENING_AUDIO = 2,
    VIDEO_START_READY = 3,
    VIDEO_START_FAILED = 4,
};

volatile uint32_t videoSessionId = 0;
volatile uint8_t videoStart_state = VIDEO_START_IDLE;

static const BaseType_t CORE_VIDEO = 1;
static const BaseType_t CORE_SERVICE = 0;
static const UBaseType_t PRIO_WEB = 1;
static const UBaseType_t PRIO_LED = 2;
static const UBaseType_t PRIO_VIDEO = 5;
static const UBaseType_t PRIO_DISPLAY = 6;
static const UBaseType_t PRIO_AUDIO = 4;
static const TickType_t WEB_DELAY_NORMAL = pdMS_TO_TICKS(10);
static const TickType_t WEB_DELAY_VIDEO = pdMS_TO_TICKS(30);
static const TickType_t LED_DELAY_NORMAL = pdMS_TO_TICKS(30);
static const TickType_t LED_DELAY_VIDEO = pdMS_TO_TICKS(50);

SPIClass *tftSpi = NULL;
SemaphoreHandle_t tftMutex = NULL;

extern SemaphoreHandle_t sdMutex;

// -------------------- Custom Audio Output --------------------
// This cleanly splits a single I2S DIN wire into two independent volume-controlled channels
class AudioOutputHaptic : public AudioOutput {
private:
    int _bclk, _lrc, _dout;
    int _sampleRate = 44100;
    uint16_t _dmaDescNum = 12;
    uint16_t _dmaFrameNum = 1000;
    i2s_chan_handle_t tx_chan; // IDF v5 channel handle
    volatile uint64_t _framesRendered = 0;
public:
    float leftVol = 0.5f;
    float rightVol = 0.75f;
    
    AudioOutputHaptic(int bclk, int lrc, int dout) : AudioOutput() {
        _bclk = bclk;
        _lrc = lrc;
        _dout = dout;
        tx_chan = NULL;
    }

    virtual ~AudioOutputHaptic() {
        stop();
    }

    virtual bool SetRate(int hz) override {
        if (hz == _sampleRate) return true;
        _sampleRate = hz;
        if (tx_chan) {
            i2s_channel_disable(tx_chan);
            i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)_sampleRate);
            i2s_channel_reconfig_std_clock(tx_chan, &clk_cfg);
            i2s_channel_enable(tx_chan);
        }
        return true;
    }
    
    // Base class does not define SetBitsPerSample/SetChannels so we drop 'override' or omit
    bool SetBitsPerSample(int bits) { return true; } // Forced to 16-bit
    bool SetChannels(int channels) { return true; }  // Forced to Stereo DIN
    void resetClock() { _framesRendered = 0; }
    uint32_t clockMs() const {
        if (_sampleRate <= 0) return 0;
        return (uint32_t)((_framesRendered * 1000ULL) / (uint64_t)_sampleRate);
    }
    uint32_t bufferedMs() const {
        if (_sampleRate <= 0) return 0;
        uint64_t frames = (uint64_t)_dmaDescNum * (uint64_t)_dmaFrameNum;
        return (uint32_t)((frames * 1000ULL) / (uint64_t)_sampleRate);
    }

    virtual bool begin() override {
        // 1. Allocate an I2S TX channel (IDF v5)
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true; // Auto clear TX buffer
        chan_cfg.dma_desc_num = _dmaDescNum;    // Deep buffer for dropout prevention
        chan_cfg.dma_frame_num = _dmaFrameNum;  // ~270ms of audio buffer
        esp_err_t err = i2s_new_channel(&chan_cfg, &tx_chan, NULL);
        if (err != ESP_OK) {
            Serial.println("I2S channel allocation failed");
            return false;
        }

        // 2. Configure for Standard I2S Mode
        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)_sampleRate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = (gpio_num_t)_bclk,
                .ws = (gpio_num_t)_lrc,
                .dout = (gpio_num_t)_dout,
                .din = I2S_GPIO_UNUSED,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };

        err = i2s_channel_init_std_mode(tx_chan, &std_cfg);
        if (err != ESP_OK) {
            Serial.println("I2S std mode initialization failed");
            return false;
        }

        // 3. Enable the channel
        err = i2s_channel_enable(tx_chan);
        if (err != ESP_OK) {
            Serial.println("I2S channel enable failed");
            return false;
        }

        resetClock();

        return true;
    }

    virtual bool ConsumeSample(int16_t sample[2]) override {
        if (!tx_chan) return false;
        if (audioAbortRequested) return false;
        
        int16_t scaled[2];
        // Average L&R to ensure mono source, then scale independently
        int32_t mono = (sample[0] + sample[1]) / 2;
        
        // Apply independent volumes. Left=Exciter, Right=Speaker
        scaled[0] = max(-32768, min(32767, (int)(mono * leftVol)));
        scaled[1] = max(-32768, min(32767, (int)(mono * rightVol)));
        
        size_t bytes_written = 0;
        esp_err_t err = i2s_channel_write(tx_chan, &scaled, 4, &bytes_written, pdMS_TO_TICKS(20));
        if (err == ESP_OK && bytes_written >= 4) {
            _framesRendered += 1ULL;
        }
        return err == ESP_OK;
    }
    
    virtual uint16_t ConsumeSamples(int16_t *samples, uint16_t count) override {
        if (!tx_chan) return 0;
        if (audioAbortRequested) return 0;
        
        const uint16_t CHUNK = 128;
        int16_t scaled[CHUNK * 2];
        uint16_t processed = 0;
        
        while (processed < count) {
            if (audioAbortRequested) return processed;
            uint16_t toProcess = min((int)(count - processed), (int)CHUNK);
            for (uint16_t i = 0; i < toProcess; i++) {
                int16_t l = samples[(processed + i) * 2];
                int16_t r = samples[(processed + i) * 2 + 1];
                int32_t mono = (l + r) / 2;
                scaled[i * 2]     = max(-32768, min(32767, (int)(mono * leftVol)));
                scaled[i * 2 + 1] = max(-32768, min(32767, (int)(mono * rightVol)));
            }
            size_t bytes_written = 0;
            esp_err_t err = i2s_channel_write(tx_chan, scaled, toProcess * 4, &bytes_written, pdMS_TO_TICKS(20));
            if (err != ESP_OK) {
                return processed;
            }
            _framesRendered += ((uint64_t)bytes_written / 4ULL);
            processed += toProcess;
        }
        return count;
    }

    virtual bool stop() override {
        if (tx_chan) {
            i2s_channel_disable(tx_chan);
            i2s_del_channel(tx_chan);
            tx_chan = NULL;
        }
        return true;
    }
};

AudioFileSource *fileSource = NULL;
AudioFileSourceID3 *id3Source = NULL;
AudioGeneratorMP3 *mp3 = NULL;
AudioOutputHaptic *out = NULL;

// -------------------- Forward Declarations & IPC --------------------
void handleApiFiles();
void handleApiPlay();
void handleApiStop();
void handleApiPause();
void handleApiSeek();
void handleApiCurrent();
void handleApiVolumeSpeaker();
void handleApiVolumeExciter();
void handleApiLedMode();
void handleApiLedColor();
void handleApiLedBrightness();
void handleApiStatus();
void handleApiPowerToggle();
void handleApiBattery();
void handleApiMediaDelete();
void handleApiMediaUpload();
void handleApiMediaStorage();
void handleCorsOptions();
void taskUploadWorker(void *pvParameters);
void taskVideoDecode(void *pvParameters);
void taskDisplayFlush(void *pvParameters);
void initDisplayVideoScaffold();
bool initSt7789Panel();
void st7789WriteCommand(uint8_t command, const uint8_t *data, size_t dataLen);
void st7789SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);
void st7789FillColor(uint16_t color);
void st7789DrawFrameRGB565(const uint8_t *frameData, uint16_t width, uint16_t height);
bool _startVideoPlayback(const String& filename, bool loopRequested, uint32_t sessionId);
bool _openVideoColorStream(const String& filename);
bool _readNextVideoPacket(VideoPacket &packet);
void _closeVideoColorStream();
bool _openRawFrameStream(const String& filename);
bool _readRawFrameToBuffer(uint8_t frameIndex);
bool _skipRawFrames(uint32_t frameCount);
void _closeRawFrameStream();
bool _openMjpegStream(const String& filename);
bool _readMjpegFrameBytes(size_t *outFrameLen);
bool _readMjpegFrameToBuffer(uint8_t frameIndex);
bool _skipMjpegFrames(uint32_t frameCount);
void _closeMjpegStream();
bool _openHmjStream(const String& filename);
bool _readHmjFrameHeader(uint32_t *outPtsMs, uint32_t *outJpegLen);
bool _readHmjFramePayloadToBuffer(uint32_t jpegLen, uint8_t frameIndex);
bool _skipHmjFramePayload(uint32_t jpegLen);
bool _rewindHmjStream();
void _closeHmjStream();
void _stopAudioDecoderOnly();
void _tryStartVideoCompanionAudio(const String& videoFilename, bool loopRequested);
void updateAudioVolumeAndBalance();
bool _startAudioPlayback(const String& filename, bool loopRequested, uint32_t startByte = 0, bool updateMediaState = true, uint32_t sessionId = 0);
bool _seekAudioToSeconds(uint32_t positionSec);
bool _enqueueAudioCommand(const AudioCommandMessage &cmd, uint32_t timeoutMs = 50);
uint32_t _videoPlaybackClockMs();
bool _acquireVideoFrameBuffer(uint8_t *frameIndex, TickType_t waitTicks);
void _releaseVideoFrameBuffer(uint8_t frameIndex);
void _resetVideoFrameQueues();
bool _queueVideoPacket(VideoPacket &packet);
void _videoSdReadBegin();
void _videoSdReadEnd();
void _releaseCompanionAudioRam();
bool _loadFileToPsram(const String& filename, uint8_t **outData, size_t *outSize);
extern size_t uploadBytesWritten;
extern size_t uploadBytesReceived;
extern bool uploadHadWriteError;
extern String uploadErrorMessage;
extern String uploadPartialPath;
extern volatile bool uploadProcessingDone;

SemaphoreHandle_t sdMutex = NULL;
SemaphoreHandle_t audioMutex = NULL;

// -------------------- Loop (Core 1) --------------------
void _stopAudioSafely() {
    if (audioMutex) {
        xSemaphoreTake(audioMutex, portMAX_DELAY);
    }
    isPaused = true; // Stop play loop entry before deleting memory
    currentMediaType = MEDIA_NONE;

    if (sdMutex) {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
    }

    if (mp3) {
        if (mp3->isRunning()) mp3->stop();
        delete mp3;
        mp3 = NULL;
    }
    if (id3Source) {
        delete id3Source;
        id3Source = NULL;
    }
    if (fileSource) {
        if (fileSource->isOpen()) fileSource->close();
        delete fileSource;
        fileSource = NULL;
    }

    _closeVideoColorStream();
    _closeRawFrameStream();
    _closeMjpegStream();
    _closeHmjStream();
    videoUseFileStream = false;
    videoUseRawFrameStream = false;
    videoUseMjpegStream = false;
    videoUseHmjStream = false;
    videoStreamEof = false;
    videoFrameCount = 0;

    if (sdMutex) {
        xSemaphoreGive(sdMutex);
    }

    if (out) {
        out->resetClock();
    }

    _releaseCompanionAudioRam();

    currentFilename = "";
    loopEnabled = false;
    videoCompanionAudioActive = false;
    videoCompanionAudioLoop = false;
    videoCompanionAudioFile = "";
    videoCompanionAudioStartMs = 0;
    if (stopRequested) {
        activeSessionId = 0;
    }
    videoStart_state = VIDEO_START_IDLE;
    _resetVideoFrameQueues();
    if (audioMutex) {
        xSemaphoreGive(audioMutex);
    }
    isPaused = false;
}

void _stopAudioDecoderOnly() {
    if (audioMutex) {
        xSemaphoreTake(audioMutex, portMAX_DELAY);
    }
    if (sdMutex) {
        xSemaphoreTake(sdMutex, portMAX_DELAY);
    }

    if (mp3) {
        if (mp3->isRunning()) mp3->stop();
        delete mp3;
        mp3 = NULL;
    }
    if (id3Source) {
        delete id3Source;
        id3Source = NULL;
    }
    if (fileSource) {
        if (fileSource->isOpen()) fileSource->close();
        delete fileSource;
        fileSource = NULL;
    }

    if (sdMutex) {
        xSemaphoreGive(sdMutex);
    }

    if (out) {
        out->resetClock();
    }

    _releaseCompanionAudioRam();
    if (audioMutex) {
        xSemaphoreGive(audioMutex);
    }
}

void _releaseCompanionAudioRam() {
    if (videoCompanionMp3Data) {
        free(videoCompanionMp3Data);
        videoCompanionMp3Data = NULL;
    }
    videoCompanionMp3Size = 0;
    videoCompanionInRam = false;
    videoAudioClockMs = 0;
    videoAudioClockLastWallMs = 0;
    videoAudioClockValid = false;
}

bool _loadFileToPsram(const String& filename, uint8_t **outData, size_t *outSize) {
    if (!outData || !outSize || !sdMutex) return false;
    *outData = NULL;
    *outSize = 0;

    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(350))) {
        return false;
    }

    bool ok = false;
    do {
        File src = SD_MMC.open(filename, FILE_READ);
        if (!src) break;

        size_t sz = src.size();
        if (sz == 0 || sz > VIDEO_COMPANION_MP3_MAX_BYTES) {
            src.close();
            break;
        }

        uint8_t *buffer = (uint8_t*)ps_malloc(sz);
        if (!buffer) {
            src.close();
            break;
        }

        size_t offset = 0;
        while (offset < sz) {
            size_t toRead = min((size_t)2048, sz - offset);
            int n = src.read(buffer + offset, toRead);
            if (n <= 0) {
                break;
            }
            offset += (size_t)n;

            if ((offset & 0x7FFFU) == 0U) {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        }
        src.close();

        if (offset != sz) {
            free(buffer);
            break;
        }

        *outData = buffer;
        *outSize = sz;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _startAudioPlayback(const String& filename, bool loopRequested, uint32_t startByte, bool updateMediaState, uint32_t sessionId) {
    if (stopRequested || (sessionId != 0 && sessionId != mediaSessionId)) {
        return false;
    }

    if (updateMediaState) {
        _stopAudioSafely();
    } else {
        _stopAudioDecoderOnly();
    }

    if (audioMutex) {
        xSemaphoreTake(audioMutex, portMAX_DELAY);
    }

    if (!updateMediaState) {
        bool success = false;
        do {
            Serial.printf("Opening companion audio file (PSRAM): %s\n", filename.c_str());

            String fnameLower = filename;
            fnameLower.toLowerCase();
            if (!fnameLower.endsWith(".mp3")) {
                Serial.println("Unsupported companion audio format");
                break;
            }

            if (!_loadFileToPsram(filename, &videoCompanionMp3Data, &videoCompanionMp3Size)) {
                Serial.println("Failed to preload companion MP3 to PSRAM");
                break;
            }

            fileSource = new AudioFileSourcePROGMEM(videoCompanionMp3Data, (int32_t)videoCompanionMp3Size);
            if (!fileSource) {
                Serial.println("Failed to create PROGMEM source");
                break;
            }

            id3Source = new AudioFileSourceID3(fileSource);
            mp3 = new AudioGeneratorMP3();
            if (!id3Source || !mp3) {
                Serial.println("Failed to create companion decoder pipeline");
                break;
            }

            if (!mp3->begin(id3Source, out)) {
                Serial.println("Failed to start companion MP3 decoder");
                break;
            }

            if (out) {
                out->resetClock();
            }

            videoCompanionInRam = true;
            videoCompanionAudioActive = true;
            videoCompanionAudioLoop = loopRequested;
            videoCompanionAudioFile = filename;
            videoCompanionAudioStartMs = millis();
            activeSessionId = (sessionId != 0) ? sessionId : mediaSessionId;
            isPaused = false;
            success = true;
            Serial.println("Companion MP3 started from PSRAM");
        } while (false);

        if (!success) {
            if (mp3) { delete mp3; mp3 = NULL; }
            if (id3Source) { delete id3Source; id3Source = NULL; }
            if (fileSource) { delete fileSource; fileSource = NULL; }
            _releaseCompanionAudioRam();
            videoCompanionAudioActive = false;
            videoCompanionAudioLoop = false;
            videoCompanionAudioFile = "";
            videoCompanionAudioStartMs = 0;
            activeSessionId = 0;
            isPaused = false;
        }

        if (audioMutex) {
            xSemaphoreGive(audioMutex);
        }
        return success;
    }

    xSemaphoreTake(sdMutex, portMAX_DELAY);

    bool success = false;
    do {
        Serial.printf("Opening media file: %s\n", filename.c_str());

        String fnameLower = filename;
        fnameLower.toLowerCase();
        if (!fnameLower.endsWith(".mp3")) {
            Serial.println("Unsupported media for Stage 1: " + filename);
            break;
        }

        fileSource = new AudioFileSourceFS(SD_MMC, filename.c_str());
        if (!fileSource || !fileSource->isOpen()) {
            Serial.println("Failed to open media file");
            break;
        }

        if (startByte > 0) {
            uint32_t fileSize = fileSource->getSize();
            uint32_t clamped = startByte;
            if (fileSize > 0 && clamped >= fileSize) {
                clamped = fileSize - 1;
            }
            fileSource->seek(clamped, SEEK_SET);
        }

        id3Source = new AudioFileSourceID3(fileSource);

        mp3 = new AudioGeneratorMP3();
        if (!id3Source || !mp3) {
            Serial.println("Failed to create decoder pipeline");
            break;
        }

        Serial.println("Starting decoder begin()...");
        if (!mp3->begin(id3Source, out)) {
            Serial.println("Failed to start MP3 decoder");
            break;
        }

        if (out) {
            out->resetClock();
        }

        if (updateMediaState) {
            currentMediaType = MEDIA_AUDIO;
            currentFilename = filename;
            loopEnabled = loopRequested;
            activeSessionId = (sessionId != 0) ? sessionId : mediaSessionId;
        } else {
            videoCompanionAudioActive = true;
            videoCompanionAudioLoop = loopRequested;
            videoCompanionAudioFile = filename;
            videoCompanionAudioStartMs = millis();
            activeSessionId = (sessionId != 0) ? sessionId : mediaSessionId;
        }
        isPaused = false;
        Serial.println("Starting MP3 Audio Decoder...");
        success = true;
    } while (false);

    if (!success) {
        if (mp3) { delete mp3; mp3 = NULL; }
        if (id3Source) { delete id3Source; id3Source = NULL; }
        if (fileSource) {
            if (fileSource->isOpen()) fileSource->close();
            delete fileSource;
            fileSource = NULL;
        }
        if (updateMediaState) {
            currentMediaType = MEDIA_NONE;
            currentFilename = "";
            loopEnabled = false;
            activeSessionId = 0;
        } else {
            videoCompanionAudioActive = false;
            videoCompanionAudioLoop = false;
            videoCompanionAudioFile = "";
            videoCompanionAudioStartMs = 0;
            activeSessionId = 0;
        }
        isPaused = false;
    }

    xSemaphoreGive(sdMutex);
    if (audioMutex) {
        xSemaphoreGive(audioMutex);
    }
    return success;
}

bool _seekAudioToSeconds(uint32_t positionSec) {
    if (currentMediaType != MEDIA_AUDIO || currentFilename.length() == 0) {
        return false;
    }

    const uint32_t assumedBitrateKbps = 128;
    uint64_t targetByte = (uint64_t)positionSec * assumedBitrateKbps * 1000ULL / 8ULL;
    return _startAudioPlayback(currentFilename, loopEnabled, (uint32_t)targetByte, true, activeSessionId);
}

bool _enqueueAudioCommand(const AudioCommandMessage &cmd, uint32_t timeoutMs) {
    if (!audioCmdQueue) return false;
    return xQueueSend(audioCmdQueue, &cmd, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

uint32_t _videoPlaybackClockMs() {
    if (videoCompanionAudioActive) {
        uint32_t now = millis();
        if (out) {
            uint32_t audioMs = out->clockMs();
            uint32_t latencyMs = out->bufferedMs();
            uint32_t adjustedMs = (audioMs > latencyMs) ? (audioMs - latencyMs) : 0;
            if (adjustedMs > 0) {
                if (!videoAudioClockValid || adjustedMs >= videoAudioClockMs) {
                    videoAudioClockMs = adjustedMs;
                }
                videoAudioClockLastWallMs = now;
                videoAudioClockValid = true;
                return videoAudioClockMs;
            }
        }
        if (videoAudioClockValid) {
            uint32_t delta = now - videoAudioClockLastWallMs;
            if (delta > AUDIO_CLOCK_MAX_STEP_MS) delta = AUDIO_CLOCK_MAX_STEP_MS;
            videoAudioClockLastWallMs = now;
            videoAudioClockMs += delta;
            return videoAudioClockMs;
        }
        if (videoCompanionAudioStartMs != 0) {
            return now - videoCompanionAudioStartMs;
        }
    }
    return millis() - videoStartMs;
}

static uint16_t _readLe16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t _readLe32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool _readFileExact(File &file, uint8_t *buffer, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        int n = file.read(buffer + offset, len - offset);
        if (n <= 0) {
            return false;
        }
        offset += (size_t)n;
    }
    return true;
}

static void _recordMovingAverage(volatile uint32_t &lastMs, volatile uint32_t &avgMs, volatile uint32_t &maxMs, uint32_t sampleMs) {
    lastMs = sampleMs;
    if (sampleMs > maxMs) {
        maxMs = sampleMs;
    }
    avgMs = (avgMs == 0) ? sampleMs : ((avgMs * 7U) + sampleMs) / 8U;
}

static bool _packetUsesFrameBuffer(const VideoPacket &packet) {
    return (packet.frameIndex < VIDEO_FRAME_BUFFER_COUNT) && videoFrameBuffers[packet.frameIndex] &&
           (videoUseRawFrameStream || videoUseMjpegStream || videoUseHmjStream);
}

bool _acquireVideoFrameBuffer(uint8_t *frameIndex, TickType_t waitTicks) {
    if (!frameIndex || !videoFreeFrameQueue) return false;
    return xQueueReceive(videoFreeFrameQueue, frameIndex, waitTicks) == pdTRUE;
}

void _releaseVideoFrameBuffer(uint8_t frameIndex) {
    if (!videoFreeFrameQueue || frameIndex >= VIDEO_FRAME_BUFFER_COUNT || !videoFrameBuffers[frameIndex]) return;
    xQueueSend(videoFreeFrameQueue, &frameIndex, 0);
}

void _resetVideoFrameQueues() {
    if (videoDisplayQueue) {
        VideoPacket packet;
        while (xQueueReceive(videoDisplayQueue, &packet, 0) == pdTRUE) {
            // Drain display queue to avoid resetting while another task may be blocked.
        }
    }
    if (!videoFreeFrameQueue) return;
    xQueueReset(videoFreeFrameQueue);
    for (uint8_t i = 0; i < VIDEO_FRAME_BUFFER_COUNT; i++) {
        if (videoFrameBuffers[i]) {
            xQueueSend(videoFreeFrameQueue, &i, 0);
        }
    }
}

bool _queueVideoPacket(VideoPacket &packet) {
    if (videoDisplayQueue && xQueueSend(videoDisplayQueue, &packet, 0) == pdTRUE) {
        return true;
    }
    if (_packetUsesFrameBuffer(packet)) {
        _releaseVideoFrameBuffer(packet.frameIndex);
    }
    videoFramesDropped++;
    return false;
}

void _videoSdReadBegin() {
    videoSdReadInProgress = true;
}

void _videoSdReadEnd() {
    videoSdReadInProgress = false;
}

void _setUploadTaskSuspension(bool suspendTasks) {
    if (suspendTasks) {
        if (uploadTasksSuspended) return;
        if (videoDecodeTaskHandle) vTaskSuspend(videoDecodeTaskHandle);
        if (displayFlushTaskHandle) vTaskSuspend(displayFlushTaskHandle);
        if (ledTaskHandle) vTaskSuspend(ledTaskHandle);
        uploadTasksSuspended = true;
    } else {
        if (!uploadTasksSuspended) return;
        if (videoDecodeTaskHandle) vTaskResume(videoDecodeTaskHandle);
        if (displayFlushTaskHandle) vTaskResume(displayFlushTaskHandle);
        if (ledTaskHandle) vTaskResume(ledTaskHandle);
        uploadTasksSuspended = false;
    }
}

// -------------------- Tasks --------------------
void taskWebServer(void *pvParameters) {
    while (true) {
        server.handleClient();
        TickType_t delayTicks = WEB_DELAY_NORMAL;
        if (currentMediaType == MEDIA_VIDEO && !isUploading) {
            delayTicks = WEB_DELAY_VIDEO;
        }
        vTaskDelay(delayTicks);
    }
}

void taskAudio(void *pvParameters) {
    uint32_t lastVideoAudioPumpMs = 0;
    while (true) {
        if (audioAbortRequested) {
            _stopAudioSafely();
            audioAbortRequested = false;
        }

        AudioCommandMessage cmdMsg;
        while (xQueueReceive(audioCmdQueue, &cmdMsg, 0) == pdTRUE) {
            if (cmdMsg.sessionId != 0 && cmdMsg.sessionId != mediaSessionId) {
                continue;
            }

            if (cmdMsg.cmd == CMD_PLAY) {
                audioAbortRequested = false;
                stopRequested = false;
                _startAudioPlayback(String(cmdMsg.filename), cmdMsg.loopRequested, 0, true, cmdMsg.sessionId);
            } else if (cmdMsg.cmd == CMD_STOP) {
                stopRequested = true;
                audioAbortRequested = true;
                _stopAudioSafely();
                activeSessionId = 0;
                audioAbortRequested = false;
            } else if (cmdMsg.cmd == CMD_SEEK) {
                audioAbortRequested = false;
                _seekAudioToSeconds(cmdMsg.positionSec);
            }
        }

        if (isUploading) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        // Audio Playback Pump safely wrapped in sdMutex to prevent SPI collisions
        bool playing = false;
        bool hasMp3 = false;
        bool isCompanionDuringVideo = (currentMediaType == MEDIA_VIDEO && videoCompanionAudioActive);
        if (!isPaused) {
            if (audioMutex) {
                if (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                    if (mp3 && mp3->isRunning()) {
                        hasMp3 = true;
                        if (isCompanionDuringVideo && videoCompanionInRam) {
                            uint32_t now = millis();
                            if ((now - lastVideoAudioPumpMs) < 3U) {
                                playing = true;
                            } else if (mp3) {
                                playing = mp3->loop();
                                lastVideoAudioPumpMs = now;
                            } else {
                                playing = true;
                            }
                        } else if (isCompanionDuringVideo) {
                            if (videoSdReadInProgress) {
                                playing = true;
                            } else if (mp3 && xSemaphoreTake(sdMutex, 0)) {
                                playing = mp3->loop();
                                xSemaphoreGive(sdMutex);
                            } else {
                                playing = true;
                            }
                        } else if (mp3 && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10))) {
                            playing = mp3->loop();
                            xSemaphoreGive(sdMutex);
                        } else {
                            playing = true;
                        }
                    }
                    xSemaphoreGive(audioMutex);
                } else {
                    playing = true;
                }
            } else if (mp3 && mp3->isRunning()) {
                hasMp3 = true;
                if (isCompanionDuringVideo && videoCompanionInRam) {
                    uint32_t now = millis();
                    if ((now - lastVideoAudioPumpMs) < 3U) {
                        playing = true;
                    } else if (mp3) {
                        playing = mp3->loop();
                        lastVideoAudioPumpMs = now;
                    } else {
                        playing = true;
                    }
                } else if (isCompanionDuringVideo) {
                    if (videoSdReadInProgress) {
                        playing = true;
                    } else if (mp3 && xSemaphoreTake(sdMutex, 0)) {
                        playing = mp3->loop();
                        xSemaphoreGive(sdMutex);
                    } else {
                        playing = true;
                    }
                } else if (mp3 && xSemaphoreTake(sdMutex, pdMS_TO_TICKS(10))) {
                    playing = mp3->loop();
                    xSemaphoreGive(sdMutex);
                } else {
                    playing = true;
                }
            }
        }

        if (hasMp3 && !playing) {
            if (currentMediaType == MEDIA_AUDIO) {
                String finishedTrack = currentFilename;
                bool shouldLoop = loopEnabled;
                uint32_t sessionAtEnd = activeSessionId;
                _stopAudioSafely();

                if (shouldLoop && finishedTrack.length() > 0 && !stopRequested && sessionAtEnd != 0 && sessionAtEnd == mediaSessionId) {
                    Serial.println("Restarting loop...");
                    _startAudioPlayback(finishedTrack, true, 0, true, sessionAtEnd);
                } else {
                    Serial.println("Playback finished naturally");
                }
            } else if (videoCompanionAudioActive) {
                String finishedTrack = videoCompanionAudioFile;
                bool shouldLoop = videoCompanionAudioLoop;
                uint32_t sessionAtEnd = activeSessionId;
                _stopAudioDecoderOnly();

                if (shouldLoop && finishedTrack.length() > 0 && !stopRequested && sessionAtEnd != 0 && sessionAtEnd == mediaSessionId) {
                    _startAudioPlayback(finishedTrack, true, 0, false, sessionAtEnd);
                } else {
                    videoCompanionAudioActive = false;
                    videoCompanionAudioLoop = false;
                    videoCompanionAudioFile = "";
                }
            }
        }
        TickType_t audioDelay = (currentMediaType == MEDIA_VIDEO && videoCompanionAudioActive) ? pdMS_TO_TICKS(5) : pdMS_TO_TICKS(2);
        vTaskDelay(audioDelay);
    }
}

void taskLEDs(void *pvParameters) {
    uint8_t hue = 0;
    while (true) {
        if (isUploading) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        if (!systemPowerOn) {
            FastLED.clear();
            FastLED.show();
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        
        if (ledMode == "off") {
            FastLED.clear();
            FastLED.show();
        } else if (ledMode == "manual") {
            long number = strtol(&ledColorHex[1], NULL, 16);
            int r = number >> 16;
            int g = number >> 8 & 0xFF;
            int b = number & 0xFF;
            fill_solid(leds, NUM_LEDS, CRGB(r, g, b));
            FastLED.show();
        } else if (ledMode == "random") {
            fill_rainbow(leds, NUM_LEDS, hue, 255/NUM_LEDS);
            FastLED.show();
            hue++;
        } else if (ledMode == "frequency" || ledMode == "oled_sync") {
             // Placeholder for audio visualizer sync
             fill_solid(leds, NUM_LEDS, CRGB::Blue);
             FastLED.show();
        }
        TickType_t ledDelay = (currentMediaType == MEDIA_VIDEO) ? LED_DELAY_VIDEO : LED_DELAY_NORMAL;
        vTaskDelay(ledDelay);
    }
}

void initDisplayVideoScaffold() {
    pinMode(TFT_CS, OUTPUT);
    pinMode(TFT_DC, OUTPUT);
    pinMode(TFT_RST, OUTPUT);
    pinMode(TFT_BL, OUTPUT);

    digitalWrite(TFT_CS, HIGH);
    digitalWrite(TFT_DC, HIGH);
    digitalWrite(TFT_RST, HIGH);
    digitalWrite(TFT_BL, HIGH);

    tftMutex = xSemaphoreCreateMutex();
    tftSpi = new SPIClass(HSPI);
    if (tftSpi) {
        tftSpi->begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
        displayReady = initSt7789Panel();
    }

    videoRawFrameSize = (size_t)TFT_WIDTH * (size_t)TFT_HEIGHT * 2U;
    bool frameBuffersOk = true;
    for (uint8_t i = 0; i < VIDEO_FRAME_BUFFER_COUNT; i++) {
        videoFrameBuffers[i] = (uint8_t*)ps_malloc(videoRawFrameSize);
        if (!videoFrameBuffers[i]) {
            frameBuffersOk = false;
        }
    }
    if (!frameBuffersOk) {
        for (uint8_t i = 0; i < VIDEO_FRAME_BUFFER_COUNT; i++) {
            if (videoFrameBuffers[i]) {
                free(videoFrameBuffers[i]);
                videoFrameBuffers[i] = NULL;
            }
        }
        Serial.println("WARN: Raw frame buffers unavailable; .v16 playback disabled.");
    }

    videoMjpegFrameCapacity = MJPEG_MAX_FRAME_BYTES;
    videoMjpegFrameData = (uint8_t*)ps_malloc(videoMjpegFrameCapacity);
    videoMjpegDecodeBufferSize = max(videoRawFrameSize, (size_t)MJPEG_DECODE_MAX_PIXELS * 2U);
    videoMjpegDecodeBuffer = (uint8_t*)ps_malloc(videoMjpegDecodeBufferSize);
    if (!videoMjpegFrameData || !videoMjpegDecodeBuffer) {
        if (videoMjpegFrameData) free(videoMjpegFrameData);
        if (videoMjpegDecodeBuffer) free(videoMjpegDecodeBuffer);
        videoMjpegFrameData = NULL;
        videoMjpegDecodeBuffer = NULL;
        videoMjpegFrameCapacity = 0;
        videoMjpegDecodeBufferSize = 0;
        Serial.println("WARN: MJPEG buffers unavailable; .mjpg/.mjpeg playback disabled.");
    }

    videoDisplayQueue = xQueueCreate(VIDEO_FRAME_BUFFER_COUNT, sizeof(VideoPacket));
    videoFreeFrameQueue = xQueueCreate(VIDEO_FRAME_BUFFER_COUNT, sizeof(uint8_t));

    if (!videoDisplayQueue || !videoFreeFrameQueue) {
        Serial.println("WARN: Video queues allocation failed; video pipeline disabled.");
        videoPipelineActive = false;
        return;
    }

    _resetVideoFrameQueues();
    videoPipelineActive = true;
    if (displayReady) {
        st7789FillColor(0xF800);
        delay(120);
        st7789FillColor(0x07E0);
        delay(120);
        st7789FillColor(0x001F);
        delay(120);
        st7789FillColor(0x0000);
        Serial.println("ST7789 panel init OK (RGB test shown)");
    } else {
        Serial.println("WARN: ST7789 panel init failed");
    }
    Serial.println("Video scaffold initialized (decode/display active).");
}

void st7789WriteCommand(uint8_t command, const uint8_t *data, size_t dataLen) {
    if (!tftSpi) return;
    if (tftMutex && xSemaphoreTake(tftMutex, pdMS_TO_TICKS(20)) != pdTRUE) return;

    tftSpi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);
    digitalWrite(TFT_DC, LOW);
    tftSpi->transfer(command);
    if (data && dataLen > 0) {
        digitalWrite(TFT_DC, HIGH);
        tftSpi->writeBytes(data, (uint32_t)dataLen);
    }
    digitalWrite(TFT_CS, HIGH);
    tftSpi->endTransaction();

    if (tftMutex) xSemaphoreGive(tftMutex);
}

void st7789SetAddressWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    x0 += TFT_X_OFFSET;
    x1 += TFT_X_OFFSET;
    y0 += TFT_Y_OFFSET;
    y1 += TFT_Y_OFFSET;

    uint8_t colData[4] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)
    };
    uint8_t rowData[4] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)
    };

    st7789WriteCommand(0x2A, colData, sizeof(colData));
    st7789WriteCommand(0x2B, rowData, sizeof(rowData));
    st7789WriteCommand(0x2C, NULL, 0);
}

void st7789FillColor(uint16_t color) {
    if (!tftSpi || !displayReady) return;
    if (tftMutex && xSemaphoreTake(tftMutex, pdMS_TO_TICKS(30)) != pdTRUE) return;

    uint8_t lineBuffer[TFT_WIDTH * 2];
    for (int x = 0; x < TFT_WIDTH; x++) {
        lineBuffer[(x * 2)] = (uint8_t)(color >> 8);
        lineBuffer[(x * 2) + 1] = (uint8_t)(color & 0xFF);
    }

    tftSpi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);

    uint16_t x0 = TFT_X_OFFSET;
    uint16_t x1 = TFT_X_OFFSET + TFT_WIDTH - 1;
    uint16_t y0 = TFT_Y_OFFSET;
    uint16_t y1 = TFT_Y_OFFSET + TFT_HEIGHT - 1;

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2A);
    digitalWrite(TFT_DC, HIGH);
    tftSpi->transfer((uint8_t)(x0 >> 8));
    tftSpi->transfer((uint8_t)(x0 & 0xFF));
    tftSpi->transfer((uint8_t)(x1 >> 8));
    tftSpi->transfer((uint8_t)(x1 & 0xFF));

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2B);
    digitalWrite(TFT_DC, HIGH);
    tftSpi->transfer((uint8_t)(y0 >> 8));
    tftSpi->transfer((uint8_t)(y0 & 0xFF));
    tftSpi->transfer((uint8_t)(y1 >> 8));
    tftSpi->transfer((uint8_t)(y1 & 0xFF));

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2C);
    digitalWrite(TFT_DC, HIGH);

    for (int y = 0; y < TFT_HEIGHT; y++) {
        tftSpi->writeBytes(lineBuffer, (uint32_t)sizeof(lineBuffer));
    }

    digitalWrite(TFT_CS, HIGH);
    tftSpi->endTransaction();

    if (tftMutex) xSemaphoreGive(tftMutex);
}

void st7789DrawFrameRGB565(const uint8_t *frameData, uint16_t width, uint16_t height) {
    if (!tftSpi || !displayReady || !frameData) return;
    if (width != TFT_WIDTH || height != TFT_HEIGHT) return;
    if (tftMutex && xSemaphoreTake(tftMutex, pdMS_TO_TICKS(40)) != pdTRUE) return;

    tftSpi->beginTransaction(SPISettings(TFT_SPI_HZ, MSBFIRST, SPI_MODE0));
    digitalWrite(TFT_CS, LOW);

    uint16_t x0 = TFT_X_OFFSET;
    uint16_t x1 = TFT_X_OFFSET + width - 1;
    uint16_t y0 = TFT_Y_OFFSET;
    uint16_t y1 = TFT_Y_OFFSET + height - 1;

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2A);
    digitalWrite(TFT_DC, HIGH);
    tftSpi->transfer((uint8_t)(x0 >> 8));
    tftSpi->transfer((uint8_t)(x0 & 0xFF));
    tftSpi->transfer((uint8_t)(x1 >> 8));
    tftSpi->transfer((uint8_t)(x1 & 0xFF));

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2B);
    digitalWrite(TFT_DC, HIGH);
    tftSpi->transfer((uint8_t)(y0 >> 8));
    tftSpi->transfer((uint8_t)(y0 & 0xFF));
    tftSpi->transfer((uint8_t)(y1 >> 8));
    tftSpi->transfer((uint8_t)(y1 & 0xFF));

    digitalWrite(TFT_DC, LOW); tftSpi->transfer(0x2C);
    digitalWrite(TFT_DC, HIGH);

    size_t totalBytes = (size_t)width * (size_t)height * 2U;
    const uint8_t *ptr = frameData;
    while (totalBytes > 0) {
        uint32_t chunk = (totalBytes > 4096U) ? 4096U : (uint32_t)totalBytes;
        tftSpi->writeBytes(ptr, chunk);
        ptr += chunk;
        totalBytes -= chunk;
    }

    digitalWrite(TFT_CS, HIGH);
    tftSpi->endTransaction();

    if (tftMutex) xSemaphoreGive(tftMutex);
}

bool initSt7789Panel() {
    if (!tftSpi) return false;

    digitalWrite(TFT_RST, LOW);
    delay(20);
    digitalWrite(TFT_RST, HIGH);
    delay(120);

    st7789WriteCommand(0x01, NULL, 0); // SWRESET
    delay(120);
    st7789WriteCommand(0x11, NULL, 0); // SLPOUT
    delay(120);

    uint8_t colmod = 0x55; // RGB565
    st7789WriteCommand(0x3A, &colmod, 1);

    uint8_t madctl = 0x00;
    st7789WriteCommand(0x36, &madctl, 1);

    uint8_t porch[5] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    st7789WriteCommand(0xB2, porch, sizeof(porch));

    uint8_t gate = 0x35;
    st7789WriteCommand(0xB7, &gate, 1);

    uint8_t vcom = 0x19;
    st7789WriteCommand(0xBB, &vcom, 1);

    uint8_t lcm = 0x2C;
    st7789WriteCommand(0xC0, &lcm, 1);

    uint8_t vdvVrh[2] = {0x01, 0xFF};
    st7789WriteCommand(0xC2, vdvVrh, sizeof(vdvVrh));

    uint8_t vrhs = 0x12;
    st7789WriteCommand(0xC3, &vrhs, 1);

    uint8_t vdvs = 0x20;
    st7789WriteCommand(0xC4, &vdvs, 1);

    uint8_t frctrl2 = 0x0F;
    st7789WriteCommand(0xC6, &frctrl2, 1);

    uint8_t pwctrl1 = 0xA4;
    st7789WriteCommand(0xD0, &pwctrl1, 1);

    st7789WriteCommand(0x21, NULL, 0); // INVON
    st7789WriteCommand(0x13, NULL, 0); // NORON
    st7789WriteCommand(0x29, NULL, 0); // DISPON
    delay(20);

    digitalWrite(TFT_BL, HIGH);
    return true;
}

bool _startVideoPlayback(const String& filename, bool loopRequested, uint32_t sessionId) {
    if (stopRequested || sessionId != mediaSessionId) {
        return false;
    }

    _stopAudioSafely();

    videoStart_state = VIDEO_START_OPENING_STREAM;
    videoSessionId++;

    videoFramesDecoded = 0;
    videoFramesDisplayed = 0;
    videoFramesDropped = 0;
    videoFramesSkipped = 0;
    videoAvDriftMs = 0;
    videoFrameCount = 0;
    videoLastFramePtsMs = 0;
    videoLastAudioClockMs = 0;
    videoDecodeLastMs = 0;
    videoDecodeAvgMs = 0;
    videoDecodeMaxMs = 0;
    videoDrawLastMs = 0;
    videoDrawAvgMs = 0;
    videoDrawMaxMs = 0;
    videoStartMs = millis();
    videoUseFileStream = false;
    videoUseRawFrameStream = false;
    videoUseMjpegStream = false;
    videoUseHmjStream = false;
    videoStreamEof = false;

    String lower = filename;
    lower.toLowerCase();
    if (lower.endsWith(".v16") || lower.endsWith(".rgb")) {
        videoUseRawFrameStream = _openRawFrameStream(filename);
        if (!videoUseRawFrameStream) {
            Serial.println("Video raw stream open failed.");
            videoStart_state = VIDEO_START_FAILED;
            return false;
        }
    } else if (lower.endsWith(".hmj")) {
        videoUseHmjStream = _openHmjStream(filename);
        if (!videoUseHmjStream) {
            Serial.println("Video HMJ stream open failed.");
            videoStart_state = VIDEO_START_FAILED;
            return false;
        }
    } else if (lower.endsWith(".mjpg") || lower.endsWith(".mjpeg")) {
        videoUseMjpegStream = _openMjpegStream(filename);
        if (!videoUseMjpegStream) {
            Serial.println("Video MJPEG stream open failed.");
            videoStart_state = VIDEO_START_FAILED;
            return false;
        }
    } else if (lower.endsWith(".vid")) {
        videoUseFileStream = _openVideoColorStream(filename);
        if (!videoUseFileStream) {
            Serial.println("Video .vid stream open failed.");
            videoStart_state = VIDEO_START_FAILED;
            return false;
        }
    } else {
        Serial.println("Unsupported video extension.");
        videoStart_state = VIDEO_START_FAILED;
        return false;
    }

    _resetVideoFrameQueues();

    if (stopRequested || sessionId != mediaSessionId) {
        _closeVideoColorStream();
        _closeRawFrameStream();
        _closeMjpegStream();
        _closeHmjStream();
        videoUseFileStream = false;
        videoUseRawFrameStream = false;
        videoUseMjpegStream = false;
        videoUseHmjStream = false;
        videoStart_state = VIDEO_START_FAILED;
        return false;
    }

    currentFilename = filename;
    loopEnabled = loopRequested;
    isPaused = false;
    activeSessionId = sessionId;

    videoStart_state = VIDEO_START_OPENING_AUDIO;
    _tryStartVideoCompanionAudio(filename, loopRequested);
    if (videoCompanionAudioActive && videoCompanionAudioStartMs != 0) {
        videoStartMs = videoCompanionAudioStartMs;
    } else {
        videoStartMs = millis();
    }

    currentMediaType = MEDIA_VIDEO;
    stopRequested = false;
    videoStart_state = VIDEO_START_READY;

    const char* modeStr = "synthetic";
    if (videoUseRawFrameStream) modeStr = "raw-frame";
    else if (videoUseHmjStream) modeStr = "hmj";
    else if (videoUseMjpegStream) modeStr = "mjpeg";
    else if (videoUseFileStream) modeStr = "color-stream";
    Serial.printf("Video playback started (%s).\n", modeStr);
    return true;
}

bool _openVideoColorStream(const String& filename) {
    if (!sdMutex) return false;

    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300))) {
        return false;
    }

    bool ok = false;
    do {
        if (videoStreamFile) {
            videoStreamFile.close();
        }

        videoStreamFile = SD_MMC.open(filename, FILE_READ);
        if (!videoStreamFile) {
            break;
        }

        uint8_t magic[4] = {0};
        if (videoStreamFile.read(magic, sizeof(magic)) != (int)sizeof(magic)) {
            videoStreamFile.close();
            break;
        }

        if (!(magic[0] == 'V' && magic[1] == 'C' && magic[2] == 'L' && magic[3] == 'R')) {
            videoStreamFile.close();
            break;
        }

        videoStreamHeaderSize = 4;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _readNextVideoPacket(VideoPacket &packet) {
    if (!sdMutex) return false;
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100))) {
        _videoSdReadEnd();
        return false;
    }

    if (!videoStreamFile) {
        xSemaphoreGive(sdMutex);
        _videoSdReadEnd();
        return false;
    }

    uint8_t buffer[6] = {0};
    int readLen = videoStreamFile.read(buffer, sizeof(buffer));
    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();

    if (readLen != (int)sizeof(buffer)) {
        return false;
    }

    packet.ptsMs = (uint32_t)buffer[0] |
                   ((uint32_t)buffer[1] << 8) |
                   ((uint32_t)buffer[2] << 16) |
                   ((uint32_t)buffer[3] << 24);
    packet.color565 = (uint16_t)buffer[4] | ((uint16_t)buffer[5] << 8);
    packet.dataLen = videoFramesDecoded;
    packet.keyFrame = ((videoFramesDecoded % 30U) == 0U);
    return true;
}

void _closeVideoColorStream() {
    if (videoStreamFile) {
        videoStreamFile.close();
    }
}

bool _openRawFrameStream(const String& filename) {
    if (!sdMutex || !videoFrameBuffers[0] || !videoFrameBuffers[1]) return false;
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300))) return false;

    bool ok = false;
    do {
        if (videoRawFile) {
            videoRawFile.close();
        }

        videoRawFile = SD_MMC.open(filename, FILE_READ);
        if (!videoRawFile) break;

        String lower = filename;
        lower.toLowerCase();
        if (lower.endsWith(".rgb")) {
            videoRawWidth = TFT_WIDTH;
            videoRawHeight = TFT_HEIGHT;
            videoRawFps = RGB_RAW_FIXED_FPS;
            videoRawFrameSize = (size_t)videoRawWidth * (size_t)videoRawHeight * 2U;
            videoRawHeaderSize = 0;
            videoTargetFps = videoRawFps;
            ok = true;
            break;
        }

        uint8_t header[10] = {0};
        if (videoRawFile.read(header, sizeof(header)) != (int)sizeof(header)) {
            videoRawFile.close();
            break;
        }

        if (!(header[0] == 'V' && header[1] == '1' && header[2] == '6' && header[3] == 'F')) {
            videoRawFile.close();
            break;
        }

        videoRawWidth = (uint16_t)header[4] | ((uint16_t)header[5] << 8);
        videoRawHeight = (uint16_t)header[6] | ((uint16_t)header[7] << 8);
        videoRawFps = (uint16_t)header[8] | ((uint16_t)header[9] << 8);

        if (videoRawWidth != TFT_WIDTH || videoRawHeight != TFT_HEIGHT || videoRawFps == 0) {
            videoRawFile.close();
            break;
        }

        videoRawFrameSize = (size_t)videoRawWidth * (size_t)videoRawHeight * 2U;
        videoRawHeaderSize = sizeof(header);
        videoTargetFps = videoRawFps;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _readRawFrameToBuffer(uint8_t frameIndex) {
    if (!sdMutex || frameIndex >= VIDEO_FRAME_BUFFER_COUNT || !videoFrameBuffers[frameIndex]) return false;
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(150))) {
        _videoSdReadEnd();
        return false;
    }

    if (!videoRawFile) {
        xSemaphoreGive(sdMutex);
        _videoSdReadEnd();
        return false;
    }

    int readLen = videoRawFile.read(videoFrameBuffers[frameIndex], videoRawFrameSize);
    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();
    return readLen == (int)videoRawFrameSize;
}

bool _skipRawFrames(uint32_t frameCount) {
    if (!sdMutex || frameCount == 0 || videoRawFrameSize == 0) return false;
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(80))) {
        _videoSdReadEnd();
        return false;
    }

    if (!videoRawFile) {
        xSemaphoreGive(sdMutex);
        _videoSdReadEnd();
        return false;
    }

    uint64_t skipBytes = (uint64_t)videoRawFrameSize * (uint64_t)frameCount;
    uint64_t cur_pos = (uint64_t)videoRawFile.position();
    uint64_t fileSize = (uint64_t)videoRawFile.size();
    uint64_t newPos = cur_pos + skipBytes;
    if (newPos > fileSize) {
        newPos = fileSize;
    }

    bool ok = videoRawFile.seek((uint32_t)newPos, SeekSet);
    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();
    return ok;
}

void _closeRawFrameStream() {
    if (videoRawFile) {
        videoRawFile.close();
    }
}

static esp_jpeg_image_scale_t _selectJpegScale(uint16_t width, uint16_t height) {
    size_t maxPixels = videoMjpegDecodeBufferSize / 2U;
    if (maxPixels == 0) {
        return JPEG_IMAGE_SCALE_1_8;
    }

    for (int s = 0; s <= 3; s++) {
        uint16_t scaledW = width;
        uint16_t scaledH = height;
        for (int i = 0; i < s; i++) {
            scaledW = (uint16_t)((scaledW + 1U) / 2U);
            scaledH = (uint16_t)((scaledH + 1U) / 2U);
        }
        size_t pixels = (size_t)scaledW * (size_t)scaledH;
        if (pixels <= maxPixels) {
            return (esp_jpeg_image_scale_t)s;
        }
    }

    return JPEG_IMAGE_SCALE_1_8;
}

static bool _decodeJpegToFrameBuffer(const uint8_t *jpegData, size_t jpegSize, uint8_t frameIndex, bool displayNative = false) {
    if (!jpegData || jpegSize < 4 || frameIndex >= VIDEO_FRAME_BUFFER_COUNT) return false;
    if (!videoFrameBuffers[frameIndex] || !videoMjpegDecodeBuffer) return false;

    esp_jpeg_image_cfg_t probeCfg = {};
    probeCfg.indata = (uint8_t*)jpegData;
    probeCfg.indata_size = jpegSize;
    probeCfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    probeCfg.out_scale = JPEG_IMAGE_SCALE_0;

    esp_jpeg_image_output_t probeOut = {};
    if (esp_jpeg_get_image_info(&probeCfg, &probeOut) != ESP_OK || probeOut.width == 0 || probeOut.height == 0) {
        return false;
    }

    esp_jpeg_image_scale_t scale = _selectJpegScale(probeOut.width, probeOut.height);
    const bool rotateCw = (MJPEG_ROTATE_90_CW != 0);
    const bool rotateCcw = (MJPEG_ROTATE_90_CCW != 0);
    const bool needsRotate = rotateCw || rotateCcw;
    const bool needsTransform = needsRotate || MJPEG_FLIP_X || MJPEG_FLIP_Y;

    if (displayNative && !needsTransform) {
        if (probeOut.width != TFT_WIDTH || probeOut.height != TFT_HEIGHT) {
            return false;
        }

        esp_jpeg_image_cfg_t nativeCfg = {};
        nativeCfg.indata = (uint8_t*)jpegData;
        nativeCfg.indata_size = jpegSize;
        nativeCfg.outbuf = videoFrameBuffers[frameIndex];
        nativeCfg.outbuf_size = videoRawFrameSize;
        nativeCfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
        nativeCfg.out_scale = JPEG_IMAGE_SCALE_0;
        nativeCfg.flags.swap_color_bytes = MJPEG_SWAP_RGB565_BYTES;

        esp_jpeg_image_output_t nativeOut = {};
        return esp_jpeg_decode(&nativeCfg, &nativeOut) == ESP_OK &&
               nativeOut.width == TFT_WIDTH &&
               nativeOut.height == TFT_HEIGHT;
    }

    esp_jpeg_image_cfg_t decodeCfg = {};
    decodeCfg.indata = (uint8_t*)jpegData;
    decodeCfg.indata_size = jpegSize;
    decodeCfg.outbuf = videoMjpegDecodeBuffer;
    decodeCfg.outbuf_size = videoMjpegDecodeBufferSize;
    decodeCfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    decodeCfg.out_scale = scale;
    decodeCfg.flags.swap_color_bytes = 0;

    esp_jpeg_image_output_t decodeOut = {};
    if (esp_jpeg_decode(&decodeCfg, &decodeOut) != ESP_OK || decodeOut.width == 0 || decodeOut.height == 0) {
        return false;
    }

    uint8_t *target = videoFrameBuffers[frameIndex];
    memset(target, 0x00, videoRawFrameSize);

    uint16_t drawWidth = decodeOut.width;
    uint16_t drawHeight = decodeOut.height;
    if (needsRotate) {
        drawWidth = decodeOut.height;
        drawHeight = decodeOut.width;
    }

    uint16_t dstX = 0;
    uint16_t dstY = 0;
    uint16_t outW = drawWidth;
    uint16_t outH = drawHeight;

    uint16_t cropX = 0;
    uint16_t cropY = 0;
    uint16_t cropW = drawWidth;
    uint16_t cropH = drawHeight;

    if (MJPEG_FILL_SCREEN_CROP) {
        outW = TFT_WIDTH;
        outH = TFT_HEIGHT;

        uint32_t srcMul = (uint32_t)drawWidth * (uint32_t)TFT_HEIGHT;
        uint32_t dstMul = (uint32_t)TFT_WIDTH * (uint32_t)drawHeight;
        if (srcMul > dstMul) {
            cropW = (uint16_t)(((uint32_t)drawHeight * (uint32_t)TFT_WIDTH) / (uint32_t)TFT_HEIGHT);
            cropX = (uint16_t)((drawWidth - cropW) / 2U);
        } else if (srcMul < dstMul) {
            cropH = (uint16_t)(((uint32_t)drawWidth * (uint32_t)TFT_HEIGHT) / (uint32_t)TFT_WIDTH);
            cropY = (uint16_t)((drawHeight - cropH) / 2U);
        }
    } else {
        if (outW > TFT_WIDTH || outH > TFT_HEIGHT) {
            return false;
        }
        dstX = (uint16_t)((TFT_WIDTH - outW) / 2U);
        dstY = (uint16_t)((TFT_HEIGHT - outH) / 2U);
    }

    for (uint16_t y = 0; y < outH; y++) {
        for (uint16_t x = 0; x < outW; x++) {
            uint16_t srcX;
            uint16_t srcY;

            uint16_t tx = x;
            uint16_t ty = y;
            if (MJPEG_FILL_SCREEN_CROP) {
                tx = (uint16_t)(cropX + ((uint32_t)x * (uint32_t)cropW) / (uint32_t)outW);
                ty = (uint16_t)(cropY + ((uint32_t)y * (uint32_t)cropH) / (uint32_t)outH);
            }

            if (rotateCw) {
                srcX = ty;
                srcY = (uint16_t)(decodeOut.height - 1U - tx);
            } else if (rotateCcw) {
                srcX = (uint16_t)(decodeOut.width - 1U - ty);
                srcY = tx;
            } else {
                srcX = tx;
                srcY = ty;
            }

            if (MJPEG_FLIP_X) {
                srcX = (uint16_t)(decodeOut.width - 1U - srcX);
            }
            if (MJPEG_FLIP_Y) {
                srcY = (uint16_t)(decodeOut.height - 1U - srcY);
            }

            size_t srcIndex = ((size_t)srcY * (size_t)decodeOut.width + (size_t)srcX) * 2U;
            uint8_t lo = videoMjpegDecodeBuffer[srcIndex];
            uint8_t hi = videoMjpegDecodeBuffer[srcIndex + 1U];

            size_t dstIndex = ((size_t)(dstY + y) * (size_t)TFT_WIDTH + (size_t)(dstX + x)) * 2U;
            if (MJPEG_SWAP_RGB565_BYTES) {
                target[dstIndex] = hi;
                target[dstIndex + 1U] = lo;
            } else {
                target[dstIndex] = lo;
                target[dstIndex + 1U] = hi;
            }
        }
    }

    return true;
}

bool _openMjpegStream(const String& filename) {
    if (!sdMutex || !videoFrameBuffers[0] || !videoFrameBuffers[1]) return false;
    if (!videoMjpegFrameData || !videoMjpegDecodeBuffer || videoMjpegFrameCapacity == 0) return false;
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300))) return false;

    bool ok = false;
    do {
        if (videoMjpegFile) {
            videoMjpegFile.close();
        }

        videoMjpegFile = SD_MMC.open(filename, FILE_READ);
        if (!videoMjpegFile) break;

        videoMjpegLastReadEof = false;
        videoTargetFps = MJPEG_DEFAULT_FPS;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _readMjpegFrameToBuffer(uint8_t frameIndex) {
    if (!sdMutex || !videoMjpegFile || frameIndex >= VIDEO_FRAME_BUFFER_COUNT) return false;
    if (!videoMjpegFrameData || videoMjpegFrameCapacity == 0) return false;

    size_t frameLen = 0;
    if (!_readMjpegFrameBytes(&frameLen)) {
        return false;
    }

    if (frameLen < 4 || frameLen > videoMjpegFrameCapacity) {
        return false;
    }

    return _decodeJpegToFrameBuffer(videoMjpegFrameData, frameLen, frameIndex);
}

bool _readMjpegFrameBytes(size_t *outFrameLen) {
    if (!outFrameLen) return false;
    *outFrameLen = 0;
    if (!sdMutex || !videoMjpegFrameData || videoMjpegFrameCapacity == 0) return false;

    uint8_t prev = 0;
    bool foundSoi = false;
    bool foundEoi = false;
    bool overflow = false;
    size_t frameLen = 0;
    videoMjpegLastReadEof = false;

    uint8_t chunk[MJPEG_READ_CHUNK_BYTES];
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(60))) {
        _videoSdReadEnd();
        return false;
    }

    if (!videoMjpegFile) {
        xSemaphoreGive(sdMutex);
        _videoSdReadEnd();
        return false;
    }

    while (!foundEoi) {
        int readLen = videoMjpegFile.read(chunk, sizeof(chunk));
        if (readLen <= 0) {
            videoMjpegLastReadEof = true;
            break;
        }

        for (int i = 0; i < readLen; i++) {
            uint8_t b = chunk[i];
            if (!foundSoi) {
                if (prev == 0xFF && b == 0xD8) {
                    if (videoMjpegFrameCapacity < 2) {
                        overflow = true;
                        break;
                    }
                    videoMjpegFrameData[0] = 0xFF;
                    videoMjpegFrameData[1] = 0xD8;
                    frameLen = 2;
                    foundSoi = true;
                }
            } else {
                if (frameLen < videoMjpegFrameCapacity) {
                    videoMjpegFrameData[frameLen++] = b;
                } else {
                    overflow = true;
                    break;
                }

                if (prev == 0xFF && b == 0xD9) {
                    foundEoi = true;
                    int unreadBytes = readLen - i - 1;
                    if (unreadBytes > 0) {
                        uint32_t curPos = videoMjpegFile.position();
                        videoMjpegFile.seek(curPos - (uint32_t)unreadBytes, SeekSet);
                    }
                    break;
                }
            }
            prev = b;
        }

        if (overflow) break;
    }

    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();

    if (!foundSoi || !foundEoi || overflow || frameLen < 4) {
        return false;
    }

    *outFrameLen = frameLen;
    return true;
}

bool _skipMjpegFrames(uint32_t frameCount) {
    if (frameCount == 0) return true;
    for (uint32_t i = 0; i < frameCount; i++) {
        size_t discardedLen = 0;
        if (!_readMjpegFrameBytes(&discardedLen)) {
            return false;
        }
    }
    return true;
}

void _closeMjpegStream() {
    if (videoMjpegFile) {
        videoMjpegFile.close();
    }
    videoMjpegLastReadEof = false;
}

bool _openHmjStream(const String& filename) {
    if (!sdMutex || !videoFrameBuffers[0] || !videoFrameBuffers[1]) return false;
    if (!videoMjpegFrameData || !videoMjpegDecodeBuffer || videoMjpegFrameCapacity == 0) return false;
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(300))) return false;

    bool ok = false;
    do {
        if (videoHmjFile) {
            videoHmjFile.close();
        }

        videoHmjFile = SD_MMC.open(filename, FILE_READ);
        if (!videoHmjFile) break;

        uint8_t header[HMJ_HEADER_BYTES] = {0};
        if (!_readFileExact(videoHmjFile, header, sizeof(header))) {
            videoHmjFile.close();
            break;
        }

        if (header[0] != HMJ_MAGIC_0 || header[1] != HMJ_MAGIC_1 ||
            header[2] != HMJ_MAGIC_2 || header[3] != HMJ_MAGIC_3) {
            videoHmjFile.close();
            break;
        }

        videoHmjWidth = _readLe16(header + 4);
        videoHmjHeight = _readLe16(header + 6);
        videoHmjFps = _readLe16(header + 8);
        videoHmjFrameCount = _readLe32(header + 12);

        if (videoHmjWidth != TFT_WIDTH || videoHmjHeight != TFT_HEIGHT ||
            videoHmjFps == 0 || videoHmjFps > 60 || videoHmjFrameCount == 0) {
            videoHmjFile.close();
            break;
        }

        videoTargetFps = videoHmjFps;
        videoFrameCount = videoHmjFrameCount;
        videoHmjCurrentFrame = 0;
        videoHmjHeaderSize = HMJ_HEADER_BYTES;
        videoHmjLastReadEof = false;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    return ok;
}

bool _readHmjFrameHeader(uint32_t *outPtsMs, uint32_t *outJpegLen) {
    if (!outPtsMs || !outJpegLen) return false;
    *outPtsMs = 0;
    *outJpegLen = 0;
    if (!sdMutex || !videoHmjFile) return false;

    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(80))) {
        _videoSdReadEnd();
        return false;
    }

    bool ok = false;
    do {
        if (!videoHmjFile) break;
        if (videoHmjFrameCount > 0 && videoHmjCurrentFrame >= videoHmjFrameCount) {
            videoHmjLastReadEof = true;
            break;
        }

        uint8_t frameHeader[HMJ_FRAME_HEADER_BYTES] = {0};
        if (!_readFileExact(videoHmjFile, frameHeader, sizeof(frameHeader))) {
            videoHmjLastReadEof = true;
            break;
        }

        uint32_t ptsMs = _readLe32(frameHeader);
        uint32_t jpegLen = _readLe32(frameHeader + 4);
        if (jpegLen < 4 || jpegLen > videoMjpegFrameCapacity) {
            break;
        }

        *outPtsMs = ptsMs;
        *outJpegLen = jpegLen;
        videoHmjLastReadEof = false;
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();
    return ok;
}

bool _readHmjFramePayloadToBuffer(uint32_t jpegLen, uint8_t frameIndex) {
    if (!sdMutex || !videoHmjFile || frameIndex >= VIDEO_FRAME_BUFFER_COUNT) return false;
    if (!videoMjpegFrameData || jpegLen < 4 || jpegLen > videoMjpegFrameCapacity) return false;

    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(120))) {
        _videoSdReadEnd();
        return false;
    }

    bool ok = false;
    do {
        if (!videoHmjFile) break;
        if (!_readFileExact(videoHmjFile, videoMjpegFrameData, jpegLen)) {
            videoHmjLastReadEof = true;
            break;
        }
        ok = true;
    } while (false);

    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();

    if (!ok) return false;
    return _decodeJpegToFrameBuffer(videoMjpegFrameData, jpegLen, frameIndex, true);
}

bool _skipHmjFramePayload(uint32_t jpegLen) {
    if (!sdMutex || !videoHmjFile || jpegLen == 0) return false;

    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(80))) {
        _videoSdReadEnd();
        return false;
    }

    bool ok = false;
    do {
        if (!videoHmjFile) break;
        uint64_t curPos = (uint64_t)videoHmjFile.position();
        uint64_t newPos = curPos + (uint64_t)jpegLen;
        if (newPos > (uint64_t)videoHmjFile.size()) {
            videoHmjLastReadEof = true;
            break;
        }
        ok = videoHmjFile.seek((uint32_t)newPos, SeekSet);
    } while (false);

    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();
    return ok;
}

bool _rewindHmjStream() {
    if (!sdMutex || !videoHmjFile) return false;
    _videoSdReadBegin();
    if (!xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100))) {
        _videoSdReadEnd();
        return false;
    }
    bool ok = videoHmjFile.seek(videoHmjHeaderSize, SeekSet);
    if (ok) {
        videoHmjCurrentFrame = 0;
        videoHmjLastReadEof = false;
    }
    xSemaphoreGive(sdMutex);
    _videoSdReadEnd();
    return ok;
}

void _closeHmjStream() {
    if (videoHmjFile) {
        videoHmjFile.close();
    }
    videoHmjLastReadEof = false;
    videoHmjCurrentFrame = 0;
    videoHmjFrameCount = 0;
}

void _tryStartVideoCompanionAudio(const String& videoFilename, bool loopRequested) {
    String lower = videoFilename;
    lower.toLowerCase();
    if (!(lower.endsWith(".hmj") || lower.endsWith(".mjpg") || lower.endsWith(".mjpeg") ||
          lower.endsWith(".v16") || lower.endsWith(".vid") || lower.endsWith(".rgb"))) {
        videoCompanionAudioActive = false;
        videoCompanionAudioLoop = false;
        videoCompanionAudioFile = "";
        videoCompanionAudioStartMs = 0;
        return;
    }

    String audioFile = videoFilename;
    int dot = audioFile.lastIndexOf('.');
    if (dot < 0) return;
    audioFile = audioFile.substring(0, dot) + ".mp3";

    bool exists = false;
    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        exists = SD_MMC.exists(audioFile);
        xSemaphoreGive(sdMutex);
    }

    if (!exists) {
        videoCompanionAudioActive = false;
        videoCompanionAudioLoop = false;
        videoCompanionAudioFile = "";
        videoCompanionAudioStartMs = 0;
        Serial.println("No companion .mp3 found for video");
        return;
    }

    if (_startAudioPlayback(audioFile, loopRequested, 0, false, activeSessionId)) {
        Serial.printf("Companion audio started: %s\n", audioFile.c_str());
        videoAudioClockMs = 0;
        videoAudioClockLastWallMs = millis();
        videoAudioClockValid = false;
    } else {
        videoCompanionAudioActive = false;
        videoCompanionAudioLoop = false;
        videoCompanionAudioFile = "";
        videoCompanionAudioStartMs = 0;
        Serial.printf("Companion audio failed: %s\n", audioFile.c_str());
    }
}

void taskVideoDecode(void *pvParameters) {
    VideoPacket packet;
    uint32_t nextFrameAtMs = 0;
    uint32_t frameRemainderAcc = 0;
    bool hasPendingPacket = false;
    VideoPacket pendingPacket;
    bool hmjHeaderPending = false;
    uint32_t hmjPendingPtsMs = 0;
    uint32_t hmjPendingJpegLen = 0;

    while (true) {
        if (!videoPipelineActive || isUploading || currentMediaType != MEDIA_VIDEO) {
            nextFrameAtMs = 0;
            frameRemainderAcc = 0;
            hasPendingPacket = false;
            hmjHeaderPending = false;
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }

        uint32_t nowMs = millis();

        if (videoUseRawFrameStream) {
            if (nextFrameAtMs == 0) {
                nextFrameAtMs = nowMs;
                frameRemainderAcc = 0;
            }

            uint32_t framePeriodMs = (videoTargetFps == 0) ? 66 : (1000UL / videoTargetFps);
            uint32_t framePeriodRem = (videoTargetFps == 0) ? 10 : (1000UL % videoTargetFps);
            if (videoCompanionAudioActive && framePeriodMs > 0) {
                uint32_t playbackClockMs = _videoPlaybackClockMs();
                uint32_t fpsNow = (videoTargetFps == 0) ? 15 : videoTargetFps;
                uint32_t sourceFrame = videoFramesDecoded + videoFramesSkipped;
                uint32_t expectedPtsMs = (uint32_t)(((uint64_t)sourceFrame * 1000ULL) / (uint64_t)fpsNow);
                int32_t lagMs = (int32_t)playbackClockMs - (int32_t)expectedPtsMs;
                if (lagMs > VIDEO_LAG_SKIP_THRESHOLD_MS) {
                    uint32_t framesToSkip = (uint32_t)lagMs / framePeriodMs;
                    if (framesToSkip > 0) {
                        if (framesToSkip > 4U) framesToSkip = 4U;
                        if (_skipRawFrames(framesToSkip)) {
                            videoFramesSkipped += framesToSkip;
                        }
                    }
                }
            }

            if ((int32_t)(nowMs - nextFrameAtMs) >= 0) {
                uint8_t frameIndex = 0xFF;
                if (!_acquireVideoFrameBuffer(&frameIndex, 0)) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }

                uint32_t decodeStartMs = millis();
                if (_readRawFrameToBuffer(frameIndex)) {
                    _recordMovingAverage(videoDecodeLastMs, videoDecodeAvgMs, videoDecodeMaxMs, millis() - decodeStartMs);
                    uint32_t fpsNow = (videoTargetFps == 0) ? 15 : videoTargetFps;
                    uint32_t sourceFrame = videoFramesDecoded + videoFramesSkipped;
                    packet.ptsMs = (uint32_t)(((uint64_t)sourceFrame * 1000ULL) / (uint64_t)fpsNow);
                    packet.dataLen = videoFramesDecoded;
                    packet.keyFrame = ((videoFramesDecoded % 30U) == 0U);
                    packet.color565 = 0;
                    packet.frameIndex = frameIndex;

                    _queueVideoPacket(packet);
                    videoFramesDecoded++;
                } else {
                    _releaseVideoFrameBuffer(frameIndex);
                    videoStreamEof = true;
                    if (loopEnabled && videoRawFile) {
                        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            _videoSdReadBegin();
                            videoRawFile.seek(videoRawHeaderSize, SeekSet);
                            xSemaphoreGive(sdMutex);
                            _videoSdReadEnd();
                            videoStartMs = millis();
                            videoFramesDecoded = 0;
                            videoFramesSkipped = 0;
                            _resetVideoFrameQueues();
                            videoStreamEof = false;
                        }
                    } else {
                        _stopAudioSafely();
                        vTaskDelay(pdMS_TO_TICKS(25));
                        continue;
                    }
                }

                nextFrameAtMs += framePeriodMs;
                frameRemainderAcc += framePeriodRem;
                uint32_t fpsNow = (videoTargetFps == 0) ? 15 : videoTargetFps;
                if (frameRemainderAcc >= fpsNow) {
                    nextFrameAtMs += 1U;
                    frameRemainderAcc -= fpsNow;
                }
                uint32_t playbackClockMs = _videoPlaybackClockMs();
                if (videoCompanionAudioActive && (int32_t)(playbackClockMs - nextFrameAtMs) > (int32_t)VIDEO_LAG_CRITICAL_MS) {
                    nextFrameAtMs = playbackClockMs;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (videoUseHmjStream) {
            if (nextFrameAtMs == 0) {
                nextFrameAtMs = nowMs;
                frameRemainderAcc = 0;
            }

            if (!hmjHeaderPending) {
                uint32_t skippedThisPass = 0;
                while (true) {
                    if (!_readHmjFrameHeader(&hmjPendingPtsMs, &hmjPendingJpegLen)) {
                        videoStreamEof = videoHmjLastReadEof;
                        if (loopEnabled && videoHmjFile && _rewindHmjStream()) {
                            videoStartMs = millis();
                            videoFramesDecoded = 0;
                            videoFramesSkipped = 0;
                            _resetVideoFrameQueues();
                            videoStreamEof = false;
                            hmjHeaderPending = false;
                        } else {
                            _stopAudioSafely();
                            vTaskDelay(pdMS_TO_TICKS(25));
                        }
                        break;
                    }

                    uint32_t playbackClockMs = videoCompanionAudioActive ? _videoPlaybackClockMs() : (millis() - videoStartMs);
                    int32_t lateMs = (int32_t)playbackClockMs - (int32_t)hmjPendingPtsMs;
                    if (videoCompanionAudioActive && lateMs > (int32_t)VIDEO_LAG_SKIP_THRESHOLD_MS &&
                        skippedThisPass < MJPEG_AUDIO_MAX_SKIP_FRAMES) {
                        if (_skipHmjFramePayload(hmjPendingJpegLen)) {
                            videoHmjCurrentFrame++;
                            videoFramesSkipped++;
                            skippedThisPass++;
                            continue;
                        }
                        videoFramesDropped++;
                    }

                    hmjHeaderPending = true;
                    break;
                }
            }

            if (!hmjHeaderPending) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            uint32_t playbackClockMs = videoCompanionAudioActive ? _videoPlaybackClockMs() : (millis() - videoStartMs);
            if ((int32_t)(playbackClockMs - hmjPendingPtsMs) < 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (videoDisplayQueue && uxQueueSpacesAvailable(videoDisplayQueue) == 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            uint8_t frameIndex = 0xFF;
            if (!_acquireVideoFrameBuffer(&frameIndex, 0)) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            uint32_t decodeStartMs = millis();
            if (_readHmjFramePayloadToBuffer(hmjPendingJpegLen, frameIndex)) {
                _recordMovingAverage(videoDecodeLastMs, videoDecodeAvgMs, videoDecodeMaxMs, millis() - decodeStartMs);
                packet.ptsMs = hmjPendingPtsMs;
                packet.dataLen = videoHmjCurrentFrame;
                packet.keyFrame = ((videoHmjCurrentFrame % 30U) == 0U);
                packet.color565 = 0;
                packet.frameIndex = frameIndex;
                _queueVideoPacket(packet);
                videoFramesDecoded++;
                videoHmjCurrentFrame++;
                hmjHeaderPending = false;
            } else {
                _releaseVideoFrameBuffer(frameIndex);
                videoFramesDropped++;
                hmjHeaderPending = false;
                if (videoHmjLastReadEof) {
                    videoStreamEof = true;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (videoUseMjpegStream) {
            if (nextFrameAtMs == 0) {
                nextFrameAtMs = nowMs;
                frameRemainderAcc = 0;
            }

            uint32_t fpsNow = (videoTargetFps == 0) ? 15 : videoTargetFps;
            uint32_t framePeriodMs = (videoTargetFps == 0) ? 66 : (1000UL / videoTargetFps);
            uint32_t framePeriodRem = (videoTargetFps == 0) ? 10 : (1000UL % videoTargetFps);

            if (videoCompanionAudioActive && framePeriodMs > 0) {
                uint32_t playbackClockMs = _videoPlaybackClockMs();
                uint32_t sourceFrame = videoFramesDecoded + videoFramesSkipped;
                uint32_t expectedPtsMs = (uint32_t)(((uint64_t)sourceFrame * 1000ULL) / (uint64_t)fpsNow);
                int32_t lagMs = (int32_t)playbackClockMs - (int32_t)expectedPtsMs;
                if (lagMs > VIDEO_LAG_SKIP_THRESHOLD_MS) {
                    uint32_t framesToSkip = (uint32_t)lagMs / framePeriodMs;
                    if (framesToSkip > MJPEG_AUDIO_MAX_SKIP_FRAMES) framesToSkip = MJPEG_AUDIO_MAX_SKIP_FRAMES;
                    if (framesToSkip > 0 && _skipMjpegFrames(framesToSkip)) {
                        videoFramesSkipped += framesToSkip;
                    }
                }
            }

            bool dueNow = videoCompanionAudioActive || ((int32_t)(nowMs - nextFrameAtMs) >= 0);
            if (dueNow) {
                if (videoDisplayQueue && uxQueueSpacesAvailable(videoDisplayQueue) == 0) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }

                uint8_t frameIndex = 0xFF;
                if (!_acquireVideoFrameBuffer(&frameIndex, 0)) {
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }

                uint32_t decodeStartMs = millis();
                if (_readMjpegFrameToBuffer(frameIndex)) {
                    _recordMovingAverage(videoDecodeLastMs, videoDecodeAvgMs, videoDecodeMaxMs, millis() - decodeStartMs);
                    uint32_t sourceFrame = videoFramesDecoded + videoFramesSkipped;
                    packet.ptsMs = (uint32_t)(((uint64_t)sourceFrame * 1000ULL) / (uint64_t)fpsNow);
                    packet.dataLen = sourceFrame;
                    packet.keyFrame = ((sourceFrame % 30U) == 0U);
                    packet.color565 = 0;
                    packet.frameIndex = frameIndex;
                    _queueVideoPacket(packet);
                    videoFramesDecoded++;
                } else {
                    _releaseVideoFrameBuffer(frameIndex);
                    if (videoMjpegLastReadEof) {
                        videoStreamEof = true;
                        if (loopEnabled && videoMjpegFile) {
                            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                                _videoSdReadBegin();
                                videoMjpegFile.seek(0, SeekSet);
                                xSemaphoreGive(sdMutex);
                                _videoSdReadEnd();
                                videoStartMs = millis();
                                videoFramesDecoded = 0;
                                videoFramesSkipped = 0;
                                _resetVideoFrameQueues();
                                videoStreamEof = false;
                                videoMjpegLastReadEof = false;
                            }
                        } else {
                            _stopAudioSafely();
                            vTaskDelay(pdMS_TO_TICKS(25));
                            continue;
                        }
                    } else {
                        videoFramesDropped++;
                    }
                }

                if (!videoCompanionAudioActive) {
                    nextFrameAtMs += framePeriodMs;
                    frameRemainderAcc += framePeriodRem;
                    if (frameRemainderAcc >= fpsNow) {
                        nextFrameAtMs += 1U;
                        frameRemainderAcc -= fpsNow;
                    }
                    if ((int32_t)(nowMs - nextFrameAtMs) > (int32_t)framePeriodMs) {
                        nextFrameAtMs = nowMs + framePeriodMs;
                    }
                }
            }

            if ((nowMs - videoSyncLogLastMs) >= 1000U) {
                uint32_t playbackClockMs = _videoPlaybackClockMs();
                Serial.printf("AVSync video: audioMs=%u decoded=%u skipped=%u drift=%dms\n",
                              (uint32_t)playbackClockMs,
                              (uint32_t)videoFramesDecoded,
                              (uint32_t)videoFramesSkipped,
                              (int)videoAvDriftMs);
                videoSyncLogLastMs = nowMs;
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        if (videoUseFileStream) {
            if (!hasPendingPacket) {
                if (_readNextVideoPacket(pendingPacket)) {
                    hasPendingPacket = true;
                } else {
                    videoStreamEof = true;
                    if (loopEnabled && videoStreamFile) {
                        if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                            videoStreamFile.seek(videoStreamHeaderSize, SeekSet);
                            xSemaphoreGive(sdMutex);
                            videoStartMs = millis();
                            videoFramesDecoded = 0;
                            videoFramesSkipped = 0;
                            videoStreamEof = false;
                        }
                    } else {
                        _stopAudioSafely();
                        vTaskDelay(pdMS_TO_TICKS(25));
                        continue;
                    }
                }
            }

            if (hasPendingPacket) {
                uint32_t elapsedMs = millis() - videoStartMs;
                if ((int32_t)(elapsedMs - pendingPacket.ptsMs) >= 0) {
                    _queueVideoPacket(pendingPacket);
                    videoFramesDecoded++;
                    hasPendingPacket = false;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        // Phase 2 source: synthetic frame producer at target FPS.
        if (nextFrameAtMs == 0) {
            nextFrameAtMs = nowMs;
            frameRemainderAcc = 0;
        }

        uint32_t framePeriodMs = (videoTargetFps == 0) ? 66 : (1000UL / videoTargetFps);

        if ((int32_t)(nowMs - nextFrameAtMs) >= 0) {
            packet.ptsMs = nowMs - videoStartMs;
            packet.dataLen = videoFramesDecoded;
            packet.keyFrame = ((videoFramesDecoded % 30U) == 0U);
            packet.color565 = (uint16_t)((((videoFramesDecoded * 3U) & 0x1FU) << 11) |
                                         (((videoFramesDecoded * 5U) & 0x3FU) << 5) |
                                         ((videoFramesDecoded * 7U) & 0x1FU));
            packet.frameIndex = 0;

            _queueVideoPacket(packet);
            videoFramesDecoded++;

            nextFrameAtMs += framePeriodMs;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void taskDisplayFlush(void *pvParameters) {
    VideoPacket packet;
    VideoPacket latestPacket;
    bool hasPending = false;
    VideoPacket pendingPacket;
    while (true) {
        if (!videoPipelineActive || !displayReady || isUploading || currentMediaType != MEDIA_VIDEO) {
            if (hasPending && _packetUsesFrameBuffer(pendingPacket)) {
                _releaseVideoFrameBuffer(pendingPacket.frameIndex);
            }
            hasPending = false;
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }

        bool audioSyncedVideo = videoCompanionAudioActive && (videoUseMjpegStream || videoUseHmjStream);
        if (audioSyncedVideo) {
            if (!hasPending) {
                if (xQueueReceive(videoDisplayQueue, &pendingPacket, pdMS_TO_TICKS(2)) == pdTRUE) {
                    hasPending = true;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(1));
                    continue;
                }
            }

            uint32_t playbackClockMs = _videoPlaybackClockMs();
            int32_t lateMs = (int32_t)(playbackClockMs - pendingPacket.ptsMs);
            if (lateMs > (int32_t)MJPEG_DISPLAY_DROP_LAG_MS) {
                if (_packetUsesFrameBuffer(pendingPacket)) {
                    _releaseVideoFrameBuffer(pendingPacket.frameIndex);
                }
                videoFramesDropped++;
                hasPending = false;
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            if (lateMs < 0) {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }

            packet = pendingPacket;
            hasPending = false;
        } else {
            if (xQueueReceive(videoDisplayQueue, &packet, pdMS_TO_TICKS(2)) == pdTRUE) {
                while (xQueueReceive(videoDisplayQueue, &latestPacket, 0) == pdTRUE) {
                    if (_packetUsesFrameBuffer(packet)) {
                        _releaseVideoFrameBuffer(packet.frameIndex);
                    }
                    packet = latestPacket;
                    videoFramesDropped++;
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
                continue;
            }
        }

        uint32_t drawStartMs = millis();
        if (_packetUsesFrameBuffer(packet)) {
            st7789DrawFrameRGB565(videoFrameBuffers[packet.frameIndex], TFT_WIDTH, TFT_HEIGHT);
        } else {
            st7789FillColor(packet.color565);
        }
        _recordMovingAverage(videoDrawLastMs, videoDrawAvgMs, videoDrawMaxMs, millis() - drawStartMs);

        uint32_t playbackClockMs = _videoPlaybackClockMs();
        videoLastAudioClockMs = playbackClockMs;
        videoLastFramePtsMs = packet.ptsMs;
        videoAvDriftMs = (int32_t)playbackClockMs - (int32_t)packet.ptsMs;
        videoFramesDisplayed++;
        if (_packetUsesFrameBuffer(packet)) {
            _releaseVideoFrameBuffer(packet.frameIndex);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// -------------------- Tasks Setup --------------------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n--- ESP32 Haptic Core (ESP8266Audio Version) ---");
    audioLogger = &Serial;

    // Load preferences
    prefs.begin("settings", false);
    speakerVolumePct = prefs.getUInt("spk_vol", 75);
    exciterVolumePct = prefs.getUInt("exc_vol", 50);

    // Initialize LED system
    FastLED.addLeds<WS2812B, LED_PIN, GRB>(leds, NUM_LEDS).setCorrection(TypicalLEDStrip);
    ledMode = prefs.getString("led_mode", "random");
    ledBrightness = prefs.getUInt("led_bright", 100);
    ledColorHex = prefs.getString("led_color", "#FF0000");
    FastLED.setBrightness(ledBrightness);

    // Initialize SD_MMC (1-bit mode)
    if (!SD_MMC.setPins(SDMMC_CLK_PIN, SDMMC_CMD_PIN, SDMMC_D0_PIN)) {
        Serial.println("SD_MMC pin configuration failed!");
    }

    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("SD_MMC initialization failed!");
    } else {
        Serial.println("SD_MMC initialized.");
    }

    sdMutex = xSemaphoreCreateMutex();
    audioMutex = xSemaphoreCreateMutex();
    if (!audioMutex) {
        Serial.println("WARN: Failed to create audio mutex");
    }
    audioCmdQueue = xQueueCreate(8, sizeof(AudioCommandMessage));
    if (!audioCmdQueue) {
        Serial.println("ERROR: Failed to create audio command queue");
    }

    initDisplayVideoScaffold();

    // Initialize custom ESP-IDF Direct DMA Audio Pipeline
    out = new AudioOutputHaptic(I2S_BCLK, I2S_LRC, I2S_DOUT);
    
    updateAudioVolumeAndBalance();

    // Connect WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 20) {
        delay(500);
        Serial.print(".");
        retries++;
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi connected.");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi connection failed! Proceeding anyway...");
    }

    // Setup routes
    server.on("/api/media/files", HTTP_GET, handleApiFiles);
    server.on("/api/media/files", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/play", HTTP_POST, handleApiPlay);
    server.on("/api/media/play", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/stop", HTTP_POST, handleApiStop);
    server.on("/api/media/stop", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/pause", HTTP_POST, handleApiPause);
    server.on("/api/media/pause", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/seek", HTTP_POST, handleApiSeek);
    server.on("/api/media/seek", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/current", HTTP_GET, handleApiCurrent);
    server.on("/api/media/current", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/volume/speaker", HTTP_POST, handleApiVolumeSpeaker);
    server.on("/api/volume/speaker", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/volume/exciter", HTTP_POST, handleApiVolumeExciter);
    server.on("/api/volume/exciter", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/delete", HTTP_DELETE, handleApiMediaDelete);
    server.on("/api/media/delete", HTTP_POST, handleApiMediaDelete);
    server.on("/api/media/delete", HTTP_OPTIONS, handleCorsOptions);
    // Note: Upload multipart route isn't easily done in server.on without huge callbacks. Using standard for now.
    server.on("/api/media/upload", HTTP_POST, [](){
        uint32_t waitStartMs = millis();
        while (!uploadProcessingDone && (millis() - waitStartMs) < 15000U) {
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        DynamicJsonDocument resp(512);
        if (!uploadProcessingDone) {
            resp["error"] = "Upload finalize timeout";
            resp["received"] = uploadBytesReceived;
            resp["written"] = uploadBytesWritten;
            if (uploadPartialPath.length()) {
                resp["partial"] = uploadPartialPath;
            }
            String payload;
            serializeJson(resp, payload);
            server.send(504, "application/json", payload);
            return;
        }

        if (uploadHadWriteError) {
            resp["error"] = uploadErrorMessage.length() ? uploadErrorMessage : "Upload failed during SD write";
            resp["received"] = uploadBytesReceived;
            resp["written"] = uploadBytesWritten;
            if (uploadPartialPath.length()) {
                resp["partial"] = uploadPartialPath;
            }
            String payload;
            serializeJson(resp, payload);
            server.send(507, "application/json", payload);
        } else {
            resp["status"] = "Upload done";
            resp["written"] = uploadBytesWritten;
            String payload;
            serializeJson(resp, payload);
            server.send(200, "application/json", payload);
        }
    }, handleApiMediaUpload);
    server.on("/api/media/upload", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/media/storage", HTTP_GET, handleApiMediaStorage);
    server.on("/api/media/storage", HTTP_OPTIONS, handleCorsOptions);

    server.on("/api/led/mode", HTTP_POST, handleApiLedMode);
    server.on("/api/led/mode", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/led/color", HTTP_POST, handleApiLedColor);
    server.on("/api/led/color", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/led/brightness", HTTP_POST, handleApiLedBrightness);
    server.on("/api/led/brightness", HTTP_OPTIONS, handleCorsOptions);
    
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/status", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/power/toggle", HTTP_POST, handleApiPowerToggle);
    server.on("/api/power/toggle", HTTP_OPTIONS, handleCorsOptions);
    server.on("/api/battery", HTTP_GET, handleApiBattery);
    server.on("/api/battery", HTTP_OPTIONS, handleCorsOptions);
    
    // Explicit OPTIONS handler for CORS preflight (Allows Swagger UI to work)
    server.onNotFound([]() {
        if (server.method() == HTTP_OPTIONS) {
            server.sendHeader("Access-Control-Allow-Origin", "*");
            server.sendHeader("Access-Control-Allow-Methods", "POST, GET, OPTIONS");
            server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
            server.send(204);
        } else {
            server.send(404, "text/plain", "Not Found");
        }
    });

    // Enable CORS
    server.enableCORS(true);
    server.begin();

    // Create proper FreeRTOS Tasks
    xTaskCreatePinnedToCore(taskWebServer, "WebServerTask", 8192, NULL, PRIO_WEB, &webServerTaskHandle, CORE_SERVICE);
    xTaskCreatePinnedToCore(taskAudio, "AudioTask", 16384, NULL, PRIO_AUDIO, &audioTaskHandle, CORE_SERVICE);
    xTaskCreatePinnedToCore(taskLEDs, "LEDTask", 4096, NULL, PRIO_LED, &ledTaskHandle, CORE_SERVICE);
    xTaskCreatePinnedToCore(taskVideoDecode, "VideoDecodeTask", 12288, NULL, PRIO_VIDEO, &videoDecodeTaskHandle, CORE_VIDEO);
    xTaskCreatePinnedToCore(taskDisplayFlush, "DisplayFlushTask", 8192, NULL, PRIO_DISPLAY, &displayFlushTaskHandle, CORE_VIDEO);

}
//                 }
//             }
//         }
//         vTaskDelay(1);
//     }
// }

// -------------------- Loop (Core 1) --------------------
// (Moved _stopAudioSafely up above setup)

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}

void handleCorsOptions() {
    server.sendHeader("Access-Control-Allow-Origin", "*");
    server.sendHeader("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
    server.sendHeader("Access-Control-Allow-Headers", "Content-Type, Accept");
    server.sendHeader("Access-Control-Allow-Private-Network", "true");
    server.send(204);
}

// -------------------- Volume Logic --------------------
void updateAudioVolumeAndBalance() {
    if (out) {
        out->leftVol = (float)exciterVolumePct / 100.0f;
        out->rightVol = (float)speakerVolumePct / 100.0f;
    }
}

// -------------------- API Handlers (Core 0) --------------------
void handleApiFiles() {
    if (isUploading) {
        server.send(503, "application/json", "{\"error\":\"Upload in progress\"}");
        return;
    }

    DynamicJsonDocument doc(4096);
    JsonArray files = doc.createNestedArray("files");

    TickType_t waitTicks = (currentMediaType == MEDIA_VIDEO) ? pdMS_TO_TICKS(15) : portMAX_DELAY;
    if (xSemaphoreTake(sdMutex, waitTicks)) {
        File root = SD_MMC.open("/");
        if (!root) {
            xSemaphoreGive(sdMutex);
            server.send(500, "application/json", "{\"error\":\"Failed to open directory\"}");
            return;
        }

        File file = root.openNextFile();
        while (file) {
            if (!file.isDirectory()) {
                JsonObject f = files.createNestedObject();
                f["filename"] = String("/") + file.name();
                f["size"] = file.size();
                
                String name = String(file.name());
                name.toLowerCase();
                if (name.endsWith(".mp3")) {
                    f["type"] = "audio";
                } else if (name.endsWith(".hmj") || name.endsWith(".mjpg") || name.endsWith(".mjpeg") ||
                           name.endsWith(".vid") || name.endsWith(".v16") || name.endsWith(".rgb")) {
                    f["type"] = "video";
                } else if (name.endsWith(".part")) {
                    f["type"] = "partial";
                } else {
                    f["type"] = "unknown";
                }
            }
            File nextFile = root.openNextFile();
            file.close();
            file = nextFile;
        }
        root.close();
        xSemaphoreGive(sdMutex);
    } else {
        server.send(503, "application/json", "{\"error\":\"SD busy during playback\"}");
        return;
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiPlay() {
    if (isUploading) {
        server.send(503, "application/json", "{\"error\":\"Upload in progress\"}");
        return;
    }

    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }

    Serial.println("Received /api/media/play");

    DynamicJsonDocument doc(1024);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    String filename = doc["filename"] | "";
    if (filename.length() == 0) {
        server.send(400, "application/json", "{\"error\":\"Missing filename\"}");
        return;
    }

    bool requestedLoop = doc["loop"] | false;
    mediaSessionId++;
    uint32_t requestedSessionId = mediaSessionId;
    stopRequested = false;
    if (!filename.startsWith("/")) {
        filename = "/" + filename;
    }

    String fnameLower = filename;
    fnameLower.toLowerCase();
    if (fnameLower.endsWith(".hmj") || fnameLower.endsWith(".mjpg") || fnameLower.endsWith(".mjpeg") ||
        fnameLower.endsWith(".vid") || fnameLower.endsWith(".v16") || fnameLower.endsWith(".rgb")) {
        audioAbortRequested = false;
        if (audioCmdQueue) {
            xQueueReset(audioCmdQueue);
        }

        if (!_startVideoPlayback(filename, requestedLoop, requestedSessionId)) {
            server.send(409, "application/json", "{\"error\":\"Failed to start video playback\"}");
            return;
        }
        server.send(200, "application/json", "{\"status\":\"Video playback configured\",\"filename\":\"" + filename + "\"}");
        return;
    }

    Serial.printf("AUDIO_CMD_PLAY: %s loop=%s\n", filename.c_str(), requestedLoop ? "true" : "false");

    audioAbortRequested = true;
    if (audioCmdQueue) {
        xQueueReset(audioCmdQueue);
    }

    AudioCommandMessage cmd = {};
    cmd.cmd = CMD_PLAY;
    cmd.loopRequested = requestedLoop;
    cmd.sessionId = requestedSessionId;
    strncpy(cmd.filename, filename.c_str(), sizeof(cmd.filename) - 1);

    if (!_enqueueAudioCommand(cmd, 100)) {
        server.send(503, "application/json", "{\"error\":\"Audio command queue busy\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"Playback configured\",\"filename\":\"" + filename + "\"}");
}

void handleApiStop() {
    Serial.println("AUDIO_CMD_STOP");

    stopRequested = true;
    mediaSessionId++;
    activeSessionId = 0;

    if (audioCmdQueue) {
        xQueueReset(audioCmdQueue);
    }

    audioAbortRequested = true;
    _stopAudioSafely();
    if (videoDisplayQueue) xQueueReset(videoDisplayQueue);
    videoFramesDecoded = 0;
    videoFramesDisplayed = 0;
    videoFramesDropped = 0;
    videoFramesSkipped = 0;
    videoAvDriftMs = 0;
    videoStart_state = VIDEO_START_IDLE;
    videoFrameCount = 0;
    _resetVideoFrameQueues();

    server.send(200, "application/json", "{\"status\":\"Playback stopped\"}");
}

void handleApiPause() {
    Serial.println("AUDIO_CMD_TOGGLE_PAUSE");
    bool hasMp3 = false;
    if (currentMediaType == MEDIA_AUDIO) {
        if (audioMutex) {
            if (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                hasMp3 = (mp3 != NULL);
                xSemaphoreGive(audioMutex);
            }
        } else {
            hasMp3 = (mp3 != NULL);
        }
    }
    if (hasMp3) {
        isPaused = !isPaused;
    }
    server.send(200, "application/json", String("{\"status\":\"Pause/Resume toggled\",\"paused\":") + (isPaused ? "true}" : "false}"));
}

void handleApiSeek() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }

    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    int positionSec = doc["position"] | -1;
    if (positionSec < 0) {
        server.send(400, "application/json", "{\"error\":\"position must be >= 0\"}");
        return;
    }

    Serial.printf("AUDIO_CMD_SEEK: %d sec\n", positionSec);

    audioAbortRequested = true;

    AudioCommandMessage cmd = {};
    cmd.cmd = CMD_SEEK;
    cmd.positionSec = (uint32_t)positionSec;
    if (!_enqueueAudioCommand(cmd, 100)) {
        server.send(503, "application/json", "{\"error\":\"Audio command queue busy\"}");
        return;
    }

    server.send(200, "application/json", "{\"status\":\"Seek requested\"}");
}

void handleApiCurrent() {
    DynamicJsonDocument doc(512);
    
    doc["playing"] = (!isPaused && currentMediaType == MEDIA_AUDIO && currentFilename != "");
    doc["paused"] = isPaused;
    doc["filename"] = currentFilename;
    doc["loop"] = loopEnabled;

    String typeStr = "none";
    if (currentMediaType == MEDIA_AUDIO) typeStr = "audio";
    if (currentMediaType == MEDIA_VIDEO) typeStr = "video";
    doc["type"] = typeStr;

    if (currentMediaType == MEDIA_AUDIO) {
        bool audioLocked = false;
        if (audioMutex) {
            audioLocked = (xSemaphoreTake(audioMutex, pdMS_TO_TICKS(20)) == pdTRUE);
        }
        if (!audioMutex || audioLocked) {
            if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
                if (fileSource && fileSource->isOpen()) {
                    uint32_t pos = fileSource->getPos();
                    uint32_t size = fileSource->getSize();
                    doc["position_bytes"] = pos;
                    doc["size_bytes"] = size;
                    doc["position"] = (uint32_t)((uint64_t)pos * 8ULL / 128000ULL);
                    doc["duration"] = (uint32_t)((uint64_t)size * 8ULL / 128000ULL);
                }
                if (id3Source) {
                    doc["id3_parsed"] = true; // Ready for album arts down the line
                }
                xSemaphoreGive(sdMutex);
            }
        }
        if (audioLocked) {
            xSemaphoreGive(audioMutex);
        }
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiVolumeSpeaker() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    int level = doc["level"] | -1;
    if (level < 0 || level > 100) {
        server.send(400, "application/json", "{\"error\":\"level must be 0..100\"}");
        return;
    }

    speakerVolumePct = (uint8_t)level;
    
    prefs.putUInt("spk_vol", speakerVolumePct);
    updateAudioVolumeAndBalance();
    
    server.send(200, "application/json", "{\"status\":\"Speaker volume set\"}");
}

void handleApiVolumeExciter() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    if (deserializeJson(doc, server.arg("plain"))) {
        server.send(400, "application/json", "{\"error\":\"Invalid JSON\"}");
        return;
    }

    int level = doc["level"] | -1;
    if (level < 0 || level > 100) {
        server.send(400, "application/json", "{\"error\":\"level must be 0..100\"}");
        return;
    }

    exciterVolumePct = (uint8_t)level;

    prefs.putUInt("exc_vol", exciterVolumePct);
    updateAudioVolumeAndBalance();
    
    server.send(200, "application/json", "{\"status\":\"Exciter volume set\"}");
}

void handleApiLedMode() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    deserializeJson(doc, server.arg("plain"));
    
    String mode = doc["mode"] | "off";
    if (mode == "random" || mode == "frequency" || mode == "oled_sync" || mode == "manual" || mode == "off") {
        ledMode = mode;
        prefs.putString("led_mode", ledMode);
        server.send(200, "application/json", "{\"status\":\"LED mode set\"}");
    } else {
        server.send(400, "application/json", "{\"error\":\"Invalid mode\"}");
    }
}

void handleApiLedColor() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    deserializeJson(doc, server.arg("plain"));
    
    ledColorHex = doc["color"] | ledColorHex;
    if (doc.containsKey("brightness")) {
        ledBrightness = doc["brightness"];
        FastLED.setBrightness(ledBrightness);
        prefs.putUInt("led_bright", ledBrightness);
    }
    prefs.putString("led_color", ledColorHex);
    ledMode = "manual";
    prefs.putString("led_mode", "manual");
    
    server.send(200, "application/json", "{\"status\":\"LED color set\"}");
}

void handleApiLedBrightness() {
    if (!server.hasArg("plain")) {
        server.send(400, "application/json", "{\"error\":\"No body\"}");
        return;
    }
    DynamicJsonDocument doc(256);
    deserializeJson(doc, server.arg("plain"));
    
    ledBrightness = doc["brightness"] | ledBrightness;
    FastLED.setBrightness(ledBrightness);
    prefs.putUInt("led_bright", ledBrightness);
    
    server.send(200, "application/json", "{\"status\":\"LED brightness set\"}");
}

void handleApiStatus() {
    DynamicJsonDocument doc(1536);
    
    doc["power"] = systemPowerOn ? "on" : "off";
    
    JsonObject bat = doc.createNestedObject("battery");
    bat["percent"] = 100; // Placeholder
    bat["voltage"] = 4.2;
    bat["charging"] = false;
    bat["full"] = true;
    
    JsonObject pb = doc.createNestedObject("playback");
    pb["playing"] = (!isPaused && currentMediaType != MEDIA_NONE && currentFilename != "");
    pb["paused"] = isPaused;
    pb["filename"] = currentFilename;
    pb["loop"] = loopEnabled;
    
    JsonObject l = doc.createNestedObject("led");
    l["mode"] = ledMode;
    l["brightness"] = ledBrightness;
    l["color"] = ledColorHex;
    
    JsonObject v = doc.createNestedObject("volume");
    v["speaker"] = speakerVolumePct;
    v["exciter"] = exciterVolumePct;

    JsonObject video = doc.createNestedObject("video");
    video["pipeline"] = videoPipelineActive;
    if (videoUseRawFrameStream) {
        video["mode"] = "raw-frame";
    } else if (videoUseHmjStream) {
        video["mode"] = "hmj";
    } else if (videoUseMjpegStream) {
        video["mode"] = "mjpeg";
    } else if (videoUseFileStream) {
        video["mode"] = "color-stream";
    } else {
        video["mode"] = "synthetic";
    }
    video["targetFps"] = videoTargetFps;
    video["decodedFrames"] = videoFramesDecoded;
    video["displayedFrames"] = videoFramesDisplayed;
    video["skippedFrames"] = videoFramesSkipped;
    video["droppedFrames"] = videoFramesDropped;
    video["frameCount"] = videoFrameCount;
    video["queueDepth"] = videoDisplayQueue ? uxQueueMessagesWaiting(videoDisplayQueue) : 0;
    video["freeBuffers"] = videoFreeFrameQueue ? uxQueueMessagesWaiting(videoFreeFrameQueue) : 0;
    video["lastFramePtsMs"] = videoLastFramePtsMs;
    video["audioClockMs"] = videoLastAudioClockMs;
    video["avDriftMs"] = videoAvDriftMs;
    video["decodeLastMs"] = videoDecodeLastMs;
    video["decodeAvgMs"] = videoDecodeAvgMs;
    video["decodeMaxMs"] = videoDecodeMaxMs;
    video["drawLastMs"] = videoDrawLastMs;
    video["drawAvgMs"] = videoDrawAvgMs;
    video["drawMaxMs"] = videoDrawMaxMs;
    video["eof"] = videoStreamEof;
    video["companionAudio"] = videoCompanionAudioActive;
    video["sessionId"] = videoSessionId;
    video["startState"] = videoStart_state;
    video["companionInRam"] = videoCompanionInRam;
    
    doc["uptime"] = millis() / 1000;
    doc["freeHeap"] = ESP.getFreeHeap();
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

void handleApiPowerToggle() {
    systemPowerOn = !systemPowerOn;
    if (!systemPowerOn) {
        AudioCommandMessage cmd = {};
        cmd.cmd = CMD_STOP;
        _enqueueAudioCommand(cmd, 50);
    }
    server.send(200, "application/json", String("{\"state\":\"") + (systemPowerOn ? "on" : "off") + "\"}");
}

void handleApiBattery() {
    // Mocked for MVP
    server.send(200, "application/json", "{\"percent\":100,\"voltage\":4.2,\"charging\":false,\"full\":true}");
}

void handleApiMediaDelete() {
    if (isUploading) {
        server.send(503, "application/json", "{\"error\":\"Upload in progress\"}");
        return;
    }

    if (!server.hasArg("filename")) {
        server.send(400, "application/json", "{\"error\":\"Missing filename query parameter\"}");
        return;
    }
    String fname = server.arg("filename");
    if (!fname.startsWith("/")) fname = "/" + fname;
    
    if (fname == currentFilename && !isPaused && currentMediaType != MEDIA_NONE) {
        server.send(409, "application/json", "{\"error\":\"File is currently playing\"}");
        return;
    }
    
    TickType_t waitTicks = (currentMediaType == MEDIA_VIDEO) ? pdMS_TO_TICKS(20) : portMAX_DELAY;
    if (xSemaphoreTake(sdMutex, waitTicks)) {
        if (SD_MMC.remove(fname)) {
            server.send(200, "application/json", "{\"status\":\"File deleted\"}");
        } else {
            server.send(404, "application/json", "{\"error\":\"File not found or locked\"}");
        }
        xSemaphoreGive(sdMutex);
    } else {
        server.send(503, "application/json", "{\"error\":\"SD busy during playback\"}");
    }
}

void handleApiMediaStorage() {
    DynamicJsonDocument doc(512);

    if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(120)) != pdTRUE) {
        server.send(503, "application/json", "{\"error\":\"SD busy\"}");
        return;
    }

    uint64_t totalBytes = SD_MMC.totalBytes();
    uint64_t usedBytes = SD_MMC.usedBytes();
    xSemaphoreGive(sdMutex);

    if (totalBytes == 0 || usedBytes > totalBytes) {
        server.send(500, "application/json", "{\"error\":\"Failed to query SD storage stats\"}");
        return;
    }

    uint64_t freeBytes = totalBytes - usedBytes;
    doc["total_bytes"] = (uint32_t)totalBytes;
    doc["used_bytes"] = (uint32_t)usedBytes;
    doc["free_bytes"] = (uint32_t)freeBytes;
    doc["is_uploading"] = isUploading;
    doc["upload_received"] = uploadBytesReceived;
    doc["upload_written"] = uploadBytesWritten;
    doc["upload_error"] = uploadHadWriteError;
    if (uploadErrorMessage.length()) {
        doc["upload_error_message"] = uploadErrorMessage;
    }
    if (uploadPartialPath.length()) {
        doc["upload_partial"] = uploadPartialPath;
    }

    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
}

File uploadFile;
uint8_t *uploadWriteScratch = NULL;
size_t uploadBytesWritten = 0;
size_t uploadBytesReceived = 0;
size_t uploadExpectedBytes = 0;
bool uploadHadWriteError = false;
String uploadErrorMessage = "";
String uploadTargetPath = "";
String uploadPartialPath = "";
volatile bool uploadProcessingDone = true;
size_t uploadBytesSinceFlush = 0;

const size_t UPLOAD_WRITE_CHUNK_SIZE = 4096;
const size_t UPLOAD_QUEUE_CHUNK_SIZE = 2048;
const uint8_t UPLOAD_QUEUE_DEPTH = 10;
const uint8_t UPLOAD_MAX_WRITE_RETRIES = 5;
const size_t UPLOAD_PRECHECK_MARGIN_BYTES = 64 * 1024;
const size_t UPLOAD_FLUSH_INTERVAL_BYTES = 256 * 1024;
const TickType_t UPLOAD_SD_LOCK_WAIT_TICKS = pdMS_TO_TICKS(120);
const TickType_t UPLOAD_QUEUE_SEND_WAIT_TICKS = pdMS_TO_TICKS(1000);
const size_t UPLOAD_YIELD_INTERVAL_BYTES = 8 * 1024;

size_t uploadBytesSinceYield = 0;
QueueHandle_t uploadWorkQueue = NULL;
TaskHandle_t uploadWorkerTaskHandle = NULL;

enum UploadWorkType : uint8_t {
    UPLOAD_WORK_START = 0,
    UPLOAD_WORK_DATA = 1,
    UPLOAD_WORK_END = 2,
    UPLOAD_WORK_ABORT = 3,
};

struct UploadWorkItem {
    UploadWorkType type;
    uint32_t totalSize;
    uint16_t dataLen;
    char filename[192];
    uint8_t data[UPLOAD_QUEUE_CHUNK_SIZE];
};

void _uploadYieldIfNeeded(size_t processedBytes = 0) {
    uploadBytesSinceYield += processedBytes;
    if (uploadBytesSinceYield >= UPLOAD_YIELD_INTERVAL_BYTES) {
        uploadBytesSinceYield = 0;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void _setUploadError(const String &msg) {
    if (!uploadHadWriteError) {
        uploadErrorMessage = msg;
    }
    uploadHadWriteError = true;
}

bool _getSdStatsLocked(uint64_t *totalBytes, uint64_t *usedBytes) {
    if (!totalBytes || !usedBytes) return false;
    *totalBytes = 0;
    *usedBytes = 0;
    uint64_t total = SD_MMC.totalBytes();
    uint64_t used = SD_MMC.usedBytes();
    if (total == 0 || used > total) return false;
    *totalBytes = total;
    *usedBytes = used;
    return true;
}

String _formatSdSpaceLocked() {
    uint64_t totalBytes = 0;
    uint64_t usedBytes = 0;
    if (!_getSdStatsLocked(&totalBytes, &usedBytes)) {
        return "sd_space=unknown";
    }
    uint64_t freeBytes = totalBytes - usedBytes;
    return String("sd_free=") + (uint32_t)(freeBytes / 1024ULL) + "KB sd_used=" + (uint32_t)(usedBytes / 1024ULL) + "KB sd_total=" + (uint32_t)(totalBytes / 1024ULL) + "KB";
}

uint32_t _readFileSizeWithRetriesLocked(const String &path, uint8_t retries = 4) {
    uint32_t best = 0;
    for (uint8_t i = 0; i < retries; i++) {
        if (!path.length() || !SD_MMC.exists(path)) {
            if (i < retries - 1) {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            continue;
        }
        File file = SD_MMC.open(path, FILE_READ);
        if (file) {
            uint32_t sz = file.size();
            file.close();
            if (sz > best) {
                best = sz;
            }
        }
        if (i < retries - 1) {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
    return best;
}

size_t _writeUploadChunkLocked(const uint8_t *data, size_t len) {
    if (!data || len == 0 || !uploadFile) return 0;

    uint8_t lockRetries = 0;
    while (xSemaphoreTake(sdMutex, UPLOAD_SD_LOCK_WAIT_TICKS) != pdTRUE) {
        lockRetries++;
        if (lockRetries >= UPLOAD_MAX_WRITE_RETRIES) {
            _setUploadError("Failed to lock SD mutex during chunk write (timeout)");
            return 0;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    size_t totalWritten = 0;
    uint8_t retries = 0;
    while (totalWritten < len) {
        size_t pending = len - totalWritten;
        size_t toWrite = (pending > UPLOAD_WRITE_CHUNK_SIZE) ? UPLOAD_WRITE_CHUNK_SIZE : pending;

        const uint8_t *writePtr = data + totalWritten;
        if (uploadWriteScratch && toWrite <= UPLOAD_WRITE_CHUNK_SIZE) {
            memcpy(uploadWriteScratch, writePtr, toWrite);
            writePtr = uploadWriteScratch;
        }

        size_t justWritten = uploadFile.write(writePtr, toWrite);
        if (justWritten == 0) {
            retries++;
            if (retries >= UPLOAD_MAX_WRITE_RETRIES) {
                _setUploadError(String("SD_MMC write returned 0 repeatedly at offset=") + (uint32_t)totalWritten + " len=" + (uint32_t)len + " " + _formatSdSpaceLocked());
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (justWritten != toWrite) {
            totalWritten += justWritten;
            _setUploadError(String("Chunk short write requested=") + (uint32_t)toWrite + " written=" + (uint32_t)justWritten);
            break;
        }
        totalWritten += justWritten;
        uploadBytesSinceFlush += justWritten;
        if (uploadBytesSinceFlush >= UPLOAD_FLUSH_INTERVAL_BYTES) {
            uploadFile.flush();
            uploadBytesSinceFlush = 0;
        }
        _uploadYieldIfNeeded(justWritten);
        retries = 0;
    }

    xSemaphoreGive(sdMutex);
    return totalWritten;
}

bool _ensureUploadWorker() {
    if (uploadWorkQueue && uploadWorkerTaskHandle) {
        return true;
    }

    if (!uploadWorkQueue) {
        uploadWorkQueue = xQueueCreate(UPLOAD_QUEUE_DEPTH, sizeof(UploadWorkItem));
        if (!uploadWorkQueue) {
            _setUploadError("Failed to create upload queue");
            return false;
        }
    }

    if (!uploadWorkerTaskHandle) {
        if (xTaskCreatePinnedToCore(taskUploadWorker, "UploadWorkerTask", 8192, NULL, PRIO_WEB + 1, &uploadWorkerTaskHandle, CORE_SERVICE) != pdPASS) {
            _setUploadError("Failed to create upload worker task");
            vQueueDelete(uploadWorkQueue);
            uploadWorkQueue = NULL;
            return false;
        }
    }

    return true;
}

bool _enqueueUploadWorkRaw(const void *itemPtr, const char *context) {
    if (!uploadWorkQueue) {
        _setUploadError("Upload queue unavailable");
        return false;
    }

    if (xQueueSend(uploadWorkQueue, itemPtr, UPLOAD_QUEUE_SEND_WAIT_TICKS) != pdTRUE) {
        _setUploadError(String("Upload queue timeout during ") + context);
        return false;
    }

    return true;
}

void taskUploadWorker(void *pvParameters) {
    (void)pvParameters;
    UploadWorkItem item;

    while (true) {
        if (!uploadWorkQueue || xQueueReceive(uploadWorkQueue, &item, portMAX_DELAY) != pdTRUE) {
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        if (item.type == UPLOAD_WORK_START) {
            uploadTargetPath = String(item.filename);
            uploadPartialPath = "/upload.tmp";
            uploadExpectedBytes = item.totalSize;
            uploadBytesWritten = 0;
            uploadBytesReceived = 0;
            uploadBytesSinceFlush = 0;

            if (uploadFile) {
                if (xSemaphoreTake(sdMutex, UPLOAD_SD_LOCK_WAIT_TICKS) == pdTRUE) {
                    uploadFile.close();
                    uploadFile = File();
                    xSemaphoreGive(sdMutex);
                }
            }

            if (!uploadWriteScratch) {
                uploadWriteScratch = (uint8_t*)heap_caps_malloc(UPLOAD_WRITE_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
                if (!uploadWriteScratch) {
                    _setUploadError("Failed to allocate internal SD write scratch buffer");
                }
            }

            if (xSemaphoreTake(sdMutex, UPLOAD_SD_LOCK_WAIT_TICKS) == pdTRUE) {
                uint64_t totalBytes = 0;
                uint64_t usedBytes = 0;
                uint64_t freeBytes = 0;
                uint64_t existingTargetBytes = 0;
                bool statsOk = _getSdStatsLocked(&totalBytes, &usedBytes);
                if (statsOk) {
                    freeBytes = totalBytes - usedBytes;
                }

                if (SD_MMC.exists(uploadTargetPath)) {
                    File existing = SD_MMC.open(uploadTargetPath, FILE_READ);
                    if (existing) {
                        existingTargetBytes = existing.size();
                        existing.close();
                    }
                }

                if (uploadExpectedBytes > 0 && statsOk) {
                    uint64_t projectedNeed = (uint64_t)uploadExpectedBytes + (uint64_t)UPLOAD_PRECHECK_MARGIN_BYTES;
                    uint64_t effectiveFree = freeBytes + existingTargetBytes;
                    if (effectiveFree < projectedNeed) {
                        _setUploadError(String("Insufficient SD space: need ") + (uint32_t)(projectedNeed / 1024ULL) + "KB, free " + (uint32_t)(effectiveFree / 1024ULL) + "KB.");
                    }
                }

                if (SD_MMC.exists(uploadPartialPath)) {
                    SD_MMC.remove(uploadPartialPath);
                }

                if (!uploadHadWriteError) {
                    uploadFile = SD_MMC.open(uploadPartialPath, FILE_WRITE);
                }
                if (!uploadFile && !uploadHadWriteError) {
                    _setUploadError("Failed to open SD file for writing");
                }
                xSemaphoreGive(sdMutex);
            } else {
                _setUploadError("Failed to lock SD mutex for upload start (timeout)");
            }

            continue;
        }

        if (item.type == UPLOAD_WORK_DATA) {
            uploadBytesReceived += item.dataLen;
            if (uploadHadWriteError) {
                continue;
            }
            if (!uploadFile) {
                _setUploadError("Upload file handle is not open");
                continue;
            }
            size_t written = _writeUploadChunkLocked(item.data, item.dataLen);
            uploadBytesWritten += written;
            if (written != item.dataLen) {
                _setUploadError(String("Chunk short write requested=") + (uint32_t)item.dataLen + " written=" + (uint32_t)written);
            }
            continue;
        }

        if (item.type == UPLOAD_WORK_END) {
            if (!uploadFile && !uploadHadWriteError) {
                _setUploadError("Upload ended without valid file handle");
            }

            if (uploadBytesWritten != uploadBytesReceived) {
                _setUploadError("Mismatch between bytes received and bytes written");
            }
            if (uploadExpectedBytes > 0 && uploadBytesWritten != uploadExpectedBytes) {
                _setUploadError("Mismatch between expected size and bytes written");
            }

            if (uploadFile) {
                if (xSemaphoreTake(sdMutex, UPLOAD_SD_LOCK_WAIT_TICKS) == pdTRUE) {
                    uploadFile.flush();
                    uploadFile.close();
                    uploadFile = File();
                    uploadBytesSinceFlush = 0;

                    if (!uploadHadWriteError) {
                        uint32_t partialSize = _readFileSizeWithRetriesLocked(uploadPartialPath);

                        if (partialSize != (uint32_t)uploadBytesWritten) {
                            _setUploadError(String("Partial file size mismatch partial=") + partialSize + " written=" + (uint32_t)uploadBytesWritten);
                        } else {
                            if (uploadTargetPath.length() && SD_MMC.exists(uploadTargetPath)) {
                                SD_MMC.remove(uploadTargetPath);
                            }
                            if (!SD_MMC.rename(uploadPartialPath, uploadTargetPath)) {
                                _setUploadError("Failed to finalize upload rename");
                            } else {
                                uint32_t finalSize = _readFileSizeWithRetriesLocked(uploadTargetPath);
                                if (finalSize != (uint32_t)uploadBytesWritten) {
                                    _setUploadError(String("Final file size mismatch final=") + finalSize + " written=" + (uint32_t)uploadBytesWritten);
                                } else {
                                    uploadPartialPath = "";
                                    uploadTargetPath = "";
                                }
                            }
                        }
                    }

                    xSemaphoreGive(sdMutex);
                } else {
                    _setUploadError("Failed to lock SD mutex during upload finalize (timeout)");
                }
            }

            if (uploadHadWriteError) {
                Serial.printf("Upload End With Errors: expected=%u received=%u written=%u reason=%s partial=%s\n", (uint32_t)uploadExpectedBytes, (uint32_t)uploadBytesReceived, (uint32_t)uploadBytesWritten, uploadErrorMessage.c_str(), uploadPartialPath.c_str());
            } else {
                Serial.printf("Upload End: %u bytes\n", (uint32_t)uploadBytesWritten);
                uploadErrorMessage = "";
            }

            uploadProcessingDone = true;
            isUploading = false;
            continue;
        }

        if (item.type == UPLOAD_WORK_ABORT) {
            if (uploadFile) {
                if (xSemaphoreTake(sdMutex, UPLOAD_SD_LOCK_WAIT_TICKS) == pdTRUE) {
                    uploadFile.close();
                    uploadFile = File();
                    if (uploadPartialPath.length() && SD_MMC.exists(uploadPartialPath)) {
                        SD_MMC.remove(uploadPartialPath);
                    }
                    xSemaphoreGive(sdMutex);
                }
            }

            uploadTargetPath = "";
            uploadPartialPath = "";
            _setUploadError("Upload aborted by client");
            uploadProcessingDone = true;
            isUploading = false;
            if (uploadWorkQueue) {
                xQueueReset(uploadWorkQueue);
            }
            Serial.println("Upload Aborted by client!");
        }
    }
}

void handleApiMediaUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        isUploading = true;
        uploadProcessingDone = false;
        _setUploadTaskSuspension(true);

        // Non-blocking playback quiesce before upload to avoid stalling HTTP stream start
        stopRequested = true;
        mediaSessionId++;
        activeSessionId = 0;
        audioAbortRequested = true;
        if (audioCmdQueue) {
            xQueueReset(audioCmdQueue);
        }
        _resetVideoFrameQueues();
        videoFramesDecoded = 0;
        videoFramesDisplayed = 0;
        videoFramesDropped = 0;
        videoFramesSkipped = 0;
        videoAvDriftMs = 0;
        videoFrameCount = 0;
        videoStart_state = VIDEO_START_IDLE;
        currentMediaType = MEDIA_NONE;
        currentFilename = "";
        loopEnabled = false;
        isPaused = false;
        _uploadYieldIfNeeded(UPLOAD_QUEUE_CHUNK_SIZE);

        // Turn off LEDs immediately to prevent FastLED interrupt starvation during WiFi/SD operation
        FastLED.clear();
        FastLED.show();
        
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        
        Serial.printf("Upload Start: %s\n", filename.c_str());
        uploadBytesWritten = 0;
        uploadBytesReceived = 0;
        uploadExpectedBytes = upload.totalSize;
        uploadBytesSinceFlush = 0;
        uploadBytesSinceYield = 0;
        uploadHadWriteError = false;
        uploadErrorMessage = "";
        uploadTargetPath = filename;
        uploadPartialPath = "/upload.tmp";

        if (!uploadWriteScratch) {
            uploadWriteScratch = (uint8_t*)heap_caps_malloc(UPLOAD_WRITE_CHUNK_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (!uploadWriteScratch) {
                _setUploadError("Failed to allocate internal SD write scratch buffer");
            }
        }

        if (xSemaphoreTake(sdMutex, UPLOAD_SD_LOCK_WAIT_TICKS) == pdTRUE) {
            uint64_t totalBytes = 0;
            uint64_t usedBytes = 0;
            uint64_t freeBytes = 0;
            uint64_t existingTargetBytes = 0;
            bool statsOk = _getSdStatsLocked(&totalBytes, &usedBytes);
            if (statsOk) {
                freeBytes = totalBytes - usedBytes;
            }

            if (SD_MMC.exists(uploadTargetPath)) {
                File existing = SD_MMC.open(uploadTargetPath, FILE_READ);
                if (existing) {
                    existingTargetBytes = existing.size();
                    existing.close();
                }
            }

            if (uploadExpectedBytes > 0 && statsOk) {
                uint64_t projectedNeed = (uint64_t)uploadExpectedBytes + (uint64_t)UPLOAD_PRECHECK_MARGIN_BYTES;
                uint64_t effectiveFree = freeBytes + existingTargetBytes;
                if (effectiveFree < projectedNeed) {
                    _setUploadError(String("Insufficient SD space: need ") + (uint32_t)(projectedNeed / 1024ULL) + "KB, free " + (uint32_t)(effectiveFree / 1024ULL) + "KB.");
                }
            }

            if (SD_MMC.exists(uploadPartialPath)) {
                SD_MMC.remove(uploadPartialPath);
            }

            if (!uploadHadWriteError) {
                uploadFile = SD_MMC.open(uploadPartialPath, FILE_WRITE);
            }
            if (!uploadFile && !uploadHadWriteError) {
                _setUploadError("Failed to open SD file for writing");
            }
            xSemaphoreGive(sdMutex);
        } else {
            _setUploadError("Failed to lock SD mutex for upload start (timeout)");
        }

        if (uploadHadWriteError) {
            uploadProcessingDone = true;
            isUploading = false;
            _setUploadTaskSuspension(false);
            return;
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        _uploadYieldIfNeeded(upload.currentSize);

        uploadBytesReceived += upload.currentSize;

        if (uploadHadWriteError) {
            return;
        }

        if (!uploadFile) {
            _setUploadError("Upload file handle is not open");
            return;
        }

        size_t written = _writeUploadChunkLocked(upload.buf, upload.currentSize);
        uploadBytesWritten += written;
        if (written != upload.currentSize) {
            _setUploadError(String("Chunk short write requested=") + (uint32_t)upload.currentSize + " written=" + (uint32_t)written);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (!uploadFile && !uploadHadWriteError) {
            _setUploadError("Upload ended without valid file handle");
        }

        if (uploadBytesWritten != uploadBytesReceived) {
            _setUploadError("Mismatch between bytes received and bytes written");
        }
        if (uploadExpectedBytes > 0 && uploadBytesWritten != uploadExpectedBytes) {
            _setUploadError("Mismatch between expected size and bytes written");
        }

        if (uploadFile) {
            if (xSemaphoreTake(sdMutex, UPLOAD_SD_LOCK_WAIT_TICKS) == pdTRUE) {
                uploadFile.flush();
                uploadFile.close();
                uploadFile = File();
                uploadBytesSinceFlush = 0;

                if (!uploadHadWriteError) {
                    uint32_t partialSize = _readFileSizeWithRetriesLocked(uploadPartialPath);

                    if (partialSize != (uint32_t)uploadBytesWritten) {
                        _setUploadError(String("Partial file size mismatch partial=") + partialSize + " written=" + (uint32_t)uploadBytesWritten);
                    } else {
                        if (uploadTargetPath.length() && SD_MMC.exists(uploadTargetPath)) {
                            SD_MMC.remove(uploadTargetPath);
                        }
                        if (!SD_MMC.rename(uploadPartialPath, uploadTargetPath)) {
                            _setUploadError("Failed to finalize upload rename");
                        } else {
                            uint32_t finalSize = _readFileSizeWithRetriesLocked(uploadTargetPath);
                            if (finalSize != (uint32_t)uploadBytesWritten) {
                                _setUploadError(String("Final file size mismatch final=") + finalSize + " written=" + (uint32_t)uploadBytesWritten);
                            } else {
                                uploadPartialPath = "";
                                uploadTargetPath = "";
                            }
                        }
                    }
                }

                xSemaphoreGive(sdMutex);
            } else {
                _setUploadError("Failed to lock SD mutex during upload finalize (timeout)");
            }
        }

        if (uploadHadWriteError) {
            Serial.printf("Upload End With Errors: expected=%u received=%u written=%u reason=%s partial=%s\n", (uint32_t)uploadExpectedBytes, (uint32_t)uploadBytesReceived, (uint32_t)uploadBytesWritten, uploadErrorMessage.c_str(), uploadPartialPath.c_str());
        } else {
            Serial.printf("Upload End: %u bytes\n", (uint32_t)uploadBytesWritten);
            uploadErrorMessage = "";
        }

        uploadProcessingDone = true;
        isUploading = false;
        _setUploadTaskSuspension(false);
    } else if (upload.status == UPLOAD_FILE_ABORTED) {
        if (uploadFile && xSemaphoreTake(sdMutex, UPLOAD_SD_LOCK_WAIT_TICKS) == pdTRUE) {
            uploadFile.close();
            uploadFile = File();
            if (uploadPartialPath.length() && SD_MMC.exists(uploadPartialPath)) {
                SD_MMC.remove(uploadPartialPath);
            }
            xSemaphoreGive(sdMutex);
        }

        uploadBytesSinceYield = 0;
        uploadBytesSinceFlush = 0;
        uploadTargetPath = "";
        uploadPartialPath = "";
        _setUploadError("Upload aborted by client");
        uploadProcessingDone = true;
        isUploading = false;
        _setUploadTaskSuspension(false);
        Serial.println("Upload Aborted by client!");
    }
}
