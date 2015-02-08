/*
 * Midi-file-playing Glockenspiel.
 * 
 * Copyright (c) 2014, 2015 Bradford Needham
 * (@bneedhamia, https://www.needhamia.com)
 *
 * Licensed under The MIT License (MIT),
 * a copy of which should have been supplied with this file.
 */

/*
 * This sketch requires an Arduino Mega 2560 Rev 3,
 * a Sparkfun TransmogriShield on it
 * (https://www.sparkfun.com/products/11469)
 * a Sparkfun Wifi CC3000 Shield on top of that
 * (https://www.sparkfun.com/products/12071)
 * and 19 solenoids
 * (https://www.sparkfun.com/products/11015 part ZHO-0420S-05A4.5)
 * which strike chimes made of 1/2" metal electrical conduit.
 * It further requires a formatted Micro SD card
 * plugged into the Wifi Shield.
 *
 * See glockenspiel.fzz for the circuit diagram.
 * Solenoid power is connected to the Arduino Vin.
 * It requires the Arduino be powered by a 9V wall supply.
 *
 * For development, connect both a 9V supply
 * and the USB cable.
 * Without the 9V supply, the solenoids won't play.
 *
 * To operate: XXX add instructions.
 *
 * NOTE: The solenoid can dissipate no more than 1.2 watts continuously.
 * At 4.5ohm solenoid resistance, and 9V solenoid supply (5V is too weak),
 * anything over an average 6% duty cycle will burn out the solenoid.
 */

#include <SD.h>
#include <SPI.h>
#include <SFE_CC3000.h>
#include <SFE_CC3000_Client.h>
#include <SDConfigFile.h> // https://github.com/bneedhamia/sdconfigfile
#include <MidiFileStream.h> // https://github.com/bneedhamia/midifilestream

/*
 * Pins:
 *
 * Wifi Shield Pins:
 * pinWifiInt = interrupt pin for Wifi Shield
 * pinWifiEnable = enable pin for with Wifi Shield
 * pinSelectWifi = the CC3000 chip select pin.
 * pinSelectSD = the SD chip select pin.
 *
 * pinNoteOffset[] = the pin number of a solenoid that plays a chime.
 *  Indexed by the offset from MIN_NOTE_NUM. That is:
 *    pin = pinNoteOffset[midiNoteNum - MIN_NOTE_NUM];
 * MIN_NOTE_NUM = the MIDI note number of the lowest-pitched chime.
 * NUM_NOTE_PINS = the number of consecutive MIDI notes we support.
 *    That is, the number of chimes we have.
 *
 * pinButtonOff = the On/Off button. Internal pullup, so LOW = On; HIGH = Off.
 * pinLedIsOn = the On/Off state LED. Lighted when not stopped.
 * pinButtonPause = the Play/Pause button. Internal pullup.
 * pinLedPlaying = the Playing/Paused state. Lighted when playing; blinking when paused.
 *
 * Pins 50-53 are the SPI bus.
 */

const int pinWifiInt = 2;
const int pinWifiEnable = 7;
const int pinSelectWifi = 10;
const int pinSelectSD = 8;

const int MIN_NOTE_NUM = 72; // Midi note 72 corresponds to C5
const int pinNoteOffset[] = {
  23, 25, 27, 29, 31, 33, 35, 37, 39, 41,
  22, 24, 26, 28, 30, 32, 34, 36, 38
};
const int NUM_NOTE_PINS = sizeof(pinNoteOffset) / sizeof(pinNoteOffset[0]);

const int pinButtonOff = A14; // Digital Input
const int pinLedIsOn = A15;     // Digital Output
const int pinButtonPause = 48; // Digital Input
const int pinLedPlaying = 49; // Digital Output

/*
 * time per solenoid actuation, in milliseconds.
 * Too large a number here can burn out the solenoids.
 */
const int MS_PER_STRIKE = 3;

/*
 * maximum line length in any file we deal with.
 * A line longer than this will cause the file reading to fail.
 * The larger this number is, the more memory is consumed.
 */
const uint8_t MAX_LINE_LENGTH = 128;

/*
 * The name of our configuration file on the SD card.
 * See the example SD/glocken.cfg for its format.
 */
