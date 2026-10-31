#include "Arduino.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "HardwareTimer.h"

namespace sim {
uint32_t nowMs=0U;
GPIO_TypeDef ports[3] = {{0U}, {1U}, {2U}};
int modes[48] = {}, levels[48] = {};
unsigned swdOnly=0U, resetAssertions=0U, resetReleases=0U;
void (*ppsInterrupt)() = 0;
void (*gnssWrite)(const uint8_t *, size_t) = 0;
bool continuousRx=false, stallRx=false, stallTx=false, stallOled=false;
unsigned rxReads=0U;
TIM_TypeDef tim1;
SimRcc rcc;
SimAfio afio;
SimExti exti;
SimIwdg iwdg;
uint32_t pclk2=72000000U;
uint32_t sysclk=72000000U, hclk=72000000U;
bool timerIrqEnabled[2] = {}, timerIrqPending[2] = {};
unsigned timerSetups=0U;
bool lsiEnabled=false;
}
timerObj_t *HardwareTimer_Handle[TIMER_NUM] = {};
SimCapture::operator uint32_t() const { TIM1->SR.bits &= ~TIM_SR_CC1IF; return bits; }
HardwareSerial Serial2;
int HardwareSerial::available() const {
  return this==&Serial2 && sim::continuousRx ? 1 : static_cast<int>(rx.size());
}
int HardwareSerial::read() {
  if (this==&Serial2) {
    if (sim::stallRx) throw sim::Stalled();
    ++sim::rxReads;
    if (sim::continuousRx && rx.empty()) return 0x55;
  }
  if (rx.empty()) return -1;
  const int value=rx.front(); rx.pop_front(); return value;
}
size_t HardwareSerial::write(const uint8_t *data, size_t size) {
  if (sim::stallTx) throw sim::Stalled();
  tx.insert(tx.end(), data, data+size);
  if (this == &Serial2 && sim::gnssWrite) sim::gnssWrite(data, size);
  return size;
}

// 実際の setup/loop と復旧処理を同じ翻訳単位で実行する。
#include "../../../src/main.cpp"
#include "../../../test/fixtures/packets.h"
#if defined(HOSHIGUARD_CLOCK_SIM)
#include "clock_scenarios.h"
#endif

#if HOSHIGUARD_OLED
namespace {
#if defined(HOSHIGUARD_REAL_OLED)
unsigned &oledBegins=sim::i2cBegins;
#else
unsigned oledBegins=0U;
bool beginBus(void *) { ++oledBegins; return true; }
bool startBus(void *, const uint8_t *, size_t, uint32_t) { return true; }
OledTransfer pollBus(void *, uint32_t) {
  if (sim::stallOled) throw sim::Stalled();
  return OledTransfer::Failed;
}
void endBus(void *) {}
#endif
}
#if !defined(HOSHIGUARD_REAL_OLED)
oled::Bus stm32OledBus() { return {0, beginBus, startBus, pollBus, endBus}; }
#endif
#endif

