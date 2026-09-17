#include "globals.h"
#include "config.h"
#include "web_handlers.h"
#include "modem.h"
#include "push.h"
#include "sms_process.h"
#include "esim.h"
#include "wifi_manager.h"
#include "ble_provisioning.h"

void setup() {
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, HIGH);
  Serial.begin(115200);
  // 缩短初始化延时，WiFi连接会处理自己的超时
  delay(200);
  Serial1.begin(115200, SERIAL_8N1, RXD, TXD);
  Serial1.setRxBufferSize(SERIAL_BUFFER_SIZE);
  while (Serial1.available()) Serial1.read();
  modemPowerCycle();
  while (Serial1.available()) Serial1.read();
  initConcatBuffer();
  loadConfig();
  loadWiFiCredentials();
  configValid = isConfigValid();
  bleProvisioningBegin();

  // WiFi 失败时保持运行并继续提供 BLE 配网，不再进入重启循环。
  connectConfiguredWiFi(20000);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/tools", HTTP_GET, handleRoot);
  server.on("/sms", HTTP_GET, handleRoot);
  server.on("/sendsms", HTTP_POST, handleSendSms);
  server.on("/ping", HTTP_POST, handlePing);
  // Keep legacy endpoints method-specific. An HTTP_ANY handler registered
  // before the CORS handlers also catches OPTIONS and challenges the browser
  // for Basic Auth, so cross-origin requests never get past preflight.
  server.on("/query", HTTP_GET, handleQuery);
  server.on("/flight", HTTP_GET, handleFlightMode);
  server.on("/at", HTTP_GET, handleATCommand);
  server.on("/log", HTTP_GET, handleLog);
  server.on("/modem", HTTP_GET, handleModem);
  server.on("/wifi", HTTP_GET, handleWifi);
  server.on("/wifi", HTTP_POST, handleWifi);
  server.on("/esim", HTTP_GET, handleESim);
  // Versioned REST aliases. Legacy routes above remain available for clients.
  server.on("/api/v1/status", HTTP_GET, handleStatus);
  server.on("/api/v1/config", HTTP_GET, handleConfigGet);
  server.on("/api/v1/config", HTTP_POST, handleSave);
  server.on("/api/v1/sms", HTTP_POST, handleSendSms);
  server.on("/api/v1/push/test", HTTP_POST, handlePushTest);
  server.on("/api/v1/ping", HTTP_POST, handlePing);
  server.on("/api/v1/wifi", HTTP_POST, handleWifi);
  server.on("/api/v1/logs", HTTP_GET, handleLog);
  const char* corsPaths[] = {
    "/api/v1/status", "/api/v1/config", "/api/v1/sms", "/api/v1/push/test", "/api/v1/ping",
    "/api/v1/wifi", "/api/v1/logs", "/query", "/flight", "/at", "/log",
    "/modem", "/wifi", "/esim", "/sendsms", "/ping", "/save"
  };
  for (const char* path : corsPaths) {
    server.on(path, HTTP_OPTIONS, handleCorsPreflight);
  }
  const char* collectedHeaders[] = {"Origin", "Access-Control-Request-Private-Network"};
  server.collectHeaders(collectedHeaders, 2);
  server.begin();
  logCaptureLn(String("HTTP服务器已启动"));

  // ---- NTP 时间同步 ----
  logCaptureLn(String("正在同步NTP时间..."));
  configTime(0, 0, "ntp.ntsc.ac.cn", "ntp.aliyun.com", "pool.ntp.org");
  int ntpRetry = 0;
  while (time(nullptr) < 100000 && ntpRetry < 100) {
    delay(1);
    server.handleClient();
    ntpRetry++;
  }
  if (time(nullptr) >= 100000) {
    timeSynced = true;
    logCaptureLn(String("NTP时间同步成功"));
    time_t now = time(nullptr);
    logCapture(String("当前UTC时间戳: "));
    logCaptureLn(String(now));
  } else {
    logCaptureLn(String("NTP时间同步失败，将使用设备时间"));
  }

  ssl_client.setInsecure();
  digitalWrite(LED_BUILTIN, LOW);

  // ---- 启动通知（网页已可用，发邮件不会影响用户访问） ----
  if (configValid) {
    logCaptureLn(String("配置有效，发送启动通知..."));
    String subject = "短信转发器已启动";
    String body = "设备已启动\n设备地址: " + getDeviceUrl();
    sendEmailNotification(subject.c_str(), body.c_str());
  }



  // ---- 模组初始化（较慢，但网页已可访问） ----
  modemInit();

  // ---- eSIM初始化 ----
  // Cellular readiness is independent of eUICC management access.
  if (modemLimitedMode) {
    logCaptureLn(String("蜂窝功能受限，仍尝试初始化 eSIM"));
  }
  logCaptureLn(String("初始化eSIM..."));
  if (esimInit()) {
    logCaptureLn(String("eSIM AT 能力检测通过，尝试读取 EID"));
    char eid[40];
    if (esimGetEID(eid, sizeof(eid))) {
      logCapture(String("EID: "));
      logCaptureLn(eid);
    } else {
      logCaptureLn(String("eSIM EID 读取失败: ") + esimGetLastError());
    }
  } else {
    logCaptureLn(String("eSIM 初始化失败: ") + esimGetLastError());
  }

}

void loop() {
  server.handleClient();
  maintainWiFiConnection();
  bleProvisioningLoop();
  if (!configValid) {
    if (millis() - lastPrintTime >= 1000) {
      lastPrintTime = millis();
      logCaptureLn(String("⚠️ 请访问 " + getDeviceUrl() + " 配置系统参数（至少配置邮件或任一推送通道）"));
    }
  }
  checkConcatTimeout();
  handleSerialConsole();
  checkSerial1URC();
}