const char *CONFIG_FILE = "glocken.cfg";

/*
 * The name of the playlist we'll create from
 * the original, net playlist.
 * This tmp playlist will refer only to SD files (not urls).
 * We use this instead of an in-memory playlist to save memory.
 */
const char *SD_TMP_PLAYLIST = "playlist.tmp";

/*
 * The maximum time (microseconds) we're willing to wait
 * using the delayMicroseconds() call.
 * Note: the maximum parameter value
 * for delayMicroseconds() is 16383.
 *
 * For delays greater than MAX_MICROS_TO_DELAY
 * we simply return from loop() and wait for another
 * call to loop() to occur.
 */
const long MAX_MICROS_TO_DELAY = 11 * 1000L;

/*
 * Values for our player state.
 * STATE_ERROR = a fatal error has occurred. The player needs to be reset.
 * STATE_STOPPED = playback is stopped. The player is idle.
 * STATE_END_FILE = previous file has ended. Need to choose a new file to play.
 * STATE_END_TRACK = previous track has ended. Need to open the next track.
 * STATE_EVENTS = reading events. Need to read the next event.
 * STATE_WAITING = waiting to play one or more simultaneous events from the queue.
 */
const char STATE_ERROR = (char) 0;
const char STATE_STOPPED = (char) 1;
const char STATE_END_FILE = (char) 2;
const char STATE_END_TRACK = (char) 3;
const char STATE_EVENTS = (char) 4;
const char STATE_WAITING = (char) 5;

boolean readConfiguration();
boolean transformSDPlaylist();
boolean getNextFilename();
char *cacheFile(const char *url);

char *playlistUrl;       // (malloc()ed) url of the playlist (could be file://)
char *wifiSsid;          // (malloc()ed) SSID of the network to connect to, or null if no net.
char *wifiPassword;      // (malloc()ed) password of the network, or null if no password.

char *playlistSDName;    // (malloc()ed) local SD card name of the playlist file.
uint8_t numPlaylistTitles; // number of titles in the temporary playlist.
uint8_t *playOrder;      // (malloc()ed) array of track numbers, size numPlaylistTitles.
                         //   (line numbers in the tmp playlist) to play,
                         //   in order of play.  E.g., if not shuffled, [0] = 0, [1] = 1, etc.
uint8_t nowPlayingIdx;   // if not 255, index into playOrder[] of the title we're currently playing
char *playingFname;      // SD filename of the file being played.
MidiFileStream midiFile; // The current Midi file being played.
File playingFile;        // the underlying SD File.

char state;              // State of our file-playing machine. See STATE_*.
uint8_t ledOnState;      // State of our On/Off indicator LED. HIGH = on; LOW = off.

long microsPerTick = 1;   // current tempo, in microseconds per tick.

/*
 * For performance measurement,
 * maxEventReadMillis = the maximum time (milliseconds) it took to read an event from SD.
 * maxMicrosLate = the maximum time (microseconds) that we're late in playing an event.
 */
unsigned long maxEventReadMillis = 0;
unsigned long maxMicrosLate = 0;

/*
 * Time returned from micros() when we started playing the current file.
 * Note: micros() will overflow about every 70 minutes that the Sketch is running,
 * so we must be careful in calculating differences from this time:
 *   unsigned long microsSinceStart = micros() - startMicros;
 * Note: because our time since start will also overflow at about 70 minutes,
 * we can't play a piece that plays longer than about 70 minutes.
 * 
 */
unsigned long startMicros;

/*
 * the time, in microseconds, between startMicros
 * and the most-recently-queued event.
 */
unsigned long microsSinceStart;

/*
 * The queue of notes to play simultaneously.
 * The maximum is the number of distinct notes we can play.
 *
 * queue[] = Midi note numbers waiting to be played simultaneously.
 * numQueued = the number of Midi Note On events in queue[].
 *  0 == nothing is queued.
 */
const int MAX_QUEUED_EVENTS = NUM_NOTE_PINS;
int queue[MAX_QUEUED_EVENTS];
int numQueued;

/*
 * If non-zero, the current event has been 'pushed back' into the stream.
 * That is, when it's time to read another event,
 * just use the current event instead of reading a new one.
 */
