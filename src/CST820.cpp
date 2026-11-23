#include "CST820.h"

CST820::CST820(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin, int8_t int_pin)
{
    _sda = sda_pin;
    _scl = scl_pin;
    _rst = rst_pin;
    _int = int_pin;
}

void CST820::begin(void)
{
    // Initialize I2C
    if (_sda != -1 && _scl != -1)
    {
        Wire.begin(_sda, _scl);
    }
    else
    {
        Wire.begin();
    }

    // Int Pin Configuration
    if (_int != -1)
    {
        // pinMode(_int, OUTPUT);
        // digitalWrite(_int, HIGH); //高电平
        // delay(1);
        // digitalWrite(_int, LOW); //低电平
        // delay(1);
        pinMode(_int, INPUT); 
    }

    // Reset Pin Configuration
    if (_rst != -1)
    {
        pinMode(_rst, OUTPUT);
        digitalWrite(_rst, LOW);
        delay(10);
        digitalWrite(_rst, HIGH);
        delay(300);
    }

    // Initialize Touch
    i2c_write(0xFE, 0XFF); //禁止自动进入低功耗模式。
}

bool CST820::getTouch(uint16_t *x, uint16_t *y, uint8_t *gesture)
{
    // One burst so the finger count and the coordinates come from the same sample.
    uint8_t frame[6];
    if (i2c_read_continuous(0x01, frame, sizeof(frame)) != 0) {
        ++_read_errors;
        return false;
    }
    *gesture = frame[0];
    if (frame[1] == 0 || frame[1] > 2) return false;

    *x = ((frame[2] & 0x0f) << 8) | frame[3];
    *y = ((frame[4] & 0x0f) << 8) | frame[5];
    return true;
}

uint8_t CST820::i2c_read_continuous(uint8_t addr, uint8_t *data, uint32_t length)
{
  Wire.beginTransmission(I2C_ADDR_CST820);
  Wire.write(addr);
    if (Wire.endTransmission(true) != 0) return -1;
    if (Wire.requestFrom(I2C_ADDR_CST820, length) != length) return -1;
    for (uint32_t index = 0; index < length; ++index) {
    *data++ = Wire.read();
  }
  return 0;
}

void CST820::i2c_write(uint8_t addr, uint8_t data)
{
    Wire.beginTransmission(I2C_ADDR_CST820);
    Wire.write(addr);
    Wire.write(data);
    Wire.endTransmission();
}

uint8_t CST820::i2c_write_continuous(uint8_t addr, const uint8_t *data, uint32_t length)
{
  Wire.beginTransmission(I2C_ADDR_CST820);
  Wire.write(addr);
  for (int i = 0; i < length; i++) {
    Wire.write(*data++);
  }
  if ( Wire.endTransmission(true))return -1;
  return 0;
}