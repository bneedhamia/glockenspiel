/*
 * Midi-file-playing Glockenspiel.
 * https://github.com/bneedhamia/glockenspiel
 * 
 * Copyright (c) 2014, 2015 Bradford Needham
 * ( @bneedhamia , https://www.needhamia.com )
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
 * To operate:
 * 1) Create the glocken.cfg, playlist.m3u, and Midi files
 *    and copy them to the SD card.
 * 2) Insert the SD card into the glockenspiel and plug in the power.
 * 3) Press the On/Off button to start playing.
 *    At any time, press the On/Off button again to stop playing.
 * 4) While playing, press the Play/Pause button to pause playing.
 *    At any time, press the Play/Pause button again to resume playing.
 * 5) While playing, press the Back button to skip backward one track.
 * 6) While playing, press the Forward button to skip forward one track.
 * 7) While playing or paused, press the Shuffle button to shuffle the tracks.
 *    Press it again to play the tracks in order.
 *XXX currently, the skip back button has no effect.
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
 * pinLedIsOn = the On/Off/Error state LED:
 *   off = stopped; blinking = error (e.g, SD card file error);
 *   on = running.
 * pinButtonPause = the Play/Pause button. Internal pullup.
 * pinLedPlaying = the Playing/Paused state. Lighted when playing; blinking when paused.
 * pinButtonBack = the Skip backward button. Internal pullup.
 * pinLedBack = the Skip back request state. Lighted when the button is pushed.
 * pinButtonForward = the Skip forward button. Internal pullup.
 * pinLedForward = the Skip forward request state. Lighted when the button is pushed.
 * pinButtonShuffle = the Shuffle Tracks button. Internal pullup.
 * pinLedShuffle = the Shuffled Tracks state. Lighted when tracks are shuffled.
 * pinRandom = an unconnected pin used to seed the random generator.
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
const int pinLedIsOn = A15;   // Digital Output
const int pinButtonShuffle = 42; // Digital Input
const int pinLedShuffle = 43;    // Digital Output
const int pinButtonForward = 44; // Digital Input
const int pinLedForward = 45;    // Digital Output
const int pinButtonBack = 46; // Digital Input
const int pinLedBack = 47;    // Digital Output
const int pinButtonPause = 48;// Digital Input
const int pinLedPlaying = 49; // Digital Output
const int pinRandom = A0;     // Analog Input

/*
 * time per solenoid actuation, in milliseconds.
 * Too large a number here can burn out the solenoids.
 * Too small a number here will cause some solenoids to not strike.
 */
const int MS_PER_STRIKE = 3;

/*
 * time (milliseconds) a button output must remain stable
 * to be considered real. Used for debounce.
 */
const int MAX_BOUNCE_MS = 50;

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
 * The name of the SD card playlist we'll create from
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
const char STATE_PAUSED = (char) 2;
const char STATE_END_FILE = (char) 3;
const char STATE_END_TRACK = (char) 4;
const char STATE_EVENTS = (char) 5;
const char STATE_WAITING = (char) 6;

void doPause();
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
uint8_t nowPlayingIdx;   // index into playOrder[] of the title we're currently playing
char *playingFname;      // SD filename of the file being played.
MidiFileStream midiFile; // The current, open Midi file being played.
File playingFile;        // the underlying SD File of that Midi file.

char state;              // State of our file-playing machine. See STATE_*.
char pausedState;        // State to restore after pausing
unsigned long microsBehindNext; // while paused, difference between the start of the next queued event
                         // and what the current time was when pause began.

boolean doShuffle;       // if true, shuffle the tracks (vs. play in order)
boolean isShuffled;      // if true, the current playlist is shuffled.

boolean pressedButtonOn; // raw previous state of the On/off button
boolean heldButtonOn;    // debounced previous state of the On/off button.
unsigned long changedButtonOnMs; // time (milliseconds) of the last change to heldButtonOn

boolean pressedButtonPlay; // raw previous state of the Play/pause button
boolean heldButtonPlay;    // debounced previous state of the Play/pause button.
unsigned long changedButtonPlayMs; // time (milliseconds) of the last change to heldButtonPlay

boolean pressedButtonBack; // raw previous state of the Back button
boolean heldButtonBack;    // debounced previous state of the Back button.
unsigned long changedButtonBackMs; // time (milliseconds) of the last change to heldButtonBack