char isPushedBack;

void setup() {
  int i;
 
  Serial.begin(115200);
  Serial.println("Reset.");

  Ram_TableDisplay(); //Debug

  pinMode(pinSelectSD, OUTPUT);
  for (i = 0; i < NUM_NOTE_PINS; ++i) {
    pinMode(pinNoteOffset[i], OUTPUT);
  }
  
  pinMode(pinButtonOff, INPUT);
  digitalWrite(pinButtonOff, HIGH);   // enable internal pull-up resistor
  pinMode(pinLedIsOn, OUTPUT);
  
  pinMode(pinButtonPause, INPUT);
  digitalWrite(pinButtonPause, HIGH);   // enable internal pull-up resistor
  pinMode(pinLedPlaying, OUTPUT);
  
  state = STATE_ERROR;
  playlistUrl = 0;
  wifiSsid = 0;
  wifiPassword = 0;
  playlistSDName = 0;
  
  if (!SD.begin(pinSelectSD)) {
    Serial.println("SD.begin() failed. Check card insertion.");
    return;
  }
  
  // Exercise each solenoid twice, because they seem to be not quite charged on startup.
  for (i = 0; i < NUM_NOTE_PINS; ++i) {
    digitalWrite(pinNoteOffset[i], HIGH);
    delay(MS_PER_STRIKE);
    digitalWrite(pinNoteOffset[i], LOW);
    delay(MS_PER_STRIKE);
    delay(125);
  }
  delay(500);
  for (i = 0; i < NUM_NOTE_PINS; ++i) {
    digitalWrite(pinNoteOffset[i], HIGH);
    delay(MS_PER_STRIKE);
    digitalWrite(pinNoteOffset[i], LOW);
    delay(MS_PER_STRIKE);
    delay(125);
  }
  delay(2000); //XXX remove once the initial state = stopped.
  
  //XXX goes in the to be written stop -> play state transition.
  
  readConfiguration();
  Serial.print("Playlist: ");
  Serial.println(playlistUrl);
  
  if (playlistSDName != 0) {
    free(playlistSDName);
  }
  playlistSDName = cacheFile(playlistUrl);
  if (!playlistSDName) {
    Serial.print("Cacheing ");
    Serial.print(playlistUrl);
    Serial.print(" failed. Press stop and start");
    state = STATE_ERROR;
    return;
  }
  
  if (!transformSDPlaylist()) {
    Serial.print("Failed to read playlist: ");
    Serial.println(playlistSDName);
  }
  
  setPlayOrder(); //XXX do this whenever the playlist runs out - that is, we always loop.
  nowPlayingIdx = 255;
  
  state = STATE_END_FILE;
  ledOnState = LOW;

}

