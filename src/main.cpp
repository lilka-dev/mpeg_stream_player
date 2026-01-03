/*
 * MJPEG Stream Receiver for Lilka v2 (ST7789 280x240)
 * Receives raw MJPEG stream over TCP and displays frames in real-time using
 * JPEG decoding via the TJpg_Decoder library.
 * 
 * Designed to be loaded from KeiraOS:
 * - Reads WiFi credentials from Keira's NVS storage (namespace "kwifi")
 * - Uses the same SSID hashing scheme as Keira for password retrieval
 * - No interactive WiFi configuration - credentials must be set in Keira first
 * 
 * Protocol: Raw MJPEG stream
 *   Frames are detected by JPEG SOI (0xFFD8) and EOI (0xFFD9) markers
 *   Compatible with GStreamer jpegenc output via tcpclientsink
 *
 * GStreamer pipeline example:
 *   gst-launch-1.0 ximagesrc ! videoscale ! video/x-raw,width=280,height=240 \
 *     ! videorate ! video/x-raw,framerate=15/1 ! jpegenc quality=50 \
 *     ! tcpclientsink host=<ESP_IP> port=8090
 *
 * Performance optimizations:
 * - TJpgDec library for efficient JPEG decoding on ESP32
 * - PSRAM buffer for JPEG data (supports frames up to ~100KB)
 * - Direct RGB565 output to display
 * - TCP with no-delay for low latency streaming
 */

#include <Arduino.h>
#include <lilka.h>
#include <WiFi.h>
#include <WiFiServer.h>
#include <esp_heap_caps.h>
#include <TJpg_Decoder.h>
#include "wifi_config.h"

// Display dimensions
#define DISPLAY_WIDTH  280
#define DISPLAY_HEIGHT 240

// Network settings
WiFiServer server(8090);
WiFiClient client;

// JPEG markers
const uint8_t JPEG_SOI[2] = {0xFF, 0xD8};  // Start of Image
const uint8_t JPEG_EOI[2] = {0xFF, 0xD9};  // End of Image

// JPEG buffer (allocate in PSRAM for larger frames)
const size_t MAX_JPEG_SIZE = 100 * 1024;  // 100KB max frame size
uint8_t* jpegBuffer = nullptr;
size_t jpegBufferPos = 0;

// Ring buffer for incoming data
const size_t RECV_BUFFER_SIZE = 32 * 1024;
uint8_t* recvBuffer = nullptr;
size_t recvBufferPos = 0;

// Stats
unsigned long frameCount = 0;
unsigned long lastStats = 0;
uint32_t frameId = 0;
unsigned long totalBytesReceived = 0;
unsigned long decodeTimeMs = 0;

// TJpgDec callback - outputs directly to display using Arduino_GFX method
bool tjpgd_output(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  // Clip to display bounds
  if (x >= DISPLAY_WIDTH || y >= DISPLAY_HEIGHT) return true;
  
  uint16_t drawW = w;
  uint16_t drawH = h;
  
  if (x + w > DISPLAY_WIDTH) drawW = DISPLAY_WIDTH - x;
  if (y + h > DISPLAY_HEIGHT) drawH = DISPLAY_HEIGHT - y;
  
  // Use Arduino_GFX fast bitmap drawing
  lilka::display.draw16bitRGBBitmap(x, y, bitmap, drawW, drawH);
  
  return true;  // Continue decoding
}

// Allocate buffers in PSRAM
bool allocateBuffers() {
  // Allocate JPEG receive buffer
  jpegBuffer = (uint8_t*)ps_malloc(MAX_JPEG_SIZE);
  if (!jpegBuffer) {
    jpegBuffer = (uint8_t*)malloc(MAX_JPEG_SIZE);
  }
  if (!jpegBuffer) {
    Serial.println("Failed to allocate JPEG buffer");
    return false;
  }
  
  // Allocate receive buffer
  recvBuffer = (uint8_t*)ps_malloc(RECV_BUFFER_SIZE);
  if (!recvBuffer) {
    recvBuffer = (uint8_t*)malloc(RECV_BUFFER_SIZE);
  }
  if (!recvBuffer) {
    Serial.println("Failed to allocate receive buffer");
    free(jpegBuffer);
    jpegBuffer = nullptr;
    return false;
  }
  
  Serial.printf("Buffers allocated: JPEG=%dKB, Recv=%dKB\n", 
                MAX_JPEG_SIZE / 1024, 
                RECV_BUFFER_SIZE / 1024);
  
  return true;
}