boolean pressedButtonForward; // raw previous state of the Forward button
boolean heldButtonForward;    // debounced previous state of the Forward button.
unsigned long changedButtonForwardMs; // time (milliseconds) of the last change to heldButtonForward

boolean pressedButtonShuffle; // raw previous state of the Shuffle button
boolean heldButtonShuffle;    // debounced previous state of the Shuffle button.
unsigned long changedButtonShuffleMs; // time (milliseconds) of the last change to heldButtonShuffle

long microsPerTick = 1;   // current tempo, in microseconds per tick.

/*
 * For performance measurement,
 * maxEventReadMillis = the maximum time (milliseconds) it took to read an event from SD.
 * maxMicrosLate = the maximum time (microseconds) that we're late in playing a Midi event.
 */
unsigned long maxEventReadMillis;
unsigned long maxMicrosLate;

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
 * and the most-recently-queued Midi event.
 */
unsigned long microsSinceStart;

/*
 * The queue of notes to play simultaneously.
 * The maximum is the number of distinct pitches we can play simultaneously.
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
 
  Serial.begin(9600);
  Serial.println("Reset.");

  Ram_TableDisplay(); //Debug

  // Set up all the pins we use.
  
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
  
  pinMode(pinButtonBack, INPUT);
  digitalWrite(pinButtonBack, HIGH);   // enable internal pull-up resistor
  pinMode(pinLedBack, OUTPUT);
  
  pinMode(pinButtonForward, INPUT);
  digitalWrite(pinButtonForward, HIGH);  // enable internal pull-up resistor
  pinMode(pinLedForward, OUTPUT);
  
  pinMode(pinButtonShuffle, INPUT);
  digitalWrite(pinButtonShuffle, HIGH);  // enable internal pull-up resistor
  pinMode(pinLedShuffle, OUTPUT);
  
  // initialize our variables
  // The random seed is a value read from an unconnected analog input.
  
  int seed;
  seed = analogRead(pinRandom);
  Serial.print("Random seed = ");
  Serial.println(seed);
  randomSeed(seed);
  
  state = STATE_ERROR;

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
  
  // Wait for the user to press the on/off button.
  state = STATE_STOPPED;
}

void loop() {
  chunk_t chunkType;
  event_t eventType;
  int bLeft;
  long uSecs;
  long microsToWait;
  boolean buttonPressed; // temporary state of a button
  boolean changeOnOff;  // If true, change the On/Off (Stopped) state.
  boolean changePlayPause; // If true, change the Play/Pause (Paused) state.
  boolean skipBack;     // If true, skip backward one track.
  boolean skipForward;  // If true, skip forward one track.
  boolean changeShuffle; // If true, change the Shuffle state.
  
  changeOnOff = false;
  changePlayPause = false;
  skipBack = false;
  skipForward = false;
  changeShuffle = false;
  
  /*
   * When the user presses the on/off button for enough time,
   * toggle the on/off state:
   * Debounce the on/off button (see http://arduino.cc/en/Tutorial/Debounce )
   * Record the time whenever the raw button state changes;
   * If the button's been stable long enough,
   * Set changeOnOff if the button has been reliably pressed.
   */

  buttonPressed = false;
  if (digitalRead(pinButtonOff) == LOW) { // Internal pullup = button is "active low"
    buttonPressed = true;
  }

  if (buttonPressed != pressedButtonOn) {
    changedButtonOnMs = millis();
  }
  pressedButtonOn = buttonPressed;

  if ((millis() - changedButtonOnMs) > MAX_BOUNCE_MS) {
    if (buttonPressed != heldButtonOn) {
      heldButtonOn = buttonPressed;
      
      changeOnOff = false;
      if (buttonPressed) {
        changeOnOff = true;
      }
    }
  }
  
  /*
   * When the user presses the play/pause button for enough time,
   * toggle the play/pause state.
   */

  buttonPressed = false;
  if (digitalRead(pinButtonPause) == LOW) { // Internal pullup = button is "active low"
    buttonPressed = true;
  }

  if (buttonPressed != pressedButtonPlay) {
    changedButtonPlayMs = millis();
  }
  pressedButtonPlay = buttonPressed;

  if ((millis() - changedButtonPlayMs) > MAX_BOUNCE_MS) {
    if (buttonPressed != heldButtonPlay) {
      heldButtonPlay = buttonPressed;
      changePlayPause = false;
      if (buttonPressed) {
        changePlayPause = true;
      }
    }
  }
  
  /*
   * When the user presses the back button for enough time,
   * skip backward one track.
   */

  buttonPressed = false;
  if (digitalRead(pinButtonBack) == LOW) { // Internal pullup = button is "active low"
    buttonPressed = true;
  }

  if (buttonPressed != pressedButtonBack) {
    changedButtonBackMs = millis();
  }
  pressedButtonBack = buttonPressed;

  if ((millis() - changedButtonBackMs) > MAX_BOUNCE_MS) {
    if (buttonPressed != heldButtonBack) {
      heldButtonBack = buttonPressed;
      skipBack = false;
      if (buttonPressed) {
        skipBack = true;
      }
    }
  }
  
  /*
   * When the user presses the forward button for enough time,
   * skip forward one track.
   */

  buttonPressed = false;
  if (digitalRead(pinButtonForward) == LOW) { // Internal pullup = button is "active low"
    buttonPressed = true;
  }
  buttonPressed = !buttonPressed; // invert because the button is miswired to NC vs. NO

  if (buttonPressed != pressedButtonForward) {
    changedButtonForwardMs = millis();
  }
  pressedButtonForward = buttonPressed;

  if ((millis() - changedButtonForwardMs) > MAX_BOUNCE_MS) {
    if (buttonPressed != heldButtonForward) {
      heldButtonForward = buttonPressed;
      skipForward = false;
      if (buttonPressed) {
        skipForward = true;
      }
    }
  }
  
  /*
   * When the user presses the shuffle button for enough time,
   * toggle the Shuffle Tracks state.
   */

  buttonPressed = false;
  if (digitalRead(pinButtonShuffle) == LOW) { // Internal pullup = button is "active low"
    buttonPressed = true;
  }
  buttonPressed = !buttonPressed; // invert because the button is miswired to NC vs. NO

  if (buttonPressed != pressedButtonShuffle) {
    changedButtonShuffleMs = millis();
  }
  pressedButtonShuffle = buttonPressed;

  if ((millis() - changedButtonShuffleMs) > MAX_BOUNCE_MS) {
    if (buttonPressed != heldButtonShuffle) {
      heldButtonShuffle = buttonPressed;
      changeShuffle = false;
      if (buttonPressed) {
        changeShuffle = true;
      }
    }
  }
  
  /*
   * Since Shuffle affects the next track played,
   * update the Shuffle state.
   * Listen to the shuffle button only if we're On.
   */
  
  if (state != STATE_ERROR && state != STATE_STOPPED) {
    if (changeShuffle) {
      doShuffle = !doShuffle;
    }
  }

  /*
   * Now that we have our button inputs,
   * calculate the next state of our state machine.
   */
  switch (state) {
  
  case STATE_ERROR:
    // Nothing to do.
    break;
    
  case STATE_STOPPED:
  
    // (ignore presses of the other buttons)
    
    if (!changeOnOff) {
      break; // nothing to do.
    }
    
    /*
     * The user has pressed the on/off button.
     * Start playing.
     */
    
    readConfiguration();
    Serial.print("Playlist: ");
    Serial.println(playlistUrl);
    
    if (playlistSDName) {
      free(playlistSDName);
      playlistSDName = 0;
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
    
    setPlayOrder();
    
    // Pretend we've just finished playing the last of the playlist.
    nowPlayingIdx = numPlaylistTitles - 1;
    
    Ram_TableDisplay(); // Debug: to see on/off memory leaks.    
    state = STATE_END_FILE;
    break;
    
  case STATE_PAUSED:
    // Handle the on/off button
    if (changeOnOff) {
      // Close the currently-playing file, if any.
      midiFile.end();
      playingFile.close();
      state = STATE_STOPPED;
      break;
    }
    
    if (!changePlayPause) {
      break; // nothing to do. Ignore the skip buttons.
    }
    
    /*
     * Resume from where we paused at:
     * Move the start-of-file time
     * as if we never paused.
     */
    startMicros = (micros() + microsBehindNext) - microsSinceStart;

    state = pausedState;
    break;
    
  case STATE_END_FILE:
  
    // Handle the on/off button
    if (changeOnOff) {
      state = STATE_STOPPED;
      break;
    }
    
    // Handle the pause button
    if (changePlayPause) {
      doPause();
      break;
    }
    
    if (!getNextFilename()) {
      // An error occurred
      
      // Report our performance numbers
      Serial.print("Maximum event-reading delay = ");
      Serial.print(maxEventReadMillis);
      Serial.println(" ms");
      
      Serial.print("Maximum event-playing lateness = ");
      Serial.print(maxMicrosLate);
      Serial.print(" us");
      
      state = STATE_ERROR;  // we can't recover, so blink the error light.
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
  
    // Handle the on/off button
    if (changeOnOff) {
      midiFile.end();
      playingFile.close();

      state = STATE_STOPPED;
      break;
    }
    
    // Handle the pause button
    if (changePlayPause) {
      doPause();
      break;
    }
    
    // Handle the Skip Forward button
    if (skipForward) {
      midiFile.end();
      playingFile.close();
      
      state = STATE_END_FILE;
      break;
    }

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
    
    // Handle the on/off button
    if (changeOnOff) {
      midiFile.end();
      playingFile.close();

      state = STATE_STOPPED;
      break;
    }
      
    // Handle the pause button
    if (changePlayPause) {
      doPause();
      break;
    }
    
    // Handle the Skip Forward button
    if (skipForward) {
      midiFile.end();
      playingFile.close();
      
      state = STATE_END_FILE;
      break;
    }

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
    
    // Handle the on/off button
    if (changeOnOff) {
      midiFile.end();
      playingFile.close();

      state = STATE_STOPPED;
      break;
    }
    
    // Handle the pause button
    if (changePlayPause) {
      doPause();
      break;
    }
    
    // Handle the Skip Forward button
    if (skipForward) {
      midiFile.end();
      playingFile.close();
      
      state = STATE_END_FILE;
      break;
    }
    
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
      
      // If the note is out of our range, don't try to play it.
      if (!(MIN_NOTE_NUM <= noteNum && noteNum < MIN_NOTE_NUM + NUM_NOTE_PINS)) {
        Serial.print("Skipping queued[");
        Serial.print(i);
        Serial.print("] note ");
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
  
  /*
   * The On/Off button's LED can show three states:
   * Error; Stopped; not-Stopped (that is, running)
   */
  if (state == STATE_ERROR) {
    // Blink
    if ((millis() % 1000) < 500) {
      digitalWrite(pinLedIsOn, HIGH);
    } else {
      digitalWrite(pinLedIsOn, LOW);
    }
  } else if (state == STATE_STOPPED) {
    digitalWrite(pinLedIsOn, LOW);
  } else {
    digitalWrite(pinLedIsOn, HIGH);
  }
  
  /*
   * The Play/Pause button's LED indicates
   * whether we are paused or not:
   * Off = playing;
   * On = paused.
   */
 
  if (state == STATE_PAUSED) {
    digitalWrite(pinLedPlaying, HIGH);
  } else {
    digitalWrite(pinLedPlaying, LOW);
  }
  
  /*
   * The Back button's LED indicates
   * that the Back button is being held.
   */

  digitalWrite(pinLedBack, heldButtonBack);
  
  /*
   * The Forward button's LED indicates
   * that the Forward button is being held.
   */
  
  digitalWrite(pinLedForward, heldButtonForward);
  
  /*
   * The Shuffle button's LED is lighted
   * if we're On and the tracks are (to be) shuffled.
   */
  if (state == STATE_ERROR || state == STATE_STOPPED) {
    digitalWrite(pinLedShuffle, LOW);
  } else {
    digitalWrite(pinLedShuffle, doShuffle);
  }

}

/*
 * Pauses playback, preserving the state
 * so that it can be resumed.
 */
void doPause() {
  pausedState = state;
  
  /*
   * Save the current time relative to the next note
   * so that we can restore that relationship on resume.
   * If we don't do this, on resume we will try to catch up with the original start.
   */
  microsBehindNext = (startMicros + microsSinceStart) - micros();

  
  state = STATE_PAUSED;
}

/*
 * Read our settings from our SD configuration file.
 * Returns true if successful; false if an error occurred.
 */
boolean readConfiguration() {
  SDConfigFile cfg;

  // Clear any previous state.
  if (playlistUrl) {
    free(playlistUrl);
    playlistUrl = 0;
  }
  if (wifiSsid) {
    free(wifiSsid);
    wifiSsid = 0;
  }
  if (wifiPassword) {
    free(wifiPassword);
    wifiPassword = 0;
  }
  
  if (!cfg.begin(CONFIG_FILE, MAX_LINE_LENGTH)) {
    Serial.print("Failed to open configuration file: ");
    Serial.println(CONFIG_FILE);
    return false;
  }
  
  while (cfg.readNextSetting()) {
    if (strcmp("ssid", cfg.getName()) == 0) {
      if (wifiSsid) {
        free(wifiSsid);
      }
      wifiSsid = cfg.copyValue();
      
    } else if (strcmp("password", cfg.getName()) == 0) {
      if (wifiPassword) {
        free(wifiPassword);
      }
      wifiPassword = cfg.copyValue();

    } else if (strcmp("playUrl", cfg.getName()) == 0) {
      if (playlistUrl) {
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
  
  return true;
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
  
  // If our temporary file exists, delete it.
  if (SD.exists((char *) SD_TMP_PLAYLIST)) {
    if (!SD.remove((char *) SD_TMP_PLAYLIST)) {
      Serial.println("Failed to remove tmp playlist");
      
      return false;
    }
  }
  
  fileIn = SD.open(playlistSDName, FILE_READ);
  if (!fileIn) {
    Serial.print("Failed to open SD playlist: ");
    Serial.println(playlistSDName);
    
    return false;
  }
  
  fileOut = SD.open(SD_TMP_PLAYLIST, FILE_WRITE);
  if (!fileOut) {
    Serial.print("Create failed: ");
    Serial.println(SD_TMP_PLAYLIST);
    
    fileIn.close();
    return false;
  }
  
  numPlaylistTitles = 0;
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
      
      if (cachedName != 0) {
        free(cachedName);
        cachedName = 0;
      }
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
  if (playOrder) {
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
 * based on the Shuffle state.
 */
void setPlayOrder() {
  uint8_t i;
  uint8_t numLeft; // number of positions remaining to shuffle
  uint8_t swap;    // temporary, for exchanging two values.

  isShuffled = false;

  // Initialize the play order
  for (i = 0; i < numPlaylistTitles; ++i) {
    playOrder[i] = i;
  }
  
  if (!doShuffle) {
    return; // play order = playlist order.
  }
  
  // Shuffle the tracks via the Fisher–Yates shuffle algorithm
  for (numLeft = numPlaylistTitles; numLeft > 1; --numLeft) {
    i = (uint8_t) random(numLeft);
    swap = playOrder[i];
    playOrder[i] = playOrder[numLeft - 1];
    playOrder[numLeft - 1] = swap;
  }
 
  /* 
  Serial.print("Play Order =");
  for (i = 0; i < numPlaylistTitles; ++i) {
    Serial.print(" ");
    Serial.print(playOrder[i]);
  }
  Serial.println();
   */
  
  isShuffled = true;

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
    len = strlen(url) - (int) prefixLen;
    result = (char *) malloc(len + 1);
    if (result == 0) {
      return 0;      // out of memory
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
    return 0;    // out of memory
  }
  strcpy(result, url);
  
  return result;
}

/*
 * Choose and open the next file we're to play.
 * Returns 1 if successful; 0 if error.
 */
boolean getNextFilename() {    
  uint8_t chosenLineNum;    // line number in the tmp playlist containing the file to play.
  File file;
  uint8_t curLineNum;
  char line[MAX_LINE_LENGTH + 1];
  
  ++nowPlayingIdx;
  
  // Reorder the tracks if we've run out or if we need to shuffle or unshuffle
  if (nowPlayingIdx >= numPlaylistTitles || doShuffle != isShuffled) {
    // loop, reshuffle if necessary.
    setPlayOrder();
    nowPlayingIdx = 0;
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

  if (playingFname) {
    free(playingFname);
  }
  playingFname = (char *) malloc(strlen(line) + 1);
  if (!playingFname) {
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

