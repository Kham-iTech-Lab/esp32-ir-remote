/*
 * ESP32 IR Remote — เปลี่ยนมือถือเป็นรีโมททีวี / แอร์ / พัดลม
 * K iTech Review (TikTok) • Kham iTech Lab (YouTube / Facebook)
 *
 * ทำอะไรได้
 *   1) เรียนรู้ปุ่มจากรีโมทเดิม (ตัวรับอินฟราเรด)
 *   2) ส่งปุ่มนั้นซ้ำ (LED อินฟราเรด + ทรานซิสเตอร์)
 *   3) กดจากมือถือผ่านหน้าเว็บใน Wi-Fi บ้าน
 *   ปุ่มที่เรียนรู้แล้วจะถูกจำไว้ในแฟลช ปิดเครื่องแล้วยังอยู่
 *
 * ไลบรารีที่ต้องติดตั้ง (Arduino IDE > Library Manager)
 *   - IRremoteESP8266  (by David Conran, crankyoldgit)
 *
 * การต่อวงจร
 *   ตัวรับ IR (VS1838B / TSOP38238):  VCC -> 3.3V, GND -> GND, OUT -> GPIO 14
 *   ตัวส่ง IR:
 *     GPIO 4 -> R 1kΩ -> ขา B ทรานซิสเตอร์ NPN (2N2222 / S8050)
 *     ขา E -> GND
 *     ขา C -> ขาสั้น (แคโทด) ของ LED อินฟราเรด
 *     ขายาว (แอโนด) ของ LED อินฟราเรด -> R 47Ω -> 5V (VIN)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <IRremoteESP8266.h>
#include <IRrecv.h>
#include <IRsend.h>
#include <IRutils.h>

// ------------------------------------------------------------------ ตั้งค่า
const char* WIFI_SSID = "ชื่อ Wi-Fi บ้าน";
const char* WIFI_PASS = "รหัส Wi-Fi";

const uint16_t PIN_IR_RECV = 14;
const uint16_t PIN_IR_LED  = 4;

// ปุ่มบนหน้าเว็บ (แก้ชื่อ/เพิ่มได้ สูงสุดตาม NUM_SLOTS)
const uint8_t NUM_SLOTS = 8;
const char* SLOT_NAMES[NUM_SLOTS] = {
  "Power", "Vol +", "Vol -", "Mute", "CH +", "CH -", "Input", "OK"
};

// ------------------------------------------------------------------ ตัวแปร
IRrecv irrecv(PIN_IR_RECV, 1024, 50, true);
IRsend irsend(PIN_IR_LED);
decode_results results;
WebServer server(80);
Preferences prefs;

struct IrCode {
  uint16_t type;     // decode_type_t
  uint64_t value;
  uint16_t bits;
};
IrCode slots[NUM_SLOTS];
int learningSlot = -1;          // -1 = ไม่ได้อยู่ในโหมดเรียนรู้
String lastSeen = "ยังไม่มี";

void saveSlot(uint8_t i);
void loadSlots();
bool slotReady(uint8_t i);
bool sendSlot(uint8_t i);
String page();
void handleRoot();
void handleSend();
void handleLearn();

// ------------------------------------------------------------------ เก็บ/โหลดปุ่ม
void saveSlot(uint8_t i) {
  prefs.begin("irslots", false);
  String k = "s" + String(i);
  prefs.putBytes(k.c_str(), &slots[i], sizeof(IrCode));
  prefs.end();
}

void loadSlots() {
  prefs.begin("irslots", true);
  for (uint8_t i = 0; i < NUM_SLOTS; i++) {
    String k = "s" + String(i);
    if (prefs.getBytes(k.c_str(), &slots[i], sizeof(IrCode)) != sizeof(IrCode)) {
      slots[i] = {(uint16_t)decode_type_t::UNKNOWN, 0, 0};
    }
  }
  prefs.end();
}

bool slotReady(uint8_t i) {
  return slots[i].type != (uint16_t)decode_type_t::UNKNOWN && slots[i].bits > 0;
}

// ------------------------------------------------------------------ ส่ง IR
bool sendSlot(uint8_t i) {
  if (i >= NUM_SLOTS || !slotReady(i)) return false;
  irrecv.disableIRIn();   // ไม่ให้ตัวรับอ่านสัญญาณของตัวเอง
  bool ok = irsend.send((decode_type_t)slots[i].type, slots[i].value, slots[i].bits);
  irrecv.enableIRIn();
  Serial.printf("ส่ง [%s] %s 0x%llX (%d bits) -> %s\n", SLOT_NAMES[i],
                typeToString((decode_type_t)slots[i].type).c_str(),
                slots[i].value, slots[i].bits, ok ? "OK" : "ไม่รองรับโปรโตคอลนี้");
  return ok;
}

// ------------------------------------------------------------------ หน้าเว็บ
String page() {
  String h = F("<!doctype html><html><head><meta charset='utf-8'>"
               "<meta name='viewport' content='width=device-width,initial-scale=1'>"
               "<title>ESP32 IR Remote</title><style>"
               "body{font-family:sans-serif;background:#0b1530;color:#fff;margin:0;padding:16px}"
               "h1{font-size:20px;margin:0 0 4px}.s{color:#9ab;font-size:13px;margin-bottom:14px}"
               ".g{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
               ".b{background:#1c2b55;border:2px solid #3cc8ff;border-radius:14px;padding:16px 8px;"
               "text-align:center;color:#fff;font-size:18px;text-decoration:none}"
               ".b.off{border-color:#555;color:#888}.l{display:block;font-size:12px;color:#ffcc44;margin-top:6px}"
               ".w{background:#3a2a00;border:1px solid #ffcc44;padding:10px;border-radius:10px;margin-bottom:12px}"
               "</style></head><body><h1>ESP32 IR Remote</h1>");
  h += "<div class='s'>สัญญาณล่าสุดที่รับได้: " + lastSeen + "</div>";
  if (learningSlot >= 0) {
    h += "<div class='w'>โหมดเรียนรู้ปุ่ม <b>" + String(SLOT_NAMES[learningSlot]) +
         "</b> — เล็งรีโมทเดิมมาที่ตัวรับ แล้วกดปุ่มหนึ่งครั้ง (หน้านี้รีเฟรชเองทุก 2 วิ)</div>"
         "<script>setTimeout(()=>location.href='/',2000)</script>";
  }
  h += "<div class='g'>";
  for (uint8_t i = 0; i < NUM_SLOTS; i++) {
    bool ok = slotReady(i);
    h += "<div><a class='b" + String(ok ? "" : " off") + "' style='display:block' href='/send?s=" + i + "'>" +
         SLOT_NAMES[i] + "</a><a class='l' href='/learn?s=" + i + "'>" + (ok ? "เรียนรู้ใหม่" : "กดเพื่อเรียนรู้") + "</a></div>";
  }
  h += "</div></body></html>";
  return h;
}

void handleRoot()  { server.send(200, "text/html; charset=utf-8", page()); }

void handleSend() {
  int i = server.arg("s").toInt();
  sendSlot(i);
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleLearn() {
  int i = server.arg("s").toInt();
  if (i >= 0 && i < NUM_SLOTS) learningSlot = i;
  server.sendHeader("Location", "/");
  server.send(303);
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  Serial.begin(115200);
  delay(200);
  irsend.begin();
  irrecv.enableIRIn();
  loadSlots();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("กำลังต่อ Wi-Fi");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.printf("\nเปิดในมือถือ: http://%s\n", WiFi.localIP().toString().c_str());

  server.on("/", handleRoot);
  server.on("/send", handleSend);
  server.on("/learn", handleLearn);
  server.begin();
}

void loop() {
  server.handleClient();

  if (irrecv.decode(&results)) {
    decode_type_t t = results.decode_type;
    if (t != decode_type_t::UNKNOWN && !results.repeat) {
      lastSeen = typeToString(t) + " 0x" + uint64ToString(results.value, 16) +
                 " (" + String(results.bits) + " bits)";
      Serial.println("รับได้: " + lastSeen);

      if (learningSlot >= 0) {
        if (hasACState(t)) {
          Serial.println("รีโมทแอร์ส่งข้อมูลยาว (state) — ตัวอย่างนี้ยังไม่รองรับ ดู README");
        } else {
          slots[learningSlot] = {(uint16_t)t, results.value, results.bits};
          saveSlot(learningSlot);
          Serial.printf("บันทึกลงปุ่ม [%s] แล้ว\n", SLOT_NAMES[learningSlot]);
          learningSlot = -1;
        }
      }
    }
    irrecv.resume();
  }
}
