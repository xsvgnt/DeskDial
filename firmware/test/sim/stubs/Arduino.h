// Simulator stand-in for the Arduino core: a fake clock and serial port.
#pragma once
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#ifdef __cplusplus
#include <algorithm>
#include <deque>
#include <vector>
using std::min;
extern "C" {
#endif
uint32_t millis(void);
#ifdef __cplusplus
}
void delay(uint32_t ms); // advances the fake clock
extern int sim_cpu_mhz;
static inline bool setCpuFrequencyMhz(uint32_t m) { sim_cpu_mhz = m; return true; }
struct FakeSerial
{
  std::deque<uint8_t> rx; // what the "PC" sent
  std::vector<uint8_t> tx; // what the board sent
  void begin(unsigned long) {}
  void setRxBufferSize(size_t) {}
  void setTxTimeoutMs(uint32_t) {}
  int available() { return (int)rx.size(); }
  size_t read(uint8_t *b, size_t n)
  {
    size_t k = 0;
    while (k < n && !rx.empty())
    {
      b[k++] = rx.front();
      rx.pop_front();
    }
    return k;
  }
  size_t write(const uint8_t *b, size_t n)
  {
    tx.insert(tx.end(), b, b + n);
    return n;
  }
  void print(const char *s) { fputs(s, stdout); }
  void println(const char *s) { puts(s); }
};
extern FakeSerial Serial;
#endif
