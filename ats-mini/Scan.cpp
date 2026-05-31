#include "Common.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"

// Tuning delays after rx.setFrequency()
#define TUNE_DELAY_DEFAULT 30
#define TUNE_DELAY_FM      60
#define TUNE_DELAY_AM_SSB  80

#define SCAN_POLL_TIME    10 // Tuning status polling interval (msecs)
#define SCAN_POINTS      200 // Number of frequencies to scan

#define SCAN_OFF    0   // Scanner off, no data
#define SCAN_RUN    1   // Scanner running
#define SCAN_DONE   2   // Scanner done, valid data in scanData[]

static struct
{
  uint8_t rssi;
  uint8_t snr;
} scanData[SCAN_POINTS];

static uint32_t scanTime = millis();
static uint8_t  scanStatus = SCAN_OFF;

static uint16_t scanStartFreq;
static uint16_t scanStep;
static uint16_t scanCount;
static uint16_t scanMaxPoints = SCAN_POINTS;
static uint8_t  scanMinRSSI;
static uint8_t  scanMaxRSSI;
static uint8_t  scanMinSNR;
static uint8_t  scanMaxSNR;

static inline uint8_t min(uint8_t a, uint8_t b) { return(a<b? a:b); }
static inline uint8_t max(uint8_t a, uint8_t b) { return(a>b? a:b); }

float scanGetRSSI(uint16_t freq)
{
  // Input frequency must be in range of existing data
  if((scanStatus!=SCAN_DONE) || (freq<scanStartFreq) || (freq>=scanStartFreq+scanStep*scanCount))
    return(0.0);

  uint8_t result = scanData[(freq - scanStartFreq) / scanStep].rssi;
  return((result - scanMinRSSI) / (float)(scanMaxRSSI - scanMinRSSI + 1));
}

float scanGetSNR(uint16_t freq)
{
  // Input frequency must be in range of existing data
  if((scanStatus!=SCAN_DONE) || (freq<scanStartFreq) || (freq>=scanStartFreq+scanStep*scanCount))
    return(0.0);

  uint8_t result = scanData[(freq - scanStartFreq) / scanStep].snr;
  return((result - scanMinSNR) / (float)(scanMaxSNR - scanMinSNR + 1));
}

static void scanInit(uint16_t centerFreq, uint16_t step, uint16_t points)
{
  scanStep      = step;
  scanCount     = 0;
  scanMaxPoints = points<1? 1 : points>SCAN_POINTS? SCAN_POINTS : points;
  scanMinRSSI = 255;
  scanMaxRSSI = 0;
  scanMinSNR  = 255;
  scanMaxSNR  = 0;
  scanStatus  = SCAN_RUN;
  scanTime    = millis();

  const Band *band = getCurrentBand();
  int freq = scanStep * (centerFreq / scanStep - scanMaxPoints / 2);

  // Adjust to band boundaries
  if(freq + scanStep * (scanMaxPoints - 1) > band->maximumFreq)
    freq = band->maximumFreq - scanStep * (scanMaxPoints - 1);
  if(freq < band->minimumFreq)
    freq = band->minimumFreq;
  scanStartFreq = freq;

  // Clear scan data
  memset(scanData, 0, sizeof(scanData));
}

static bool scanTickTime()
{
  // Scan must be on
  if((scanStatus!=SCAN_RUN) || (scanCount>=scanMaxPoints)) return(false);

  // Wait for the right time
  if(millis() - scanTime < SCAN_POLL_TIME) return(true);

  // This is our current frequency to scan
  uint16_t freq = scanStartFreq + scanStep * scanCount;

  // Poll for the tuning status
  rx.getStatus(0, 0);
  if(!rx.getTuneCompleteTriggered())
  {
    scanTime = millis();
    return(true);
  }

  // If frequency not yet set, set it and wait until next call to measure
  if(rx.getCurrentFrequency() != freq)
  {
    rx.setFrequency(freq); // Implies tuning delay
    scanTime = millis() - SCAN_POLL_TIME;
    return(true);
  }

  // Measure RSSI/SNR values
  rx.getCurrentReceivedSignalQuality();
  scanData[scanCount].rssi = rx.getCurrentRSSI();
  scanData[scanCount].snr  = rx.getCurrentSNR();

  // Measure range of values
  scanMinRSSI = min(scanData[scanCount].rssi, scanMinRSSI);
  scanMaxRSSI = max(scanData[scanCount].rssi, scanMaxRSSI);
  scanMinSNR  = min(scanData[scanCount].snr, scanMinSNR);
  scanMaxSNR  = max(scanData[scanCount].snr, scanMaxSNR);

  // Next frequency to scan
  freq += scanStep;

  // Set next frequency to scan or expire scan
  if((++scanCount >= scanMaxPoints) || !isFreqInBand(getCurrentBand(), freq) || consumeAbortPending())
    scanStatus = SCAN_DONE;
  else
    rx.setFrequency(freq); // Implies tuning delay

  // Save last scan time
  scanTime = millis() - SCAN_POLL_TIME;

  // Return current scan status
  return(scanStatus==SCAN_RUN);
}