namespace {
void step(uint32_t now) {
  sim::nowMs=now;
  const unsigned feeds=sim::iwdg.feeds;
  loop();
#if HOSHIGUARD_IWDG
  assert(sim::iwdg.feeds==feeds+1U);
  assert(sim::iwdg.lastFeed==now);
#else
  assert(sim::iwdg.feeds==feeds);
#endif
}
void latchPps(uint16_t counter) {
  if ((TIM1->CCER & TIM_CCER_CC1E) == 0U) return;
  if ((TIM1->SR.bits & TIM_SR_CC1IF) != 0U) TIM1->SR.bits |= TIM_SR_CC1OF;
  TIM1->CCR1.bits=counter;
  TIM1->SR.bits |= TIM_SR_CC1IF;
}
void pulse() {
  latchPps(static_cast<uint16_t>(static_cast<uint64_t>(sim::nowMs)*72000U));
  EXTI->PR=0U; // HAL_GPIO_EXTI_IRQHandler はコールバック前に消去する。
  assert(sim::ppsInterrupt);
  sim::ppsInterrupt();
}
#if HOSHIGUARD_GNSS_RESET
void inject(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t length) {
  uint8_t bytes[264];
  const size_t size=UbxParser::buildPacket(cls,id,payload,length,bytes,sizeof(bytes));
  assert(size != 0U);
  Serial2.rx.insert(Serial2.rx.end(),bytes,bytes+size);
}
// 設定値を RAM から読み戻す MAX-M10S の合成応答。実機の代替証拠ではない。
void respond(const uint8_t *data, size_t size) {
  if (size<8U) return;
  if (data[2]==0x0AU && data[3]==0x04U) {
    uint8_t version[40] = {};
    inject(0x0AU,0x04U,version,sizeof(version));
  } else if (data[2]==0x06U && data[3]==0x8BU && size>=16U) {
    const uint32_t key=bridge::getU32Le(data+10U);
    size_t count=0U;
    const ConfigKeySpec *items=gnssConfigItems(count);
    for (size_t i=0U;i<count;++i) if (items[i].key==key) {
      uint8_t value[12] = {1U,0U,0U,0U};
      bridge::putU32Le(value+4U,key);
      bridge::putU32Le(value+8U,items[i].targetValue);
      inject(0x06U,0x8BU,value,static_cast<uint16_t>(8U+items[i].width));
    }
  }
}
#endif
unsigned validateBridge() {
  for (unsigned i=0U;i<100U;++i) bridgeTransport.pump();
  size_t start=0U;
  unsigned frames=0U, l1s=0U;
  for (size_t i=0U;i<DebugUart.tx.size();++i) if (DebugUart.tx[i]==0U) {
    uint8_t logical[bridge::kMaximumLogicalFrame];
    size_t length=0U;
    assert(bridge::cobsDecode(DebugUart.tx.data()+start,i-start,logical,sizeof(logical),length));
    bridge::DecodedFrame frame = {};
    assert(bridge::decodeLogicalFrame(logical,length,frame));
    assert(frame.header.sequence==frames++);
    if (frame.header.frameType==bridge::FrameType::L1sFrame) ++l1s;
    start=i+1U;
  }
  assert(start==DebugUart.tx.size());
  assert(frames>0U);
  const char *wirePath=getenv("HOSHIGUARD_SIM_WIRE");
  if (wirePath) {
    FILE *wire=fopen(wirePath,"wb");
    assert(wire);
    assert(fwrite(DebugUart.tx.data(),1U,DebugUart.tx.size(),wire)==DebugUart.tx.size());
    assert(fclose(wire)==0);
  }
  return l1s;
}
void normalReception(bool ppsOnly) {
  setup();
  if (ppsOnly) {
    sim::nowMs=100U;
    pulse();
  } else {
    // 実 fixture の UBX は、有効な受信実績として自動復旧を中止させる。
    Serial2.rx.insert(Serial2.rx.end(),kRealFixtures[0].data,kRealFixtures[0].data+kRealFixtures[0].length);
  }
  for (uint32_t now=100U;now<=400000U;now+=100U) step(now);
  assert(sim::resetAssertions==0U);
  assert(Serial2.beginCount==3U); // 元の baud 探索のみ。
  validateBridge();
}
#if HOSHIGUARD_GNSS_RESET
void silentRecovery() {
  setup();
  for (uint32_t now=0U;now<=149999U;now+=1U) step(now);
  assert(Serial2.beginCount==6U); // 各 3 baud、ソフトウェア再試行 1 回。
  assert(sim::resetAssertions==0U);
  // 半端な UBX と古い関連付けを意図的に置き、復旧時の無効化を検証する。
  UbxPacketView view;
  ubx.feed(0xB5U,view);
  timeSync.setTimeGridKnown(true);
  timeSync.observePps(1U,1U);
  utc.valid=true;
  step(150000U);
  assert(sim::resetAssertions==1U);
  assert(sim::levels[PA15]==LOW);
  assert(!utc.valid && timeSync.newestPps()==0 && !timeSync.timeGridKnown());
  sim::nowMs=150009U;
  pulse(); // 保持中の PPS 様ノイズを次の時刻 epoch に持ち越さない。
  step(150009U);
  assert(sim::levels[PA15]==LOW);
  step(150010U);
  assert(sim::levels[PA15]==HIGH);
  // 保持・起動待ち中の入力を採用しない。
  uint8_t empty[40] = {};
  inject(0x0AU,0x04U,empty,sizeof(empty));
  step(151009U);
  assert(ubx.stats().validPackets==0U);
  step(151010U);
  assert(Serial2.baud==9600U && Serial2.beginCount==7U);
  assert(ubx.stats().validPackets==0U);
  assert(ppsState.sequence==0U && pps.healthStats().count==0U);
  sim::gnssWrite=respond;
  for (uint32_t now=151011U;now<170000U;++now) step(now);
  assert(gnss.configurationFinished());
  assert(gnss.configEvidence(CFG_SIGNAL_QZSS_L1S_ENA)->result==ConfigResult::Verified);
  assert(gnss.timePulseConfigAllVerified());
  // 回復後に実 UBX fixture を再生し、PPS の通し番号も維持する。
  const RealFixture &fixture=kRealFixtures[0];
  Serial2.rx.insert(Serial2.rx.end(),fixture.data,fixture.data+fixture.length);
  sim::nowMs=170000U;
  pulse();
  step(170000U);
  assert(ppsState.sequence==2U); // ISR 通し番号は保持する。
#if USE_TIM1_INPUT_CAPTURE
  assert(ppsState.hardware.captureValid && !ppsState.hardware.intervalModuloValid);
#endif
  for (uint32_t now=170001U;now<170100U;++now) step(now);
  // 合成 TIM-TP と実入口の PPS を組み合わせ、復旧後に改めて 3 回の一致を要求する。
  for (uint32_t i=1U;i<=3U;++i) {
    uint8_t timing[16] = {};
    bridge::putU32Le(timing,i*1000U);
    bridge::putU16Le(timing+12U,2400U);
    timing[14]=0x03U;
    timing[15]=0x40U;
    inject(0x0DU,0x01U,timing,sizeof(timing));
    step(170000U+i*1000U-100U);
    sim::nowMs=170000U+i*1000U;
    pulse();
    step(sim::nowMs);
    if (i<3U) assert(timeSync.quality()!=TimeQuality::UtcPpsConfirmed);
  }
  assert(timeSync.quality()==TimeQuality::UtcPpsConfirmed);
  for (uint32_t now=200000U;now<=1000000U;now+=1000U) step(now);
  assert(sim::resetAssertions==1U);
  assert(validateBridge()>0U);
}
#endif
#if USE_TIM1_INPUT_CAPTURE
void captureEvidence() {
  setup();
  assert(pps.hardwareActive());
  assert(TIM1->PSC==0U && TIM1->ARR==65535U && TIM1->RCR==0U && TIM1->DIER==0U);
  assert(!sim::timerIrqEnabled[0] && !sim::timerIrqEnabled[1]);
  assert(!sim::timerIrqPending[0] && !sim::timerIrqPending[1]);
  PpsSnapshot out = {};
  assert(!pps.update(out));
  const uint32_t times[] = {1000U,2000U,3000U,6000U,6001U,7001U,4294000U,4295000U};
  uint32_t previous=0U;
  unsigned sequence=0U;
  uint16_t previousCounter=0U;
  for (uint32_t now : times) {
    sim::nowMs=now;
    const uint32_t timestamp=micros();
    const uint16_t counter=static_cast<uint16_t>(static_cast<uint64_t>(now)*72000U-17U);
    latchPps(counter);
    TIM1->SR.bits |= 1U; // 更新未処理・同時発生を含めて UIF は保持する。
    sim::ppsInterrupt();
    assert(pps.update(out));
    assert(out.sequence==++sequence && out.edgeMicros==timestamp);
    assert(out.hardware.captureValid && out.hardware.capturedCounter==counter);
    assert(out.hardware.intervalModuloValid==(sequence>1U));
    if (sequence>1U) {
      assert(out.intervalMicros==timestamp-previous);
      assert(out.hardware.intervalModuloTicks==static_cast<uint16_t>(counter-previousCounter));
    }
    assert(TIM1->SR.bits==1U);
    assert(!pps.update(out));
    previous=timestamp;
    previousCounter=counter;
  }
  assert(out.lostCount>0U && out.anomalyCount>0U);
  // 二つの捕捉が ISR より先行した場合、時刻経路の動作は維持し証跡だけ無効化する。
  latchPps(5U); latchPps(6U);
  sim::ppsInterrupt();
  assert(pps.update(out) && out.sequence==++sequence);
  assert(out.edgeMicros==micros() && !out.hardware.captureValid && out.hardware.overcapture);
  assert(TIM1->SR.bits==1U);
  latchPps(7U); sim::ppsInterrupt();
  assert(pps.update(out) && !out.hardware.intervalModuloValid);
  ++sequence;
  // ES096: CCR 読み出しと競合したエッジの EXTI pending が残る場合。
  latchPps(8U); EXTI->PR=GPIO_PIN_8; sim::ppsInterrupt();
  assert(pps.update(out) && out.hardware.readRace && !out.hardware.captureValid);
  EXTI->PR=0U;
  ++sequence;
  // 主ループが二つの ISR をまとめて読んだ場合も、間隔の連続性を捏造しない。
  pulse(); pulse();
  assert(pps.update(out) && out.sequence==sequence+2U && !out.hardware.intervalModuloValid);
  sequence=out.sequence;
  pulse();
  pps.begin();
  assert(!pps.update(out) && sim::timerSetups==1U);
  pulse();
  assert(pps.update(out) && out.sequence==sequence+2U && !out.hardware.intervalModuloValid);
  assert(pps.healthStats().count==1U);
#if HOSHIGUARD_GNSS_RESET
  pulse(); pps.discardHistory();
  assert(!pps.update(out));
  pulse(); assert(pps.update(out) && !out.hardware.intervalModuloValid);
#endif
}

void captureApbClock(bool divided) {
  sim::pclk2=divided ? 36000000U : 72000000U;
  RCC->CFGR=(RCC->CFGR & ~RCC_CFGR_PPRE2) | ((divided ? 4U : 3U) << 11);
  setup();
  assert(pps.hardwareActive() && TIM1->PSC==0U);
}

void captureUnavailable(const char *reason) {
  timerObj_t other = {};
  if (!strcmp(reason,"timer-owner")) HardwareTimer_Handle[0]=&other;
  else if (!strcmp(reason,"timer-clock")) sim::pclk2=71000001U;
  else if (!strcmp(reason,"timer-8mhz")) sim::sysclk=sim::hclk=sim::pclk2=8000000U;
  else if (!strcmp(reason,"timer-48mhz")) sim::sysclk=sim::hclk=sim::pclk2=48000000U;
  else if (!strcmp(reason,"timer-64mhz")) sim::sysclk=sim::hclk=sim::pclk2=64000000U;
  else if (!strcmp(reason,"timer-stale-clock")) sim::sysclk=36000000U;
  else if (!strcmp(reason,"timer-ahb")) RCC->CFGR |= 8U << 4U;
  else if (!strcmp(reason,"timer-apb4")) { sim::pclk2=18000000U; RCC->CFGR |= 5U << 11U; }
  else if (!strcmp(reason,"timer-unlocked")) RCC->CR &= ~RCC_CR_PLLRDY;
  else if (!strcmp(reason,"timer-hse-unready")) RCC->CR &= ~RCC_CR_HSERDY;
  else if (!strcmp(reason,"timer-switching")) RCC->CFGR &= ~RCC_CFGR_SW;
  else if (!strcmp(reason,"timer-reset")) RCC->APB2RSTR=RCC_APB2RSTR_TIM1RST;
  else if (!strcmp(reason,"timer-remap")) AFIO->MAPR=AFIO_MAPR_TIM1_REMAP;
  else RCC->APB2ENR=RCC_APB2ENR_TIM1EN;
  setup();
  assert(!pps.hardwareActive() && sim::timerSetups==0U);
  sim::nowMs=1000U; pulse();
  PpsSnapshot out = {};
  assert(pps.update(out) && out.edgeMicros==1000000U && out.sequence==1U);
  assert(!out.hardware.captureValid && out.hardware.unavailable);
}

void captureRuntimeFaults() {
  setup();
  timerObj_t other = {};
  for (unsigned fault=0U; fault<22U; ++fault) {
    assert(pps.hardwareActive());
    pulse();
    PpsSnapshot out = {};
    assert(pps.update(out) && out.hardware.captureValid);
    const TIM_TypeDef savedTimer=*TIM1;
    const SimRcc savedRcc=*RCC;
    timerObj_t *const savedOwner=HardwareTimer_Handle[0];
    switch (fault) {
      case 0: TIM1->PSC=71U; break;
      case 1: TIM1->ARR=1000U; break;
      case 2: TIM1->RCR=1U; break;
      case 3: TIM1->CR1=0U; break;
      case 4: TIM1->CR1 |= 1U << 4; break;
      case 5: TIM1->CR2=0x20U; break;
      case 6: TIM1->SMCR=7U; break;
      case 7: TIM1->CCMR1 |= 1U << 4; break;
      case 8: TIM1->CCMR2=1U; break;
      case 9: TIM1->CCER |= 2U; break;
      case 10: TIM1->DIER=1U; break;
      case 11: TIM1->BDTR=1U; break;
      case 12: RCC->CFGR ^= 4U << 11; break;
      case 13: RCC->CR &= ~RCC_CR_PLLRDY; break;
      case 14: RCC->APB2ENR=0U; break;
      case 15: RCC->APB2RSTR=RCC_APB2RSTR_TIM1RST; break;
      case 16: AFIO->MAPR=AFIO_MAPR_TIM1_REMAP; break;
      case 17: HardwareTimer_Handle[0]=&other; break;
      case 18: sim::timerIrqEnabled[0]=true; break;
      case 19: sim::timerIrqEnabled[1]=true; break;
      case 20: sim::hclk=36000000U; break;
      case 21: HardwareTimer_Handle[0]=0; break;
    }
    const uint32_t dier=TIM1->DIER, ccer=TIM1->CCER;
    pulse();
    assert(pps.update(out) && !out.hardware.captureValid && out.hardware.unavailable);
    assert(!out.hardware.intervalModuloValid && !pps.hardwareActive());
    assert(out.edgeMicros==micros());
    // 故障検出時に他者の設定・割り込みを変更しない。
    assert(TIM1->DIER==dier && TIM1->CCER==ccer);
    if (fault==17U || fault==21U) {
      const unsigned setups=sim::timerSetups;
      pps.begin();
      assert(!pps.hardwareActive() && sim::timerSetups==setups);
    }
    *TIM1=savedTimer;
    *RCC=savedRcc;
    AFIO->MAPR=0U;
    HardwareTimer_Handle[0]=savedOwner;
    sim::hclk=72000000U;
    sim::timerIrqEnabled[0]=sim::timerIrqEnabled[1]=false;
    pulse();
    assert(pps.update(out) && out.hardware.unavailable); // 元に戻すだけでは再開しない。
    pps.begin();
    assert(pps.hardwareActive() && !pps.update(out));
    pulse();
    assert(pps.update(out) && out.hardware.captureValid && !out.hardware.intervalModuloValid);
    pulse();
    assert(pps.update(out) && out.hardware.intervalModuloValid);
  }
  assert(sim::timerSetups==1U);
}
#endif

#if HOSHIGUARD_IWDG
void watchdogProgress() {
  // 診断を実装していないので、起動原因フラグも読み替え・消去しない。
  RCC->CSR=0xFC000000U;
  setup();
  assert(sim::iwdg.enabled && sim::lsiEnabled);
  assert(sim::iwdg.prescaler==6U && sim::iwdg.reload==3749U);
  assert(RCC->CSR==0xFC000000U);
  sim::continuousRx=true;
  for (uint32_t now=1U;now<=40000U;now+=100U) {
    const unsigned reads=sim::rxReads;
    step(now);
    // GNSS が baud を変更する回では、その排出にも最大 256 byte を許可する。
    assert(sim::rxReads-reads>=256U && sim::rxReads-reads<=512U);
  }
  sim::continuousRx=false;
  // Bridge の背圧は外部の状態なので、処理が戻ればリロードを続ける。
  DebugUart.room=0;
  for (uint32_t now=40001U;now<80000U;now+=100U) step(now);
  const unsigned feeds=sim::iwdg.feeds;
  // ISR だけは動いても、主ループが止まれば reload は増えない。
  sim::nowMs=120000U; pulse();
  assert(sim::iwdg.feeds==feeds && sim::nowMs-sim::iwdg.lastFeed>32000U);
}

void watchdogInterruptedPass(const char *where) {
  setup();
  const unsigned feeds=sim::iwdg.feeds;
  bool stalled=false;
  if (!strcmp(where,"stall-rx")) { Serial2.rx.push_back(0x55U); sim::stallRx=true; }
  else if (!strcmp(where,"stall-tx")) sim::stallTx=true;
  else sim::stallOled=true;
  try {
    // OLED の非同期初期化が poll 段階に進むまで最大 10 回だけ実行する。
    for (unsigned i=0U;i<10U;++i) {
      sim::nowMs=1000U+i;
      const unsigned before=sim::iwdg.feeds;
      try { loop(); } catch (const sim::Stalled &) {
        assert(sim::iwdg.feeds==before);
        throw;
      }
    }
  } catch (const sim::Stalled &) { stalled=true; }
  assert(stalled);
  if (strcmp(where,"stall-oled")) assert(sim::iwdg.feeds==feeds);
}
#endif
}
int main(int argc, char **argv) {
  assert(argc==2);
  (void)bridgeTextSequence;
#if defined(HOSHIGUARD_CLOCK_SIM)
  if (!strncmp(argv[1],"clock-",6U)) {
    clockScenario(argv[1]);
    printf("クロック模擬試験: %s PASS\n",argv[1]);
    return 0;
  }
  initializeClock();
#endif
#if USE_TIM1_INPUT_CAPTURE
  if (!strcmp(argv[1],"capture")) captureEvidence();
  else if (!strcmp(argv[1],"capture-apb")) captureApbClock(true);
  else if (!strcmp(argv[1],"capture-apb-undivided")) captureApbClock(false);
  else if (!strcmp(argv[1],"capture-runtime")) captureRuntimeFaults();
  else if (!strncmp(argv[1],"timer-",6U)) captureUnavailable(argv[1]);
  else
#endif
#if HOSHIGUARD_IWDG
  if (!strcmp(argv[1],"watchdog")) watchdogProgress();
  else if (!strncmp(argv[1],"stall-",6U)) watchdogInterruptedPass(argv[1]);
  else
#endif
#if HOSHIGUARD_GNSS_RESET
  if (!strcmp(argv[1],"silent")) silentRecovery();
  else
#endif
  if (!strcmp(argv[1],"ubx")) normalReception(false);
  else if (!strcmp(argv[1],"pps")) normalReception(true);
  else {
    // 未使用の全スイッチが ON でも、baud とプロトコルには影響しない。
    const int unusedPins[] = {19,20,24,25,26,27};
    for (int pin : unusedPins) sim::levels[pin]=HIGH;
    sim::levels[PB12]=!strcmp(argv[1],"switch-on") ? HIGH : LOW;
    setup();
    for (uint32_t now=0U;now<1000U;++now) step(now);
#if HOSHIGUARD_OLED
    assert(oledBegins==(sim::levels[PB12]==HIGH ? 0U : 1U));
    assert(sim::modes[PB12]==INPUT_PULLDOWN);
#else
    assert(sim::modes[PB12]==0);
#endif
    for (int pin : unusedPins) assert(sim::modes[pin]==0);
    assert(DebugUart.baud==115200U);
    assert(sim::resetAssertions==0U);
    validateBridge();
  }
#if HOSHIGUARD_GNSS_RESET
  assert(sim::swdOnly==1U);
  assert(sim::modes[PA15]==GPIO_MODE_OUTPUT_OD);
#else
  assert(sim::swdOnly==0U && sim::modes[PA15]==0);
  assert(sim::resetAssertions==0U && sim::resetReleases==0U);
#endif
#if HOSHIGUARD_STATUS_LED
  assert(sim::modes[45]==GPIO_MODE_OUTPUT_OD);
#else
  assert(sim::modes[45]==0);
#endif
  assert(sim::modes[13]==0 && sim::modes[19]==0 && sim::modes[20]==0);
#if !USE_TIM1_INPUT_CAPTURE
  assert(sim::timerSetups==0U && TIM1->CR1==0U && TIM1->DIER==0U);
#endif
#if !HOSHIGUARD_IWDG
  assert(!sim::iwdg.enabled && !sim::lsiEnabled && sim::iwdg.feeds==0U);
#endif
  printf("ハードウェア模擬試験: %s PASS\n",argv[1]);
}