bool readExactly(WiFiClient& c, uint8_t* dst, size_t len) {
  size_t got = 0;
  unsigned long startTime = millis();
  const unsigned long timeout = 5000;  // 5 second timeout
  
  while (got < len && c.connected()) {
    if (millis() - startTime > timeout) {
      Serial.printf("Read timeout: got %d/%d bytes\n", got, len);
      return false;
    }
    
    int available = c.available();
    if (available > 0) {
      int toRead = min((size_t)available, len - got);
      int chunk = c.read(dst + got, toRead);
      if (chunk > 0) {
        got += chunk;
        startTime = millis();  // Reset timeout on successful read
      }
    } else {
      delay(1);  // Allow other tasks
    }
  }
  return got == len;
}

// Find JPEG frame in buffer, returns frame size or 0 if not found
size_t findJpegFrame(uint8_t* buffer, size_t len, size_t* frameStart) {
  // Look for SOI marker (0xFFD8)
  for (size_t i = 0; i < len - 1; i++) {
    if (buffer[i] == 0xFF && buffer[i + 1] == 0xD8) {
      *frameStart = i;
      // Look for EOI marker (0xFFD9)
      for (size_t j = i + 2; j < len - 1; j++) {
        if (buffer[j] == 0xFF && buffer[j + 1] == 0xD9) {
          return j + 2 - i;  // Frame size including markers
        }
      }
      return 0;  // SOI found but no EOI yet
    }
  }
  return 0;  // No SOI found
}

// Display waiting screen with IP address and status message
void showWaitingScreen() {
  lilka::display.fillScreen(lilka::colors::Black);
  int16_t x1, y1;
  uint16_t w, h;
  
  lilka::display.setTextSize(1);
  lilka::display.setTextColor(lilka::colors::White);
  lilka::display.getTextBounds("MJPEG Receiver", 0, 0, &x1, &y1, &w, &h);
  lilka::display.setCursor((lilka::display.width() - w) / 2, 60);
  lilka::display.println("MJPEG Receiver");
  
  lilka::display.getTextBounds("IP Address:", 0, 0, &x1, &y1, &w, &h);
  lilka::display.setCursor((lilka::display.width() - w) / 2, 100);
  lilka::display.println("IP Address:");
  
  String ipStr = WiFi.localIP().toString();
  lilka::display.setTextSize(1);
  lilka::display.setTextColor(lilka::colors::Green);
  lilka::display.getTextBounds(ipStr.c_str(), 0, 0, &x1, &y1, &w, &h);
  lilka::display.setCursor((lilka::display.width() - w) / 2, 125);
  lilka::display.println(ipStr);
  
  lilka::display.setTextColor(lilka::colors::Cyan);
  lilka::display.getTextBounds("Port: 8090", 0, 0, &x1, &y1, &w, &h);
  lilka::display.setCursor((lilka::display.width() - w) / 2, 150);
  lilka::display.println("Port: 8090");
  
  lilka::display.setTextSize(1);
  lilka::display.setTextColor(lilka::colors::Yellow);
  lilka::display.getTextBounds("Waiting for stream...", 0, 0, &x1, &y1, &w, &h);
  lilka::display.setCursor((lilka::display.width() - w) / 2, 210);
  lilka::display.println("Waiting for stream...");
}

