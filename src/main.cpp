#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>

// Настройки виртуальной Wi-Fi сети Wokwi
const char* WIFI_SSID = "Wokwi-GUEST";
const char* WIFI_PASS = "";

// Определение пинов ESP32
#define MQ2_PIN     34  // Аналоговый вход датчика MQ-2 (ADC1)
#define MQ135_PIN   35  // Аналоговый вход датчика MQ-135 (ADC1)
#define FAN_PIN     13  // Управление реле / MOSFET вентилятора
#define BUZZER_PIN  14  // Пьезозуммер
#define GREEN_LED   26  // Зелёный светодиод (чисто)
#define YELLOW_LED  25  // Жёлтый светодиод (лёгкое загрязнение)
#define ORANGE_LED  33  // Оранжевый светодиод (среднее загрязнение)
#define RED_LED     27  // Красный светодиод (опасное загрязнение)

// Глобальные переменные состояния
WebServer server(80);
bool fanAutoMode = true;
bool fanState    = false;
bool buzzerState = false;
int  mq2Value    = 0;
int  mq135Value  = 0;
unsigned long lastCheck = 0;
const unsigned long CHECK_INTERVAL = 2000; // Интервал опроса: 2 с

enum AirLevel { CLEAN, LIGHT, MEDIUM, DANGER };
AirLevel currentLevel = CLEAN;