//
// Run entire scan once
//
void scanRun(uint16_t centerFreq, uint16_t step)
{
  // Set tuning delay
  rx.setMaxDelaySetFrequency(currentMode == FM ? TUNE_DELAY_FM : TUNE_DELAY_AM_SSB);
  // Mute the audio
  muteOn(MUTE_TEMP, true);
  // Flag is set by rotary encoder and cleared on seek/scan entry
  seekStop = false;
  // Save current frequency
  uint16_t curFreq = rx.getFrequency();
  // Scan the whole range
  for(scanInit(centerFreq, step, SCAN_POINTS) ; scanTickTime(););
  // Restore current frequency
  rx.setFrequency(curFreq);
  // Unmute the audio
  muteOn(MUTE_TEMP, false);
  // Restore tuning delay
  rx.setMaxDelaySetFrequency(TUNE_DELAY_DEFAULT);
}

//
// Cooperative remote sweep. Unlike scanRun(), the sweep is not run in a blocking
// loop: scanRemoteStart() sets it up and scanRemoteTick() advances it one step
// per main-loop iteration, so the radio's UI and I/O stay alive during the sweep.
// While scanRemoteActive() the main loop must skip its periodic tuner/display work
// (RSSI/squelch, RDS, ...) so it does not contend with the scan.
//
static Stream*  scanRemoteStream = nullptr;
static uint16_t scanRemoteSavedFreq = 0;

//
// Stream the completed sweep to the remote:
//
//   P<startFreq>,<step>,<count>\r\n   - ASCII header line
//   <count * 2-byte hex rssi,snr>     - hex blob, wrapped at 32 points per line
//
// startFreq/step are in the band's internal units (FM = 10 kHz, AM/SSB = 1 kHz),
// count is the number of points actually measured (may be < requested at a band
// edge or on abort). rssi/snr are the raw 0..127 values from the SI473x.
//
static void scanRemoteEmit(Stream* stream)
{
  stream->printf("\r\nP%u,%u,%u\r\n", scanStartFreq, scanStep, scanCount);
  for(uint16_t i=0 ; i<scanCount ; i++)
  {
    stream->printf("%02x%02x", scanData[i].rssi, scanData[i].snr);
    if((i & 31) == 31) stream->println("");
  }
  stream->println("");
  stream->flush();
}

bool scanRemoteActive()
{
  return scanRemoteStream != nullptr;
}

//
// Begin a sweep centered on the current frequency, streaming the result to
// `stream` once it completes. Returns immediately; the sweep is advanced by
// scanRemoteTick(). Rejected (with a short error) if a scan is already running.
//
void scanRemoteStart(Stream* stream, uint16_t step, uint16_t points)
{
  // One sweep at a time (also blocks while the on-device scan is running)
  if(scanRemoteStream || (scanStatus == SCAN_RUN))
  {
    stream->println("\r\nError: Scan busy");
    return;
  }

  // Sanitize parameters (scanInit clamps points, but guard step too)
  if(step < 1) step = 1;

  // Set tuning delay
  rx.setMaxDelaySetFrequency(currentMode == FM ? TUNE_DELAY_FM : TUNE_DELAY_AM_SSB);
  // Mute the audio
  muteOn(MUTE_TEMP, true);
  // Flag is set by rotary encoder and cleared on seek/scan entry
  seekStop = false;
  // Save current frequency for restore on completion
  scanRemoteSavedFreq = rx.getFrequency();
  // Set up the scan centered on the current frequency
  scanInit(currentFrequency, step, points);
  scanRemoteStream = stream;

  // Show an indicator so the frozen-looking screen reads as intentional
  drawScreen();
  drawMessage("Remote scan...");
}

//
// Advance an in-flight remote sweep by one step. Call once per main loop.
//
void scanRemoteTick()
{
  if(!scanRemoteStream) return;

  // Still running?
  if(scanTickTime()) return;

  // Done (or aborted): stream the result and restore radio state
  scanRemoteEmit(scanRemoteStream);
  rx.setFrequency(scanRemoteSavedFreq);
  muteOn(MUTE_TEMP, false);
  rx.setMaxDelaySetFrequency(TUNE_DELAY_DEFAULT);
  scanRemoteStream = nullptr;

  // Restore the normal screen now (the main loop has no event to trigger it)
  drawScreen();
}
