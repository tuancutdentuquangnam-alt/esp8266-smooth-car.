#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

// Wi-Fi AP: connect phone to this network, then open http://192.168.4.1
const char* ssid = "Smooth_Car_WiFi";
const char* password = "123456789";
ESP8266WebServer server(80);

// L298N / compatible motor driver pins
#define ENA D5  // PWM right motor
#define ENB D6  // PWM left motor
#define IN1 D7  // right motor direction 1
#define IN2 D8  // right motor direction 2
#define IN3 D3  // left motor direction 1
#define IN4 D4  // left motor direction 2

// Lamp output: D0 / GPIO16. Use a transistor/MOSFET or relay module for a real lamp.
// Do NOT power a lamp directly from the ESP8266 pin.
#define LAMP_PIN D0
bool lampOn = false;

const int MAX_PWM = 1023;
const int DRIVE_PWM = 820;       // cruising speed
const int TURN_INNER_PWM = 430;  // slower inside wheel for gentle turns
const int accelStep = 12;        // smaller = smoother/slower
const unsigned long updateInterval = 15;

char carState = 'S';
int currentRight = 0;
int currentLeft = 0;
unsigned long lastUpdateTime = 0;

const String htmlPage = R"rawliteral(
<!DOCTYPE html><html><head>
<meta name="viewport" content="width=device-width, initial-scale=1, maximum-scale=1, user-scalable=no">
<meta charset="utf-8"><title>ESP8266 Smooth Car</title>
<style>
body{font-family:Arial,sans-serif;text-align:center;background:#20242a;color:white;margin:0;padding:18px;user-select:none}
h2{color:#00e6bd;font-size:22px}.hint{color:#c9d0d8;font-size:14px}
.grid{display:grid;grid-template-columns:repeat(3, minmax(72px,100px));gap:12px;justify-content:center;margin:24px auto}
button{height:88px;border:2px solid #00e6bd;border-radius:15px;background:#3b424b;color:white;font-size:25px;font-weight:bold;touch-action:none}
button:active{background:#00e6bd;color:#111}.stop{background:#c93b42;border-color:#ff6666}
.lamp{height:54px;width:min(310px,90%);font-size:18px;border-color:#ffc857}.empty{visibility:hidden}
</style></head><body>
<h2>ESP8266 SMOOTH CAR</h2>
<div class="hint">Giữ nút để chạy · thả nút để dừng</div>
<div class="grid">
<div class="empty"></div><button data-drive="F">▲</button><div class="empty"></div>
<button data-drive="L">◀</button><button class="stop" data-drive="S">■</button><button data-drive="R">▶</button>
<div class="empty"></div><button data-drive="B">▼</button><div class="empty"></div>
</div>
<button class="lamp" id="lamp">💡 BẬT / TẮT ĐÈN</button>
<script>
async function sendState(s){try{await fetch('/action?State='+s,{cache:'no-store'})}catch(e){}}
document.querySelectorAll('[data-drive]').forEach(b=>{
  const s=b.dataset.drive;
  b.addEventListener('pointerdown',e=>{e.preventDefault();b.setPointerCapture?.(e.pointerId);sendState(s)});
  const release=e=>{e.preventDefault();if(s!=='S')sendState('S')};
  b.addEventListener('pointerup',release);
  b.addEventListener('pointercancel',release);
  b.addEventListener('lostpointercapture',()=>{if(s!=='S')sendState('S')});
});
document.getElementById('lamp').addEventListener('click',async()=>{
  try{const r=await fetch('/lamp',{cache:'no-store'});document.getElementById('lamp').textContent=await r.text()}catch(e){}
});
</script></body></html>
)rawliteral";

void setMotorDirections(char state) {
  // L/R use differential steering: both wheels drive forward, inner wheel slower.
  // This produces a smoother arc instead of abruptly reversing one motor.
  switch (state) {
    case 'F':
    case 'L':
    case 'R':
      digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
      digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);
      break;
    case 'B':
      digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
      digitalWrite(IN3, LOW); digitalWrite(IN4, HIGH);
      break;
    case 'S':
    default:
      digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
      digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
      break;
  }
}

void handleRoot() { server.send(200, "text/html; charset=utf-8", htmlPage); }

void handleAction() {
  if (server.hasArg("State")) {
    String s = server.arg("State");
    if (s.length()) {
      char next = s.charAt(0);
      if (next=='F' || next=='B' || next=='L' || next=='R' || next=='S') {
        // Change direction only after a stop command; speed ramps down/up smoothly.
        carState = next;
      }
    }
  }
  server.send(200, "text/plain", "OK");
}

void handleLamp() {
  lampOn = !lampOn;
  digitalWrite(LAMP_PIN, lampOn ? HIGH : LOW);
  server.send(200, "text/plain; charset=utf-8", lampOn ? "💡 ĐÈN ĐANG BẬT" : "💡 ĐÈN ĐANG TẮT");
}

void setup() {
  Serial.begin(115200);
  pinMode(ENA, OUTPUT); pinMode(ENB, OUTPUT);
  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  pinMode(LAMP_PIN, OUTPUT);
  digitalWrite(LAMP_PIN, LOW);
  analogWriteRange(MAX_PWM);
  analogWriteFreq(1000);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);

  server.on("/", handleRoot);
  server.on("/action", handleAction);
  server.on("/lamp", handleLamp);
  server.begin();
}

void loop() {
  server.handleClient();
  applySmoothDrive();
}

void applySmoothDrive() {
  unsigned long now = millis();
  if (now - lastUpdateTime < updateInterval) return;
  lastUpdateTime = now;

  int targetRight = 0, targetLeft = 0;
  if (carState == 'F' || carState == 'B') {
    targetRight = DRIVE_PWM; targetLeft = DRIVE_PWM;
  } else if (carState == 'L') {
    targetRight = DRIVE_PWM; targetLeft = TURN_INNER_PWM;
  } else if (carState == 'R') {
    targetRight = TURN_INNER_PWM; targetLeft = DRIVE_PWM;
  }

  // Set direction for the requested mode; PWM ramps smoothly to the new target.
  setMotorDirections(carState);

  if (currentRight < targetRight) currentRight = min(currentRight + accelStep, targetRight);
  else if (currentRight > targetRight) currentRight = max(currentRight - accelStep * 2, targetRight);
  if (currentLeft < targetLeft) currentLeft = min(currentLeft + accelStep, targetLeft);
  else if (currentLeft > targetLeft) currentLeft = max(currentLeft - accelStep * 2, targetLeft);

  analogWrite(ENA, currentRight);
  analogWrite(ENB, currentLeft);
}