// HTML/CSS/JS веб-сайта управления (хранится во Flash-памяти)
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="ru">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Мониторинг качества воздуха ESP32</title>
  <style>
    body { font-family: Arial, sans-serif; background: #f1f5f9; color: #0f172a;
           max-width: 640px; margin: 24px auto; padding: 0 16px; }
    .card { background: #fff; border-radius: 12px; padding: 18px;
            margin-bottom: 14px; box-shadow: 0 2px 8px rgba(0,0,0,0.08); }
    h1 { font-size: 20px; margin-top: 0; color: #1e293b; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
    .val { font-size: 28px; font-weight: bold; margin-top: 6px; }
    .badge { display: inline-block; padding: 6px 12px; border-radius: 8px;
             color: #fff; font-weight: bold; }
    .btns { margin-top: 10px; }
    .btns button { padding: 10px 14px; margin-right: 8px; border: none;
                   border-radius: 8px; cursor: pointer; font-weight: bold; font-size: 14px; }
    .on { background: #16a34a; color: #fff; }
    .off { background: #dc2626; color: #fff; }
    .auto { background: #2563eb; color: #fff; }
    button:hover { opacity: 0.9; }

    /* Тревожный баннер при опасном уровне */
    .alert-banner {
      display: none;
      background: #dc2626;
      color: #ffffff;
      padding: 16px 20px;
      border-radius: 12px;
      margin-bottom: 14px;
      text-align: center;
      font-weight: bold;
      font-size: 16px;
      line-height: 1.4;
      box-shadow: 0 4px 16px rgba(220, 38, 38, 0.45);
      animation: pulseAlert 0.8s infinite alternate;
    }
    @keyframes pulseAlert {
      0% { opacity: 1; transform: scale(1); }
      100% { opacity: 0.88; transform: scale(1.02); }
    }
  </style>
</head>
<body>
  <!-- Всплывающее предупреждение при тревоге -->
  <div id="alarmBanner" class="alert-banner">
    🚨 ВНИМАНИЕ! КРИТИЧЕСКИЙ УРОВЕНЬ ЗАГРЯЗНЕНИЯ ВОЗДУХА! 🚨<br>
    <span style="font-size: 13px; font-weight: normal;">Превышен порог безопасности (>900). Включена вентиляция и тревожная сирена!</span>
  </div>

  <div class="card">
    <h1>Контроль качества воздуха (ESP32)</h1>
    <p>Состояние: <span id="status" class="badge">Загрузка...</span></p>
  </div>
  <div class="grid">
    <div class="card"><div>MQ-2 (дым / газы)</div><div id="mq2" class="val">-</div></div>
    <div class="card"><div>MQ-135 (качество)</div><div id="mq135" class="val">-</div></div>
  </div>
  <div class="card">
    <p>Вентилятор: <b id="fan">-</b> (<span id="mode">-</span>) | Зуммер: <b id="bz">-</b></p>
    <div class="btns">
      <button class="on"   onclick="setFan('on')">Включить</button>
      <button class="off"  onclick="setFan('off')">Выключить</button>
      <button class="auto" onclick="setFan('auto')">Авторежим</button>
    </div>
  </div>
  <script>
    let audioCtx = null;
    let isAlarmPlaying = false;
    let soundInterval = null;

    // Инициализация Web Audio API по первому клику пользователя
    function initAudio() {
      if (!audioCtx) {
        audioCtx = new (window.AudioContext || window.webkitAudioContext)();
      }
      if (audioCtx.state === 'suspended') {
        audioCtx.resume();
      }
    }
    document.addEventListener('click', initAudio, { once: true });
    document.addEventListener('touchstart', initAudio, { once: true });

    // Воспроизведение прерывистой сирены тревоги (1000 Гц)
    function playAlarm() {
      if (isAlarmPlaying) return;
      initAudio();
      isAlarmPlaying = true;
      soundInterval = setInterval(() => {
        if (!audioCtx || audioCtx.state !== 'running') return;
        try {
          const osc = audioCtx.createOscillator();
          const gain = audioCtx.createGain();
          osc.type = 'sawtooth';
          osc.frequency.setValueAtTime(1000, audioCtx.currentTime); // 1000 Гц сирена
          gain.gain.setValueAtTime(0.25, audioCtx.currentTime);
          gain.gain.exponentialRampToValueAtTime(0.01, audioCtx.currentTime + 0.22);
          osc.connect(gain);
          gain.connect(audioCtx.destination);
          osc.start();
          osc.stop(audioCtx.currentTime + 0.22);
        } catch(e) { console.error(e); }
      }, 350);
    }

    function stopAlarm() {
      if (!isAlarmPlaying) return;
      isAlarmPlaying = false;
      if (soundInterval) {
        clearInterval(soundInterval);
        soundInterval = null;
      }
    }

    async function refresh() {
      try {
        const r = await fetch('/api/status');
        const d = await r.json();
        document.getElementById('mq2').textContent = d.mq2;
        document.getElementById('mq135').textContent = d.mq135;
        document.getElementById('fan').textContent = d.fan ? 'Включён' : 'Выключен';
        document.getElementById('mode').textContent = d.auto ? 'Авто' : 'Ручной';
        document.getElementById('bz').textContent = d.buzzer ? 'Тревога' : 'Выкл.';
        const st = document.getElementById('status');
        st.textContent = d.label;
        st.style.background = d.color;

        // Обработка тревоги: красный индикатор или включенный зуммер
        const banner = document.getElementById('alarmBanner');
        if (d.buzzer || d.color === '#dc2626') {
          banner.style.display = 'block';
          playAlarm();
        } else {
          banner.style.display = 'none';
          stopAlarm();
        }
      } catch (e) {
        console.error(e);
      }
    }
    async function setFan(m) { 
      try {
        initAudio();
        await fetch('/api/fan?mode=' + m); 
        refresh(); 
      } catch (e) {
        console.error(e);
      }
    }
    refresh(); 
    setInterval(refresh, 2000);
  </script>
</body>
</html>)rawliteral";

// Логика оценки уровня загрязнения и управления периферией
void controlSystem() {
  int maxVal = max(mq2Value, mq135Value);
  if      (maxVal > 900) currentLevel = DANGER;
  else if (maxVal > 700) currentLevel = MEDIUM;
  else if (maxVal > 400) currentLevel = LIGHT;
  else                   currentLevel = CLEAN;

  // Светодиодная индикация
  digitalWrite(GREEN_LED,  currentLevel == CLEAN  ? HIGH : LOW);
  digitalWrite(YELLOW_LED, currentLevel == LIGHT  ? HIGH : LOW);
  digitalWrite(ORANGE_LED, currentLevel == MEDIUM ? HIGH : LOW);
  digitalWrite(RED_LED,    currentLevel == DANGER ? HIGH : LOW);

  // Звуковая сигнализация при критическом уровне
  if (currentLevel == DANGER) {
    tone(BUZZER_PIN, 1000);
    buzzerState = true;
  } else {
    noTone(BUZZER_PIN);
    buzzerState = false;
  }

  // Управление вентилятором (автоматический или ручной режим)
  if (fanAutoMode) {
    fanState = (currentLevel >= MEDIUM);
  }
  digitalWrite(FAN_PIN, fanState ? HIGH : LOW);
}

// Обработчик главной страницы веб-сайта
void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", INDEX_HTML);
}

// Обработчик JSON API: текущие показания и состояние системы
void handleStatus() {
  JsonDocument doc;
  doc["mq2"]    = mq2Value;
  doc["mq135"]  = mq135Value;
  doc["fan"]    = fanState;
  doc["auto"]   = fanAutoMode;
  doc["buzzer"] = buzzerState;
  switch (currentLevel) {
    case CLEAN:  doc["label"] = "Чистый воздух";       doc["color"] = "#16a34a"; break;
    case LIGHT:  doc["label"] = "Лёгкое загрязнение";  doc["color"] = "#ca8a04"; break;
    case MEDIUM: doc["label"] = "Среднее загрязнение"; doc["color"] = "#ea580c"; break;
    case DANGER: doc["label"] = "Опасный уровень!";    doc["color"] = "#dc2626"; break;
  }
  String json;
  serializeJson(doc, json);
  server.send(200, "application/json", json);
}

// Обработчик команд управления вентилятором с веб-сайта
void handleFanControl() {
  if (server.hasArg("mode")) {
    String mode = server.arg("mode");
    if (mode == "on") {
      fanAutoMode = false; fanState = true;
    } else if (mode == "off") {
      fanAutoMode = false; fanState = false;
    } else if (mode == "auto") {
      fanAutoMode = true;
    }
    controlSystem();
    Serial.printf("Команда с веб-сайта: mode=%s\n", mode.c_str());
  }
  handleStatus();
}

// Инициализация системы
void setup() {
  Serial.begin(115200);

  // Установка 10-битного разрешения АЦП (диапазон 0-1023) 
  // в точном соответствии с порогами таблицы 6 ТЗ (<400, 400-700, 701-900, >900)
  analogReadResolution(10);

  pinMode(FAN_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED, OUTPUT);
  pinMode(YELLOW_LED, OUTPUT);
  pinMode(ORANGE_LED, OUTPUT);
  pinMode(RED_LED, OUTPUT);

  Serial.println("\n--- Запуск устройства мониторинга воздуха ESP32 ---");
  Serial.print("Подключение к Wi-Fi (Wokwi-GUEST)...");
  WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf("\nWi-Fi подключён. IP-адрес ESP32: %s\n", WiFi.localIP().toString().c_str());

  server.on("/",           HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/fan",    HTTP_GET, handleFanControl);
  server.begin();
  Serial.println("HTTP сервер запущен на порту 80.");
  Serial.println("В симуляторе Wokwi веб-интерфейс доступен по ссылке: http://localhost:8180");
}

// Основной неблокирующий цикл
void loop() {
  server.handleClient();
  unsigned long now = millis();
  if (now - lastCheck >= CHECK_INTERVAL) {
    lastCheck  = now;
    mq2Value   = analogRead(MQ2_PIN);
    mq135Value = analogRead(MQ135_PIN);
    controlSystem();
    Serial.printf("MQ-2 (GPIO 34): %d | MQ-135 (GPIO 35): %d | Вентилятор: %s | Зуммер: %s\n",
                  mq2Value, mq135Value, fanState ? "ВКЛ" : "ВЫКЛ", buzzerState ? "ТРЕВОГА" : "ВЫКЛ");
  }
}
