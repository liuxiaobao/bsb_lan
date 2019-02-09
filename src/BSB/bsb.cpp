#if defined(ARDUINO) && ARDUINO >= 100
#include "Arduino.h"
#else
#include "WProgram.h"
#endif

#include "bsb.h"

#define DEBUG_LL 0

extern int bus_type;

// Constructor
BSB::BSB(uint8_t rx, uint8_t tx, uint8_t addr, uint8_t d_addr) {
  serial = new BSBSoftwareSerial(rx, tx, true);

  serial->begin(4800);
  serial->listen();
  myAddr=addr;
  destAddr=d_addr;
}

uint8_t BSB::setBusType(uint8_t bus_type_val, uint8_t addr, uint8_t d_addr) {
  bus_type = bus_type_val;
  switch (bus_type) {
    case 0: len_idx = 3; break;
    case 1: len_idx = 1; break;
    case 2: len_idx = 8; break;
    default: len_idx = 3; break;
  }
  if (addr<0xff) {
    myAddr = addr;
  }
  if (d_addr<0xff) {
    destAddr = d_addr;
  }
  Serial.print(F("My address: "));
  Serial.println(myAddr);
  Serial.print(F("Destination address: "));
  Serial.println(destAddr);
  return bus_type;
}

uint8_t BSB::getBusType() {
  return bus_type;
}

uint8_t BSB::getBusAddr() {
  return myAddr;
}

uint8_t BSB::getBusDest() {
  return destAddr;
}


// Dumps a message to Serial
void BSB::print(byte* msg) {
  if (bus_type != 2) {
    //if (msg[0] != 0xDC) return;
    byte len = msg[len_idx];
    //if (len > 30) return;
    byte data = 0;

    for (; len > 0-bus_type; len--) {	// msg length counts from zero with LPB (bus_type 1) and from 1 with BSB (bus_type 0)
      data = msg[msg[len_idx]-len];
      if (data < 16) Serial.print("0");
      Serial.print(data, HEX);
      Serial.print(" ");
    }
    Serial.println();
  }
}

// Receives a message and stores it to buffer
boolean BSB::Monitor(byte* msg) {
  unsigned long int ts;
  byte read;
  byte i=0;
    
  if (serial->available() > 0) {
    // get timestamp
    ts=millis();
    // output
    Serial.print(ts);
    Serial.print(" ");
    while (serial->available() > 0) {
      
      // Read serial data...
      read = serial->read();
      if (bus_type != 2) {
        read = read ^ 0xFF;
      }

      msg[i] = read;
      i++;

      // output
      if(read<16){  
        Serial.print("0");
      }
      Serial.print(read, HEX);
      Serial.print(" ");
      // if no inout available -> wait
      if (serial->available() == 0) {
        unsigned long timeout = millis() + 3;// > ((11/4800)*1000);
        while (millis() < timeout) {
          delayMicroseconds(15);
        }
      }
      // if still no input available telegramm has finished
      if (serial->available() == 0) break;
    }
    Serial.println();
    return true;
  }
  return false;
}

bool BSB::GetMessage(byte* msg) {
  byte i=0,timeout;
  byte read;

  while (serial->available() > 0) {
    // Read serial data...
    read = serial->read();
    if (bus_type != 2) {
      read = read ^ 0xFF;
    }

#if DEBUG_LL
    Serial.println();    
    if(read<16){  
      Serial.print("0");
    }
    Serial.print(read, HEX);
    Serial.print(" ");
#endif    
    
    // ... until SOF detected (= 0xDC, 0xDE bei BSB bzw. 0x78 bei LPB)
    if ((bus_type == 0 && (read == 0xDC || read == 0xDE)) || (bus_type == 1 && read == 0x78) || (bus_type == 2 && (read == 0x17 || read == 0x1D || read == 0x1E))) {
      // Restore otherwise dropped SOF indicator
      msg[i++] = read;
      if (bus_type == 2 && read == 0x17) {
	uint8_t PPS_write_enabled = myAddr;
	if (PPS_write_enabled == 1) {
          return true; // PPS-Bus request byte 0x17 just contains one byte, so return
	} else {
	  len_idx = 9;
	}
      }

      // Delay for more data
      delay(1);

      // read the rest of the message
      while (serial->available() > 0) {
        read = serial->read();
        if (bus_type != 2) {
          read = read ^ 0xFF;
        }
        msg[i++] = read;
#if DEBUG_LL
        if(read<16){  
          Serial.print("0");
        }
        Serial.print(read, HEX);
        Serial.print(" ");
#endif
        // Break if message seems to be completely received (i==msg.length)
        if (i > len_idx) {
          if (bus_type == 2) {
            break;
          }
          if ( msg[len_idx] > 32 ) // check for maximum message length
            break;
          if (i >= msg[len_idx]+bus_type)
            break;
        }
        // Delay until we got next byte
        if (serial->available() == 0) {
          timeout = 30;
          while ((timeout > 0) && (serial->available() == 0)){
            delayMicroseconds(15);
            timeout--;
          }
        }

        // Break if next byte signals next message (0x23 ^ 0xFF == 0xDC)
        // if((serial->peek() == 0x23)) break;
        // DO NOT break because some messages contain a 0xDC 
      }

      // We should have read the message completely. Now check and return

      if (bus_type == 2) {
        if (i == len_idx+1) {
	  len_idx = 8;
          return true; // TODO: add CRC check before returning true/false
        }
      } else {

        if (i == msg[len_idx]+bus_type) {		// LPB msg length is one less than BSB
          // Seems to have received all data
          if (bus_type == 1) {
            if (CRC_LPB(msg, i-1)-msg[i-2]*256-msg[i-1] == 0) return true;
            else return false;
	  } else {
            if (CRC(msg, i) == 0) return true;
            else return false;
	  }
        } else {
          // Length error
          return false;
        }
      }
    }
  }
  // We got no data so:
  return false;
}

