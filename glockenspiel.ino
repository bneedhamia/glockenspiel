#include <SD.h> // Modified SD library, with MEGA_SOFT_SPI defined as 1 in Sd2Card.h
#include <MidiFileStream.h> // My own Midi File reading library

/*
 * Midi-file-playing Glockenspiel.
 * Version 1.0.0
 * Date: December 10, 2014
 * 
 * Copyright (c) 2014 Bradford Needham
 * (@bneedhamia, https://www.needhamia.com)
 *
 * Licensed under The MIT License (MIT):
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * This sketch assumes an Arduino Mega 2560
 * with a Sparkfun MicroSD shield
 * (https://www.sparkfun.com/products/12761)
 * and 19 solenoids
 * (https://www.sparkfun.com/products/11015 part ZHO-0420S-05A4.5)
 * which strike chimes made of 1/2" metal electrical conduit
 *
 * See glockenspiel.fzz for the circuit diagram.
 * Solenoid power is connected to the Arduino Vin.
 * It assumes the Arduino is powered from a 9V wall supply.
 *
 * For development, connect both a 9V supply
 * and the USB cable.
 * Without the 9V supply, the solenoids won't play.
 *
 * To operate: XXX add instructions.
 *
 * NOTE: The solenoid can dissipate no more than 1.2 watts continuously.
 * At 4.5ohm solenoid resistance, and 9V solenoid supply (5V was too weak),
 * anything over a 6% duty cycle will burn out the solenoid.
 */

/*
 * Pins:
 *
 * pinLED = the LED to blink on error.
 * pinSelectSD = the MicroSD shield chip select pin.
 * pinNoteOffset[] = the pin number of a solenoid that plays a chime.
 *  Indexed by the offset from MIN_NOTE_NUM. That is:
 *    pin = pinNoteOffset[midiNoteNum - MIN_NOTE_NUM];
 * MIN_NOTE_NUM = the MIDI note number of the lowest-pitched chime.
 * NUM_NOTE_PINS = the number of MIDI notes we support.
 *    That is, the number of chimes we have.
 */
 
const int pinLed = 13;
const int pinSelectSD = 8;  // Sparkfun SD card chip select

const int MIN_NOTE_NUM = 72; // Midi note 72 corresponds to C5
const int pinNoteOffset[] = {
  23, 25, 27, 29, 31, 33, 35, 37, 39, 41,
  22, 24, 26, 28, 30, 32, 34, 36, 38
};
const int NUM_NOTE_PINS = sizeof(pinNoteOffset) / sizeof(pinNoteOffset[0]);

/*
 * time per solenoid actuation, in milliseconds.
 * Too large a number here can burn out the solenoids.
 */
const int MS_PER_STRIKE = 3;

/*
 * Values for state.
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

/*
 * State of our file-playing machine.
 * See STATE_*.
 */
char state;

// A dummy playlist.
const char *names[] = {
//    "ANTHEM.MID",
//    "ANTHEM2.MID",
//    "CSSAMP.MID",
//    "FLOUR.MID",
//    "FOREST.MID",
//    "FULLJAM.MID",
    "SOMERSET.MID",
//    "JOY.MID",
//    "STOP1.MID",
//    "THETOWN.MID",
    0
};
int nameIdx = 0;

const int MAX_FNAME_BYTES = 12 + 1;          // 12 = 8 + '.' + 3, +1 for null termination
char fName[MAX_FNAME_BYTES] = { (char) 0 };  // current file being played.

MidiFileStream midiFile;  // The current Midi file being read.
File file;          // the underlying SD File.

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
 * we can't play a piece that plays longer than that time.
 * 
 */
unsigned long startMicros;

/*
 * the time, in microseconds, between startMicros
 * and the most-recently-queued event.
 */
unsigned long microsSinceStart;

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
long MAX_MICROS_TO_DELAY = 11 * 1000L;

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

int getNextFilename();

void setup() {
  int i;
 
  Serial.begin(9600);
  Serial.println("Reset.");

  Ram_TableDisplay(); //Debug

  pinMode(pinLed, OUTPUT);
  pinMode(pinSelectSD, OUTPUT);
  for (i = 0; i < NUM_NOTE_PINS; ++i) {
    pinMode(pinNoteOffset[i], OUTPUT);
  }
  
  state = STATE_ERROR;
  
  Serial.println("calling SD.begin()");
  if (!SD.begin(pinSelectSD)) {
    Serial.println("SD.begin() failed. Check card insertion.");
    return;
  }
  
  state = STATE_END_FILE;

}

void loop() {
  char chunkType;
  event_t eventType;
  int bLeft;
  long uSecs;
  long microsToWait;
  
  //XXX Check any buttons.
  
  switch (state) {
  
  case STATE_ERROR:
    // Show an error by blinking the LED
    if ((millis() % 1000) < 500) {
      digitalWrite(pinLed, HIGH);
    } else {
      digitalWrite(pinLed, LOW);
    }
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
      file = SD.open(fName, FILE_READ);
      if (!file) {
        Serial.print("failed to open file ");
        Serial.println(fName);
        
        state = STATE_END_FILE; // skip to the next file
      } else {
        if (!midiFile.begin(file)) {
          Serial.print("failed to open midi file: ");
          Serial.println(fName);
          midiFile.end();
          file.close();
                   
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
      file.close();
      
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
      file.close();
      
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
     * Either the current event is simultenous with the previous one,
     * or there's nothing queued.
     * In either case,
     * Update the time of this event relative to the start of the track
     * and copy this event into the queue.
     */
    
    if (microsSinceStart + uSecs < microsSinceStart) {
      Serial.println("File too long: time has overflowed.");
      midiFile.end();
      file.close();
      
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
      // If there's anthing in the queue, play it.
      //XXX not quite right, because a delay to end of track isn't queued.
      if (numQueued > 0) {
        isPushedBack = true;
        state = STATE_WAITING;
        break;
      }
      
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
    
    // Play the notes in the queue
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
}

/*
 * Choose and open the next file we're to play.
 * Returns 1 if successful; 0 if error or if there is nothing more to play.
 */
int getNextFilename() {  

  if (names[nameIdx] == 0) {
    Serial.println("End of Playlist");
    return false;
  }
  
  // Copy the filename to play
  if (strlen(names[nameIdx]) > MAX_FNAME_BYTES - 1) {
    Serial.print("names[");
    Serial.print(nameIdx);
    Serial.print("] too long: ");
    Serial.println(strlen(names[nameIdx]));
    return false;
  }
  strcpy(fName, names[nameIdx]);
  Serial.print("Playing ");
  Serial.println(fName);
  
  // Prepare to get the next filename.
  ++nameIdx;
  
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