void loop() {
  chunk_t chunkType;
  event_t eventType;
  int bLeft;
  long uSecs;
  long microsToWait;
  
  //XXX Check any buttons.
  
  switch (state) {
  
  case STATE_ERROR:
    /*
    //XXX Rewrite this, because the SPI bus seems to use pin 13.
    // Show an error by blinking the LED
    if ((millis() % 1000) < 500) {
      digitalWrite(pinLed, HIGH);
    } else {
      digitalWrite(pinLed, LOW);
    }
    */
    break;
    
  case STATE_STOPPED:
    // Nothing to do.
    break;
    
  case STATE_END_FILE:
    if (!getNextFilename()) {
      
      // Report our performance numbers
      Serial.print("Maximum event-reading delay = ");
      Serial.print(maxEventReadMillis);
      Serial.println(" ms");
      
      Serial.print("Maximum event-playing lateness = ");
      Serial.print(maxMicrosLate);
      Serial.print(" us");
      
      state = STATE_STOPPED;  // no next file to play.
    } else {       
      playingFile = SD.open(playingFname, FILE_READ);
      if (!playingFile) {
        Serial.print("failed to open file ");
        Serial.println(playingFname);
        
        state = STATE_END_FILE; // skip to the next file
      } else {
        if (!midiFile.begin(playingFile)) {
          Serial.print("failed to open midi file: ");
          Serial.println(playingFname);
          midiFile.end();
          playingFile.close();
                   
          state = STATE_END_FILE;  // skip to the next file.
        } else {
          // Now is the start of playback for this track.
          startMicros = micros();
          microsSinceStart = 0;
          isPushedBack = false;
          numQueued = 0;
 
          state = STATE_END_TRACK;  // ready to open a track.
        }
      }
    }
    break;
    
  case STATE_END_TRACK:

    chunkType = midiFile.openChunk();
    if (chunkType != CT_MTRK) {
      if (chunkType != CT_END) {
        Serial.print("Expected chunk type CT_MTRK, instead read type ");
        Serial.println((int) chunkType);
        // fallthrough
      }
      
      // Skip to the next file.
      midiFile.end();
      playingFile.close();
      
      state = STATE_END_FILE;
      break;
    }
    
    // Track is open.
//    Serial.print("Track length = ");
//    Serial.println(midiFile.getChunkBytesLeft());

    state = STATE_EVENTS;
    break;
    
  case STATE_EVENTS:
  
    /*
     * If we have read an event we need to reprocess,
     * use that.
     * Otherwise, read the next event.
     */
     
    if (isPushedBack) {
      isPushedBack = false;
      eventType = midiFile.getEventType();
    } else {
      unsigned long preMillis = millis();
      eventType = midiFile.readEvent();
      unsigned long delayMillis = millis() - preMillis;
      if (maxEventReadMillis < delayMillis) {
        maxEventReadMillis = delayMillis;
      }
    }
    if (eventType == ET_END) {
      // No more events in this track. Normal end of a track.
      if (numQueued > 0) {
        // The queue should have been flushed by an ET_END_TRACK just before this.
        Serial.print("Warning: track ended with ");
        Serial.print(numQueued);
        Serial.println(" notes queued.");
      }
      
      state = STATE_END_TRACK;
      break;
    }
    if (eventType == ET_UNK) {
      Serial.println("Error reading event.");
      // Skip to the next file.
      midiFile.end();
      playingFile.close();
      
      state = STATE_END_FILE;
      break;
    }

    /*
     * Calculate the delay from
     * the previous event to this event
     */
     
    uSecs = midiFile.getEventDeltaTicks() * microsPerTick;
        
    /*
     * If this event isn't simultaneous with the previous one
     * and we have some queued events to play,
     * delay until the right time to play those queued events.
     */
    if (uSecs != 0 && numQueued > 0) {
      isPushedBack = true;
      state = STATE_WAITING;
      break;
    }
    
    /*
     * Either the current event is simultanous with the previous one,
     * or there's nothing queued.
     * In either case,
     * Update the time of this event relative to the start of the track
     * and copy this event into the queue.
     */
    
    if (microsSinceStart + uSecs < microsSinceStart) {
      Serial.println("File too long: time has overflowed.");
      midiFile.end();
      playingFile.close();
      
      state = STATE_END_FILE;
      break;
    }
    microsSinceStart += uSecs;
   
    if (eventType == ET_TEMPO) {
      struct dataTempo *pEv = &midiFile.getEventDataP()->tempo;
      /*
       * Perform the tempo change now rather than queueing it
       * because queued notes will not cross a tempo change,
       *
       * Truncate the microseconds per beat to the nearest microsecond.
       * A 1 uSec error per beat over 70 minutes can accumulate to
       * a maximum of about 1/2 second. Insignificant for our purposes.
       */
      microsPerTick = pEv->uSecPerBeat / midiFile.getTicksPerBeat();
      
      // Stay in STATE_EVENTS, to read another event
      break;
      
    } else if (eventType == ET_CHANNEL) {
        struct dataChannel *pEv = &midiFile.getEventDataP()->channel;
        
        // We care only about note on events.
        if (pEv->code != CH_NOTE_ON) {
          break; // ignore this event; read another
        }
        
        /*
         * Queue this Note On event,
         * to be played when we've read all the simultaneous notes.
         *
         * We care only about the note; we ignore channel, etc.
         */
         
        if (numQueued >= MAX_QUEUED_EVENTS) {
          Serial.print("Too many simultaneous notes (");
          Serial.print(MAX_QUEUED_EVENTS);
          Serial.println("). Skipping one");
          
          // Stay in STATE_EVENTS, read another event.
          break;
        }
        queue[numQueued++] = pEv->param1; // the note number.
        
        // Stay in STATE_EVENTS, read another event.
        break;

    } else if (eventType == ET_END_TRACK) {
      // process end of track by playing any delay or queued notes
      state = STATE_WAITING;
      break;
    }

    // Stay in STATE_EVENTS, to read another event.
    break;
    
  case STATE_WAITING:
    
    /*
     * If we need to wait a long time to play the notes,
     * wait for another call to loop().
     */
    
    /* signed */ microsToWait = (microsSinceStart + startMicros) - micros();
    
    if (microsToWait > MAX_MICROS_TO_DELAY) {
      // Stay in STATE_WAITING.
      break;
    }

    // If we need to wait a short time, wait that time.
    if (microsToWait > 0) {
      delayMicroseconds((unsigned int) microsToWait);
    }
    microsToWait = (microsSinceStart + startMicros) - micros();
    
    // If we're late, remember that.
    if (microsToWait < 0) {
      if (maxMicrosLate < -microsToWait) {
        maxMicrosLate = -microsToWait;
      }
    }
    
    // Play the notes in the queue, if any
    int i;
    for (i = 0; i < numQueued; ++i) {
      int noteNum = queue[i];
      
      if (!(MIN_NOTE_NUM <= noteNum && noteNum < MIN_NOTE_NUM + NUM_NOTE_PINS)) {
        Serial.print("Skipping queued[");
        Serial.print(i);
        Serial.print("] note number ");
        Serial.println(noteNum);
        continue;
      }
      
      digitalWrite(pinNoteOffset[noteNum - MIN_NOTE_NUM], HIGH);
      delay(MS_PER_STRIKE);
      digitalWrite(pinNoteOffset[noteNum - MIN_NOTE_NUM], LOW);

    }
    numQueued = 0;    
  
    // Now that we've played the events, read more.
    state = STATE_EVENTS;
    break;
    
  default:
    Serial.print("INTERNAL ERROR: unknown state: ");
    Serial.println(state);
    state = STATE_ERROR;
  }
  
  // Output to the LEDs
  
  //XXX for now, just echo the On/Off switch state to the LED.
  if (digitalRead(pinButtonOff)) {
    ledOnState = LOW;
  } else {
    ledOnState = HIGH;
  }
  digitalWrite(pinLedIsOn, ledOnState);
  
  //XXX for now, just echo the Play/Pause switch state to the LED.
  if (digitalRead(pinButtonPause)) {
    ledOnState = LOW;
  } else {
    ledOnState = HIGH;
  }
  digitalWrite(pinLedPlaying, ledOnState);
  
}

