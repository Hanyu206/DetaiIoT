#include "secrets.h"

#define BLYNK_TEMPLATE_ID   "TMPL6vGjltW6r"
#define BLYNK_TEMPLATE_NAME "DenGiaoThongIoT"
#define BLYNK_AUTH_TOKEN    SECRET_AUTH_TOKEN
#define BLYNK_PRINT Serial

#include <WiFi.h>
#include <WiFiClient.h>
#include <BlynkSimpleEsp32.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = SECRET_WIFI_SSID;
char pass[] = SECRET_WIFI_PASS;

// ===================== CHÂN GPIO - HƯỚNG A (Bắc - Nam) =====================
#define LED_A_GREEN   16
#define LED_A_YELLOW  17
#define LED_A_RED     19

// ===================== CHÂN GPIO - HƯỚNG B (Đông - Tây) ====================
// GPIO 25, 26, 27 là các chân GPIO thông thường, an toàn để xuất tín hiệu,
// không thuộc dải chỉ nhận đầu vào (34-39) và không phải chân cấu hình boot (0,2,12,15)
#define LED_B_GREEN   25
#define LED_B_YELLOW  26
#define LED_B_RED     27

LiquidCrystal_I2C lcd(0x27, 16, 2);

// ===================== BIẾN TRẠNG THÁI =====================
int soXe_A = 0;          // Số xe hướng A, nhận từ V0
int soXe_B = 0;          // Số xe hướng B, nhận từ V5
int uuTien = 0;           // 0 = không ưu tiên | 1 = ưu tiên hướng A | 2 = ưu tiên hướng B, nhận từ V2

const int THOI_GIAN_VANG = 3;      // Thời gian đèn vàng cố định (giây)
const int THOI_GIAN_AN_TOAN = 10;  // Thời gian xanh mặc định khi mất kết nối (giây)

// ===================== NHẬN DỮ LIỆU TỪ BLYNK =====================
BLYNK_WRITE(V0) {              // Số xe hướng A (từ AI)
  soXe_A = param.asInt();
}

BLYNK_WRITE(V5) {              // Số xe hướng B (từ AI)
  soXe_B = param.asInt();
}

BLYNK_WRITE(V2) {              // Cờ ưu tiên: 0=không, 1=ưu tiên A, 2=ưu tiên B
  uuTien = param.asInt();
  Blynk.virtualWrite(V3, "ACK");
}

BLYNK_CONNECTED() {
  Blynk.syncVirtual(V0);
  Blynk.syncVirtual(V5);
  Blynk.syncVirtual(V2);
}

bool isOnline() {
  return (WiFi.status() == WL_CONNECTED) && Blynk.connected();
}

// Phân ngưỡng thời gian xanh theo mật độ xe (giữ nguyên thuật toán gốc, dùng chung cho cả 2 hướng)
int tinhThoiGianXanh(int N, const char* &matDoOut) {
  if (N >= 12) { matDoOut = "CAO     "; return 15; }
  if (N >= 6)  { matDoOut = "TB "; return 8;  }
  matDoOut = "THAP    ";
  return 5;
}

