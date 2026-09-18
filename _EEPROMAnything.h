//
//---------------------------------------------------------------------------------
// EEPROM read write any size of variable
//---------------------------------------------------------------------------------
//

#include <EEPROM.h>
//#include <Arduino.h>  // for type definitions

#if defined(ARDUINO) && ARDUINO >= 100
#include <Arduino.h>
#else
#include <WProgram.h>
#endif


template <class T> int EEPROM_writeAnything(int ee, const T& value)
{
    const byte* p = (const byte*)(const void*)&value;
    unsigned int i;
    for (i = 0; i < sizeof(value); i++)
          EEPROM.write(ee++, *p++);
#if defined(ESP32) || defined(PLATFORM_ESP32S3_TOUCH)
    EEPROM.commit();
#endif
    return i;
}

template <class T> int EEPROM_readAnything(int ee, T& value)
{
    byte* p = (byte*)(void*)&value;
    unsigned int i;
    for (i = 0; i < sizeof(value); i++)
          *p++ = EEPROM.read(ee++);
    return i;
}