/*
 * Read our settings from our SD configuration file.
 */
boolean readConfiguration() {
  SDConfigFile cfg;

  playlistUrl = 0;
  wifiSsid = 0;
  wifiPassword = 0;
  
  if (!cfg.begin(CONFIG_FILE, MAX_LINE_LENGTH)) {
    Serial.print("Failed to open configuration file: ");
    Serial.println(CONFIG_FILE);
    return false;
  }
  
  while (cfg.readNextSetting()) {
    if (strcmp("ssid", cfg.getName()) == 0) {
      if (wifiSsid != 0) {
        free(wifiSsid);
      }
      wifiSsid = cfg.copyValue();
      
    } else if (strcmp("password", cfg.getName()) == 0) {
      if (wifiPassword != 0) {
        free(wifiPassword);
      }
      wifiPassword = cfg.copyValue();

    } else if (strcmp("playUrl", cfg.getName()) == 0) {
      if (playlistUrl != 0) {
        free(playlistUrl);
      }
      playlistUrl = cfg.copyValue();

    } else {
      // Skip unrecognized names.
      Serial.print("Unknown name in config: ");
      Serial.println(cfg.getName());
    }
  }
  
  cfg.end();
}

/*
 * Reads the local SD card playlist,
 * cacheing each of the referenced Midi files,
 * creating a temporary playlist that refers only
 * to SD files (not urls), one per line.
 * Also counts the number of items in the playlist.
 * Returns true if successful, false if not.
 */