void setup() {
  // Initialize Lilka (display, buttons, SD card, etc.)
  lilka::begin();
  lilka::display.fillScreen(lilka::colors::Black);

  Serial.println("MJPEG Stream Receiver starting...");

  // Initialize TJpgDec
  TJpgDec.setJpgScale(1);  // No scaling - image should already be 280x240
  TJpgDec.setSwapBytes(false);  // Don't swap bytes - Arduino_GFX handles byte order
  TJpgDec.setCallback(tjpgd_output);
  
  // Allocate buffers
  if (!allocateBuffers()) {
    lilka::Alert alert(
      "Memory Error",
      "Failed to allocate buffers.\n\nPSRAM may not be available.\n\nPress A to restart."
    );
    alert.draw(&lilka::display);
    while (!alert.isFinished()) {
      alert.update();
    }
    ESP.restart();
  }

  // Load WiFi credentials from Keira's NVS storage
  String ssid, password;
  if (!loadWiFiCredentials(ssid, password)) {
    lilka::Alert alert(
      "WiFi Error",
      "No WiFi configured.\n\nPlease configure WiFi in Keira first.\n\nPress A to restart."
    );
    alert.draw(&lilka::display);
    while (!alert.isFinished()) {
      alert.update();
    }
    ESP.restart();
  }
  
  Serial.printf("Found WiFi credentials for: %s\n", ssid.c_str());
  
  // Connect to WiFi
  if (!connectToWiFi(ssid, password)) {
    lilka::Alert alert(
      "Connection Failed",
      "Failed to connect to WiFi.\n\nCheck credentials in Keira.\n\nPress A to restart."
    );
    alert.draw(&lilka::display);
    while (!alert.isFinished()) {
      alert.update();
    }
    ESP.restart();
  }

  showWaitingScreen();

  server.begin();
  server.setNoDelay(true);
  Serial.println("MJPEG server listening on port 8090");
}

bool handleClient() {
  // Accept new client
  if (!client || !client.connected()) {
    client = server.available();
    if (client) {
      Serial.println("Client connected - MJPEG stream starting");
      client.setNoDelay(true);
      client.setTimeout(100);
      frameCount = 0;
      totalBytesReceived = 0;
      decodeTimeMs = 0;
      lastStats = millis();
      jpegBufferPos = 0;
    }
  }

  if (!client || !client.connected()) {
    return false;
  }

  // Read available data into JPEG buffer
  int available = client.available();
  if (available > 0) {
    size_t toRead = min((size_t)available, MAX_JPEG_SIZE - jpegBufferPos);
    if (toRead > 0) {
      int bytesRead = client.read(jpegBuffer + jpegBufferPos, toRead);
      if (bytesRead > 0) {
        jpegBufferPos += bytesRead;
        totalBytesReceived += bytesRead;
      }
    }
  }

  // Look for complete JPEG frame
  if (jpegBufferPos >= 4) {
    size_t frameStart = 0;
    size_t frameSize = findJpegFrame(jpegBuffer, jpegBufferPos, &frameStart);
    
    if (frameSize > 0) {
      // Found complete frame, decode it
      unsigned long decodeStart = millis();
      
      JRESULT res = TJpgDec.drawJpg(0, 0, jpegBuffer + frameStart, frameSize);
      
      decodeTimeMs += millis() - decodeStart;
      
      if (res != JDR_OK) {
        Serial.printf("JPEG decode error: %d (frame size: %d)\n", res, frameSize);
      } else {
        frameCount++;
        frameId++;
      }
      
      // Remove processed frame from buffer
      size_t frameEnd = frameStart + frameSize;
      if (frameEnd < jpegBufferPos) {
        memmove(jpegBuffer, jpegBuffer + frameEnd, jpegBufferPos - frameEnd);
        jpegBufferPos -= frameEnd;
      } else {
        jpegBufferPos = 0;
      }
    }
  }

  // Prevent buffer overflow - discard old data if buffer is getting full
  if (jpegBufferPos > MAX_JPEG_SIZE - 1024) {
    Serial.println("Buffer overflow, resetting");
    jpegBufferPos = 0;
  }

  // Print stats every 2 seconds
  unsigned long now = millis();
  if (now - lastStats >= 2000) {
    float elapsed = (now - lastStats) / 1000.0f;
    float fps = frameCount / elapsed;
    float bandwidth = (totalBytesReceived * 8.0f) / (elapsed * 1000.0f);  // kbps
    float avgDecode = (frameCount > 0) ? (float)decodeTimeMs / frameCount : 0;
    
    Serial.printf("FPS: %.1f | Bandwidth: %.1f kbps | Avg decode: %.1fms | Frames: %u\n",
                  fps, bandwidth, avgDecode, frameId);
    
    frameCount = 0;
    totalBytesReceived = 0;
    decodeTimeMs = 0;
    lastStats = now;
  }

  return true;
}

void loop() {
  handleClient();
  
  if (client && !client.connected()) {
    Serial.println("Client disconnected");
    jpegBufferPos = 0;  // Reset buffer
    showWaitingScreen();
  }
  
  delay(1);
}