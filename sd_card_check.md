#include "FS.h"
#include "SD.h"
#include "SPI.h"

// Define your SD Chip Select pin (Must be different from TFT_CS)
#define SD_CS 10

// Function to list directories
static void listDir(fs::FS &fs, const char * dirname, uint8_t levels);

void setup() {
  Serial.begin(115200);
  delay(3000);
  Serial.println("\n--- ESP32-S3 SD CARD TEST ---");

  // 1. Manually start SPI on your verified pins
  // SCK=12, MISO=13, MOSI=11
  SPI.begin(12, 13, 11);

  // 2. Initialize SD Card
  if (!SD.begin(SD_CS)) {
    Serial.println("Card Mount Failed! Check wiring and SD_CS pin.");
    return;
  }

  // 3. Get Card Info
  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("No SD card attached");
    return;
  }

  Serial.print("SD Card Type: ");
  if (cardType == CARD_MMC) Serial.println("MMC");
  else if (cardType == CARD_SD) Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("UNKNOWN");

  uint64_t cardSize = SD.cardSize() / (1024ULL * 1024ULL);
  Serial.printf("SD Card Size: %llu MB\n", (unsigned long long)cardSize);

  // 4. List Files
  listDir(SD, "/", 0);

  Serial.println("\n--- TEST COMPLETE ---");
}

void loop() {
  // Idle
}

static void listDir(fs::FS &fs, const char * dirname, uint8_t levels) {
  Serial.printf("Listing directory: %s\n", dirname);
  File root = fs.open(dirname);
  if (!root) {
    Serial.println("Failed to open directory");
    return;
  }
  if (!root.isDirectory()) {
    Serial.println("Not a directory");
    root.close();
    return;
  }

  File file = root.openNextFile();
  while (file) {
    if (file.isDirectory()) {
      Serial.print("  DIR : ");
      Serial.println(file.name());
      if (levels) listDir(fs, file.name(), levels - 1);
    } else {
      Serial.print("  FILE: ");
      Serial.print(file.name());
      Serial.print("  SIZE: ");
      Serial.println((unsigned)file.size());
    }
    file = root.openNextFile();
  }
  root.close();
}