boolean transformSDPlaylist() {
  File fileIn;          // the playlist being read
  File fileOut;         // the playlist being written, which refers only to SD card files.
  char line[MAX_LINE_LENGTH + 1];
  char *cachedName;
  
  
  fileIn = SD.open(playlistSDName, FILE_READ);
  if (!fileIn) {
    Serial.print("Failed to open SD playlist: ");
    Serial.println(playlistSDName);
    
    return false;
  }
  
  numPlaylistTitles = 0;
  
  SD.remove((char *) SD_TMP_PLAYLIST);
  fileOut = SD.open(SD_TMP_PLAYLIST, FILE_WRITE);
  if (!fileOut) {
    Serial.print("Create failed: ");
    Serial.println(SD_TMP_PLAYLIST);
    
    fileIn.close();
    return false;
  }
  
  cachedName = 0;
  while (readNextLine(&fileIn, line, MAX_LINE_LENGTH + 1)) {
    if (line[0] == '\0' || line[0] == '#') {
      continue;  // skip blank and comment lines.
    }

    if (cachedName != 0) {
      free(cachedName);
      cachedName = 0;
    }
    cachedName = cacheFile(line);
    if (cachedName == 0) {
      fileIn.close();
      fileOut.close();
      return false;
    }
    
    ++numPlaylistTitles;
    if (fileOut.println(cachedName) < strlen(cachedName)) {
      // write error.
      Serial.println("write error");
      fileIn.close();
      fileOut.close();
      return false;
    }

  }
  if (cachedName != 0) {
    free(cachedName);
    cachedName = 0;
  }
  
  // Allocate the playOrder[] array
  if (playOrder != 0) {
    free(playOrder);
  }
  playOrder = (uint8_t *) malloc(numPlaylistTitles * sizeof(uint8_t));
  if (playOrder == 0) {
    Serial.println("mem");
    fileIn.close();
    fileOut.close();
    return false;
  }
    
  fileIn.close();
  fileOut.close();
}

/*
 * Fills in playOrder[],
 * based on the 
 */
void setPlayOrder() {
  int i;

  // We currently support only in-order play (no shuffle yet).
  
  for (i = 0; i < numPlaylistTitles; ++i) {
    playOrder[i] = i;
  }
}

/*
 * Reads the next line of the given file into the given buffer.
 * pFile = points to the File to read from.
 * buffer = points to the array to read the line into.
 * bufferLength = the size (bytes) of buffer[], including the terminating null.
 *  Note: the maximum line length is bufferLength - 1.
 * Returns true if the line was successfully copied into buffer[],
 *   false if end of file or some error occurred.
 */
boolean readNextLine(File *pFile, char *buffer, uint8_t bufferLength) {
  uint8_t lineLength;
  int bint;
  
  lineLength = 0;
  
  bint = pFile->read();
  if (bint < 0) {
    return false;
  }
  while (bint >= 0 && (char) bint != '\n') {
    if ((char) bint == '\r') {  // ignore returns.
      bint = pFile->read();
      continue;
    }
    
    if (lineLength >= bufferLength - 1) {
      // Line too long.
      return false;
    }
    buffer[lineLength++] = (char) bint;
    
    bint = pFile->read();
  }
  // Note: a line can end with End Of File rather than a newline.
  buffer[lineLength] = '\0';

  return true;
}

/*
 * Copies the file at the given URL to the SD card.
 * Returns a malloc()ed name of the resultant SD card file.
 * NOTE: if url starts with file:// the file is assumed
 * to already be on the SD card.
 */
