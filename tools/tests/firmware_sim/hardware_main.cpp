#include "Arduino.h"
#include <assert.h>
#include <string.h>

namespace sim {
uint32_t nowMs=0U;
GPIO_TypeDef ports[3] = {{0U}, {1U}, {2U}};
int modes[48] = {}, levels[48] = {};
unsigned swdOnly=0U, resetAssertions=0U, resetReleases=0U;
void (*ppsInterrupt)() = 0;
void (*gnssWrite)(const uint8_t *, size_t) = 0;
}
HardwareSerial Serial2;
size_t HardwareSerial::write(const uint8_t *data, size_t size) {
  tx.insert(tx.end(), data, data+size);
  if (this == &Serial2 && sim::gnssWrite) sim::gnssWrite(data, size);
  return size;
}

// 実際の setup/loop と復旧処理を同じ翻訳単位で実行する。
#include "../../../src/main.cpp"
#include "../../../test/fixtures/packets.h"

#if HOSHIGUARD_OLED
namespace {
unsigned oledBegins=0U;
bool beginBus(void *) { ++oledBegins; return true; }
bool startBus(void *, const uint8_t *, size_t, uint32_t) { return true; }
OledTransfer pollBus(void *, uint32_t) { return OledTransfer::Failed; }
void endBus(void *) {}
}
oled::Bus stm32OledBus() { return {0, beginBus, startBus, pollBus, endBus}; }
#endif

namespace {
void step(uint32_t now) { sim::nowMs=now; loop(); }
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
  return l1s;
}
void normalReception(bool ppsOnly) {
  setup();
  if (ppsOnly) {
    sim::nowMs=100U;
    sim::ppsInterrupt();
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
  sim::ppsInterrupt(); // 保持中の PPS 様ノイズを次の時刻 epoch に持ち越さない。
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
  sim::ppsInterrupt();
  step(170000U);
  assert(ppsState.sequence==2U); // ISR 通し番号は保持する。
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
    sim::ppsInterrupt();
    step(sim::nowMs);
    if (i<3U) assert(timeSync.quality()!=TimeQuality::UtcPpsConfirmed);
  }
  assert(timeSync.quality()==TimeQuality::UtcPpsConfirmed);
  for (uint32_t now=200000U;now<=1000000U;now+=1000U) step(now);
  assert(sim::resetAssertions==1U);
  assert(validateBridge()>0U);
}
#endif
}
int main(int argc, char **argv) {
  assert(argc==2);
  (void)bridgeTextSequence;
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
  printf("ハードウェア模擬試験: %s PASS\n",argv[1]);
}