// ===================== VÒNG PHA CHO 1 HƯỚNG =====================
// huong: 1 = A đang chạy (B giữ đỏ) | 2 = B đang chạy (A giữ đỏ)
// Đèn đỏ của hướng đối diện luôn được giữ trong ĐÚNG khoảng thời gian
// xanh + vàng của hướng đang chạy -> đây là điểm sửa lỗi so với bản cũ
// (bản cũ đèn đỏ luôn cố định 10s bất kể hướng còn lại chạy bao lâu).
void chayPha(int huong) {
  int ledXanhChinh, ledVangChinh, ledDoChinh;
  int ledXanhDoi, ledVangDoi, ledDoDoi;
  int soXeChinh;
  int vPinTrangThaiChinh, vPinThoiGianChinh, vPinDemLuiChinh;
  int vPinTrangThaiDoi;
  char kyHieuChinh, kyHieuDoi;

  if (huong == 1) {
    ledXanhChinh = LED_A_GREEN;  ledVangChinh = LED_A_YELLOW;  ledDoChinh = LED_A_RED;
    ledXanhDoi   = LED_B_GREEN;  ledVangDoi   = LED_B_YELLOW;  ledDoDoi   = LED_B_RED;
    soXeChinh = soXe_A;
    vPinTrangThaiChinh = V3; vPinThoiGianChinh = V1; vPinDemLuiChinh = V4;
    vPinTrangThaiDoi = V7;
    kyHieuChinh = 'A'; kyHieuDoi = 'B';
  } else {
    ledXanhChinh = LED_B_GREEN;  ledVangChinh = LED_B_YELLOW;  ledDoChinh = LED_B_RED;
    ledXanhDoi   = LED_A_GREEN;  ledVangDoi   = LED_A_YELLOW;  ledDoDoi   = LED_A_RED;
    soXeChinh = soXe_B;
    vPinTrangThaiChinh = V7; vPinThoiGianChinh = V6; vPinDemLuiChinh = V8;
    vPinTrangThaiDoi = V3;
    kyHieuChinh = 'B'; kyHieuDoi = 'A';
  }

  bool online = isOnline();
  const char* matDo;
  int thoiGianXanh = online ? tinhThoiGianXanh(soXeChinh, matDo) : THOI_GIAN_AN_TOAN;
  if (!online) matDo = "OFF ";

  // Hướng đối diện chuyển sang đỏ NGAY khi hướng chính bắt đầu xanh
  digitalWrite(ledDoDoi, HIGH);
  digitalWrite(ledVangDoi, LOW);
  digitalWrite(ledXanhDoi, LOW);
  if (online) Blynk.virtualWrite(vPinTrangThaiDoi, "DO");

  // ---- PHA XANH (hướng chính) ----
  digitalWrite(ledXanhChinh, HIGH);
  digitalWrite(ledVangChinh, LOW);
  digitalWrite(ledDoChinh, LOW);
  if (online) {
    Blynk.virtualWrite(vPinTrangThaiChinh, "XANH");
    Blynk.virtualWrite(vPinThoiGianChinh, thoiGianXanh);
  }

  int i = thoiGianXanh;
  while (i > 0) {
    if (WiFi.status() == WL_CONNECTED) Blynk.run();
    online = isOnline();

    // Xe ưu tiên đang ở CHÍNH hướng này -> giữ đèn xanh, không đếm lùi
    if (uuTien == huong && online) {
      Blynk.virtualWrite(vPinTrangThaiChinh, "UU TIEN");
      Blynk.virtualWrite(vPinDemLuiChinh, 99);
      lcd.setCursor(0, 0);
      lcd.print(kyHieuChinh); lcd.print("-UU TIEN!       ");
      lcd.setCursor(0, 1);
      lcd.print("XE UU TIEN      ");
      delay(500);
      continue; // không giảm i, giữ pha xanh cho tới khi hết cờ ưu tiên
    }

    // Xe ưu tiên đang ở hướng ĐỐI DIỆN -> rút ngắn pha xanh hiện tại để nhường đường
    if (uuTien == (huong == 1 ? 2 : 1) && online) {
      i = 1; // chuyển nhanh sang pha vàng ở vòng lặp kế
    }

    if (online) Blynk.virtualWrite(vPinDemLuiChinh, i);

    char line0[17], line1[17];
    snprintf(line0, sizeof(line0), "%c-X:%-2d %c-D:%-2d", kyHieuChinh, i, kyHieuDoi, i + THOI_GIAN_VANG);
    snprintf(line1, sizeof(line1), "N:%-2d CHE DO:%s", soXeChinh, online ? matDo : "OFF ");
    lcd.setCursor(0, 0); lcd.print(line0);
    lcd.setCursor(0, 1); lcd.print(line1);

    delay(1000);
    i--;
  }

  if (uuTien == huong && isOnline()) return; // vẫn đang ưu tiên, giữ nguyên pha xanh, bỏ qua vàng/đỏ

  // ---- PHA VÀNG (hướng chính) ----
  digitalWrite(ledXanhChinh, LOW);
  digitalWrite(ledVangChinh, HIGH);
  if (online) Blynk.virtualWrite(vPinTrangThaiChinh, "VANG");

  for (int j = THOI_GIAN_VANG; j > 0; j--) {
    if (WiFi.status() == WL_CONNECTED) Blynk.run();
    online = isOnline();
    if (online) Blynk.virtualWrite(vPinDemLuiChinh, j);

    char line0[17], line1[17];
    snprintf(line0, sizeof(line0), "%c-V:%-2d %c-D:%-2d", kyHieuChinh, j, kyHieuDoi, j);
    snprintf(line1, sizeof(line1), "CHUAN BI DUNG   ");
    lcd.setCursor(0, 0); lcd.print(line0);
    lcd.setCursor(0, 1); lcd.print(line1);

    delay(1000);
  }

  // ---- KẾT THÚC PHA: hướng chính chuyển đỏ, chờ vòng lặp gọi hướng còn lại ----
  digitalWrite(ledVangChinh, LOW);
  digitalWrite(ledDoChinh, HIGH);
  if (isOnline()) Blynk.virtualWrite(vPinTrangThaiChinh, "DO");
}

void setup() {
  Serial.begin(115200);

  pinMode(LED_A_GREEN, OUTPUT);  pinMode(LED_A_YELLOW, OUTPUT);  pinMode(LED_A_RED, OUTPUT);
  pinMode(LED_B_GREEN, OUTPUT);  pinMode(LED_B_YELLOW, OUTPUT);  pinMode(LED_B_RED, OUTPUT);

  // Trạng thái khởi tạo an toàn: cả 2 hướng đều đỏ cho tới khi vào vòng lặp đầu tiên
  digitalWrite(LED_A_RED, HIGH); digitalWrite(LED_A_YELLOW, LOW); digitalWrite(LED_A_GREEN, LOW);
  digitalWrite(LED_B_RED, HIGH); digitalWrite(LED_B_YELLOW, LOW); digitalWrite(LED_B_GREEN, LOW);

  Wire.begin(21, 22);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Dang khoi dong...");

  WiFi.begin(ssid, pass);
  Blynk.config(auth);
  delay(1000);
  lcd.clear();
}

void loop() {
  chayPha(1); // Hướng A xanh + vàng, hướng B giữ đỏ trong cùng khoảng thời gian đó
  chayPha(2); // Hướng B xanh + vàng, hướng A giữ đỏ trong cùng khoảng thời gian đó
}