// Generates CCITT XMODEM CRC from BSB message
uint16_t BSB::CRC (byte* buffer, uint8_t length) {
  uint16_t crc = 0, i;

  for (i = 0; i < length; i++) {
    crc = _crc_xmodem_update(crc, buffer[i]);
  }

  // Complete message returns 0x00
  // Message w/o last 2 bytes (CRC) returns last 2 bytes (CRC)
  return crc;
}

// Generates checksum from LPB message
// (255 - (Telegrammlänge ohne PS - 1)) * 256 + Telegrammlänge ohne PS - 1 + Summe aller Telegrammbytes
uint16_t BSB::CRC_LPB (byte* buffer, uint8_t length) {
  uint16_t crc = 0;
  uint8_t i;

  crc = (257-length)*256+length-2;

  for (i = 0; i < length-1; i++) {
    crc = crc+buffer[i];
  }

  return crc;
}

// Generates CRC for PPS message
uint8_t BSB::CRC_PPS (byte* buffer, uint8_t length) {
  uint8_t crc = 0, i;
  int sum = 0;

  for (i = 0; i < length; i++) {
    sum+=buffer[i];
  }
  sum = sum & 0xFF;
  crc = 0xFF - sum + 1;

  return crc;
}

// Low-Level sending of message to bus
inline bool BSB::_send(byte* msg) {
// Nun - Ein Teilnehmer will senden :
  byte i;
  byte data, len;
  if (bus_type != 2) {
    len = msg[len_idx];
  } else {
    len = len_idx;
  }
  switch (bus_type) {
    case 0:
      msg[0] = 0xDC;
      msg[1] = myAddr | 0x80;
      msg[2] = destAddr;
      break;
    case 1:
      msg[0] = 0x78;
      msg[2] = destAddr;
      msg[3] = myAddr;
      break;
  }
  {
    if (bus_type == 0) {
      uint16_t crc = CRC (msg, len -2);
      msg[len -2] = (crc >> 8);
      msg[len -1] = (crc & 0xFF);
    }
    if (bus_type == 1) {
      uint16_t crc = CRC_LPB (msg, len);
      msg[len-1] = (crc >> 8);
      msg[len] = (crc & 0xFF);
    }
    if (bus_type == 2) {
      uint8_t crc = CRC_PPS (msg, len);
      msg[len] = crc;
    }
  }

#if DEBUG_LL  
  print(msg);
#endif  
  /*
Er wartet 11/4800 Sek ab (statt 10, Hinweis von miwi), lauscht und schaut ob der Bus in dieser Zeit von jemand anderem benutzt wird. Sprich ob der Bus in dieser Zeit mal
auf 0 runtergezogen wurde. Wenn ja - mit den warten neu anfangen.
*/
  unsigned long timeoutabort = millis() + 1000;  // one second timeout
  retry:
  // Select a random wait time between 60 and 79 ms
  unsigned long waitfree = random(1,60) + 25; // range 26 .. 85 ms
//  unsigned long waitfree = random(1,20) + 59; // range 60 .. 79 ms
  { // block begins
    if(millis() > timeoutabort){  // one second has elapsed
      return false;
    }
    if (bus_type != 2) {
      // Wait 59 ms plus a random time
      unsigned long timeout = millis() + waitfree;
//      unsigned long timeout = millis() + 3;//((1/480)*1000);
      while (millis() < timeout) {
        if ( serial->rx_pin_read()) // inverse logic
        {
          goto retry;
        } // endif
      } // endwhile
    }
  } // block ends

  //Serial.println("bus free");

/*
Wenn nicht wird das erste Bit gesendet. ( Startbit )

Jedes gesendete Bit wird ( wegen Bus ) ja sofort auf der Empfangsleitung
wieder ankommen. Man schaut nach, ob das gesendete Bit mit dem
empfangenen Bit übereinstimmt.
Wenn ich eine "0" sende - also den Bus auf High lasse, dann will ich
sehen, dass der Bus weiterhin auf High ist. Sollte ein anderer
Teilnehmer in dieser Zeit eine "1" senden - also den Bus herunterziehen,
dann höre ich sofort auf mit dem Senden und fange oben wieder an.
Danach folgen nach gleichem Muster die folgenden Bits, Bit 7..0, Parity
und Stop Bit.
*/

/* 
FH 27.12.2018: Wer auch immer das obige geschrieben hat, es macht bezogen auf
den nachfolgenden Code keinen Sinn: 
1. Es wird hier nicht bitweise gesendet, sondern ein ganzes Byte an
BSBSoftwareSerial::write übergeben. Dort wird dann unabhängig davon, ob der 
Bus frei ist oder nicht, dieses komplette Byte inkl. Start-, Stop- und Parity-
Bytes gesendet.
2. BSBSoftwareSerial::write gibt immer 1 zurück, außer wenn _tx_delay == 0 ist.
Diese Variable wird aber nur einmalig bei Aufruf von BSBSoftwareSerial::begin
gesetzt und wäre nur in seltenen Ausnahmefällen == 0.
So wie es jetzt scheint, findet die Kollisionsprüfung beim Senden nicht statt.
*/

  cli();
  if (bus_type != 2) {
    for (i=0; i < msg[len_idx]+bus_type; i++) {	// same msg length difference as above
      data = msg[i] ^ 0xFF;
      if (serial->write(data) != 1) {
        // Collision
        sei();
        goto retry;
      }
    }
  } else {
    for (i=0; i <= len; i++) {
      data = msg[i];
      if (serial->write(data) != 1) {
        // Collision
        sei();
        goto retry;
      }
    }
  }
  sei();
  return true;
}