char *cacheFile(const char *url) {
  char *result = 0;           // value to return
 
  char *filePrefixLC = "file://";
  char *filePrefixUC = "FILE://";
  char *httpPrefixLC = "http://";
  char *httpPrefixUC = "HTTP://";
  uint8_t prefixLen;
  int len;
  
  // If it starts with file://, it's a local file
  prefixLen = strlen(filePrefixLC);
  if (strncmp(filePrefixLC, url, prefixLen) == 0 || strncmp(filePrefixUC, url, prefixLen) == 0) {
    // It's a file. Return the local name.
    len = strlen(url) - prefixLen;
    result = (char *) malloc(len + 1);
    if (result == 0) {
      // out of memory
      return 0;
    }
    strcpy(result, &url[prefixLen]);
     
    return result;
  }
   
  // If it starts with http://, copy the url contents to the SD card.
  prefixLen = strlen(httpPrefixLC);
  if (strncmp(httpPrefixLC, url, prefixLen) == 0 || strncmp(httpPrefixUC, url, prefixLen) == 0) {
    Serial.print("http not yet supported: ");
    Serial.println(url);
    return 0;
  }
   
  // If it's none of the above, assume it's a bare local filename (not a URL).
  len = strlen(url);
  result = (char *) malloc(len + 1);
  if (result == 0) {
    // out of memory
    return 0;
  }
  strcpy(result, url);
  
  return result;
}

/*
 * Choose and open the next file we're to play.
 * Returns 1 if successful; 0 if error or if there is nothing more to play.
 */
boolean getNextFilename() {    
  uint8_t chosenLineNum;    // line number in the tmp playlist containing the file to play.
  File file;
  uint8_t curLineNum;
  char line[MAX_LINE_LENGTH + 1];
  
  if (nowPlayingIdx == 255) {
    nowPlayingIdx = 0;
  } else {
    ++nowPlayingIdx;
    
    if (nowPlayingIdx >= numPlaylistTitles) {
      // loop, reshuffle if necessary.
      setPlayOrder();
      nowPlayingIdx = 0;
    }
  }
  
  chosenLineNum = playOrder[nowPlayingIdx];
  
  file = SD.open(SD_TMP_PLAYLIST, FILE_READ);
  if (!file) {
    Serial.println("failed to open tmp playlist");
    return false;
  }
  
  curLineNum = (uint8_t) -1;;
  while (readNextLine(&file, line, MAX_LINE_LENGTH + 1)) {
    ++curLineNum;
    if (curLineNum == chosenLineNum) {
      break;
    }
  }
  
  file.close();
  
  if (curLineNum != chosenLineNum) {
    Serial.print("couldn't read line ");
    Serial.print(chosenLineNum);
    Serial.println(" from tmp playlist");
    return false;
  }

  if (playingFname != 0) {
    free(playingFname);
  }
  playingFname = (char *) malloc(strlen(line) + 1);
  if (playingFname == 0) {
    Serial.println("mem");
    return false;
  }
  strcpy(playingFname, line);
  
  Serial.print("Playing ");
  Serial.println(playingFname);

  
  return true;
}

//************************************************************************
// From http://www.avr-developers.com/mm/memoryusage.html
//*	http://www.nongnu.org/avr-libc/user-manual/malloc.html
//*	thanks to John O.
void	Ram_TableDisplay(void) 
{
  char stack = 1;
extern char *__data_start;
extern char *__data_end;
extern char *__bss_start;
extern char *__bss_end;
extern char *__heap_start;
extern char *__heap_end;

  int	data_size	=	(int)&__data_end - (int)&__data_start;
  int	bss_size	=	(int)&__bss_end - (int)&__data_end;
  int	heap_end	=	(int)&stack - (int)&__malloc_margin;
  int	heap_size	=	heap_end - (int)&__bss_end;
  int	stack_size	=	RAMEND - (int)&stack + 1;
  int	available	=	(RAMEND - (int)&__data_start + 1);	
  available	-=	data_size + bss_size + heap_size + stack_size;

  Serial.println();
  Serial.print("data size     = ");
  Serial.println(data_size);
  Serial.print("bss_size      = ");
  Serial.println(bss_size);
  Serial.print("heap size     = ");
  Serial.println(heap_size);
  Serial.print("stack used    = ");
  Serial.println(stack_size);
  Serial.print("stack available     = ");
  Serial.println(available);
  Serial.print("Free memory   = ");
  Serial.println(get_free_memory());
  Serial.println();

}
int get_free_memory()
{
extern char __bss_end;
extern char *__brkval;

  int free_memory;

  if((int)__brkval == 0)
    free_memory = ((int)&free_memory) - ((int)&__bss_end);
  else
    free_memory = ((int)&free_memory) - ((int)__brkval);

  return free_memory;
}

