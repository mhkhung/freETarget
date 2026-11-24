/*
 * Just enough of the Arduino core to compile json.ino on a computer.
 * Serial output is captured in serial_out; EEPROM is a 4 KB array
 * with guard bytes after it.
 */
#ifndef ARDUINO_STUB_H
#define ARDUINO_STUB_H
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

typedef uint8_t byte;
#define F(s) (s)
#define PI 3.14159265358979
#define HEX 16
#define INPUT_PULLUP 2
#define OUTPUT 1
#define sq(x) ((x) * (x))
#define dtostrf(value, width, precision, buffer) (sprintf((buffer), "%*.*f", (width), (precision), (double)(value)), (buffer))

extern std::string serial_out;
struct SerialStub
{
  template <typename T> void print(T value) { serial_out += std::to_string(value); }
  void print(const char *s) { serial_out += s; }
  void print(char *s) { serial_out += s; }
  void print(char c) { serial_out += c; }
  template <typename T> void print(T value, int) { print(value); }
  int available(void) { return 0; }
  int read(void) { return -1; }
};
extern SerialStub Serial, Serial2, Serial3;

#define EEPROM_SIZE 4096
#define EEPROM_GUARD 64
extern uint8_t eeprom[EEPROM_SIZE + EEPROM_GUARD];
extern unsigned int eeprom_writes;
struct EEPROMStub
{
  template <typename T> T &get(int address, T &value)
  {
    memcpy(&value, &eeprom[address], sizeof(T));
    return value;
  }
  template <typename T> const T &put(int address, const T &value)
  {
    memcpy(&eeprom[address], &value, sizeof(T));
    eeprom_writes += sizeof(T);
    return value;
  }
  void update(int address, uint8_t value)
  {
    if ( eeprom[address] != value )
    {
      eeprom[address] = value;
      eeprom_writes++;
    }
  }
};
extern EEPROMStub EEPROM;

static inline void delay(unsigned long) {}
static inline unsigned long millis(void) { return 0; }
#endif
