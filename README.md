# glockenspiel

## Introduction

glockenspiel is an Arduino sketch that plays MIDI files from an SD card on a set of chimes. The circuit consists of an Arduino Mega 2560 Rev 3, a Sparkfun TransmogriShield, a Sparkfun CC3000 wifi/SD shield, 19 solenoids and drivers, 19 chimes made from electrical conduit, and 5 lighted buttons.

The five buttons control the glockenspiel: stop/start, play/pause, skip backward, skip forward, and shuffle.


This is a work in progress. Project status is updated in the blog at [https://needhamia.com/](https://needhamia.com/)

## Files

* **glockenspiel.ino** is the Arduino sketch
* **SD** is a set of files to copy to the SD card. The files include a sample configuration file, a sample playlist, and a set of MIDI files to play.
* **glockenspiel.fzz** is a Fritzing diagram of the circuit
* **BillOfMaterials.ods** is a parts list for the project
* **MechanicalNotes.odp** contains notes about the mechanical design
* **buttonIcons.odg** is a drawing of the button icons I'd like to use.

## Libraries used

* **MidiFileStream** is a custom library for reading the MIDI files. See [https://github.com/bneedhamia/midifilestream](https://github.com/bneedhamia/midifilestream)
* **SDConfigFile** is a custom library for reading the sketch configuration from an SD file. See [https://github.com/bneedhamia/sdconfigfile](https://github.com/bneedhamia/sdconfigfile)
* **SD** is the standard Arduino SD card library. See [http://arduino.cc/en/Reference/SD](http://arduino.cc/en/Reference/SD)

## Range
The glockenspiel has 19 chromatic notes, ranging through

    C5 = An octave above middle C
    F#6 = a little over an octave and a fourth above C5

inclusive. This provides enough range to play the melody of a large number of vocal pieces, if the melody is transposed up an octave (e.g., Middle C to C5)