bool BSB::Send(uint8_t type, uint32_t cmd, byte* rx_msg, byte* tx_msg, byte* param, byte param_len, bool wait_for_reply) {
  byte i;

  if (bus_type == 2) {
    return _send(tx_msg);
  }

  // first two bytes are swapped
  byte A2 = (cmd & 0xff000000) >> 24;
  byte A1 = (cmd & 0x00ff0000) >> 16;
  byte A3 = (cmd & 0x0000ff00) >> 8;
  byte A4 = (cmd & 0x000000ff);
  
  if (bus_type == 1) {
    tx_msg[1] = param_len + 14;
    tx_msg[4] = 0xC0;	// some kind of sending/receiving flag?
    tx_msg[5] = 0x02;	// yet unknown
    tx_msg[6] = 0x00;	// yet unknown
    tx_msg[7] = 0x14;	// yet unknown
    tx_msg[8] = type;
    // Adress
    tx_msg[9] = A1;
    tx_msg[10] = A2;
    tx_msg[11] = A3;
    tx_msg[12] = A4;
  } else {
    tx_msg[3] = param_len + 11;
    tx_msg[4] = type;
    // Adress
    tx_msg[5] = A1;
    tx_msg[6] = A2;
    tx_msg[7] = A3;
    tx_msg[8] = A4;
  }

  // Value
  for (i=0; i < param_len; i++) {
    if (bus_type == 1) {
      tx_msg[13+i] = param[i];
    } else {
      tx_msg[9+i] = param[i];
    }
  }
  if(!_send(tx_msg)) return false;
  if(!wait_for_reply) return true;

  i=15;

  unsigned long timeout = millis() + 3000;
  while ((i > 0) && (millis() < timeout)) {
    if (GetMessage(rx_msg)) {
      Serial.print(F("Duration: "));
      Serial.println(3000-(timeout-millis()));

      i--;
      if (bus_type == 1) {
/* Activate for LPB systems with truncated error messages (no commandID in return telegram) 
	if (rx_msg[2] == myAddr && rx_msg[8]==0x08) {  // TYPE_ERR
	  return false;
	}
*/
        if (rx_msg[2] == myAddr && rx_msg[9] == A2 && rx_msg[10] == A1 && rx_msg[11] == A3 && rx_msg[12] == A4) {
          return true;
	}
      } else {
        if ((rx_msg[2] == myAddr) && (rx_msg[5] == A2) && (rx_msg[6] == A1) && (rx_msg[7] == A3) && (rx_msg[8] == A4)) {
          return true;
	}
      }
    }
    else {
      delayMicroseconds(205);
    }
  }
  Serial.println(F("Timeout waiting for answer..."));
  return false;
}
