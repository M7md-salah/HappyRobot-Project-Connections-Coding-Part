/* Happy Robot: ESP32-CAM AI-Thinker, Wi-Fi-only camera robot.
   Select AI Thinker ESP32-CAM and enable PSRAM.
   L298N: IN1=GPIO2, IN2=GPIO4, IN3=GPIO12, IN4=GPIO16.
   GPIO4 shares the flash LED; GPIO12 must stay LOW during reset or boot can fail. */
#include <Wire.h>
#include <WiFi.h>
#include <DHT.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include "time.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

// Change these to your 2.4 GHz network credentials.
const char *ssid = "Galaxy A55 F4F6";
const char *password = "0987654321";
#define SDA_PIN 13
#define SCL_PIN 14
#define DHTPIN 15
#define DHTTYPE DHT11
#define IN1 2
#define IN2 4
#define IN3 12
#define IN4 16
Adafruit_SH1106G oled(128, 64, &Wire, -1);
DHT dht(DHTPIN, DHTTYPE);

// AI-Thinker camera pins
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

httpd_handle_t webServer = NULL, streamServer = NULL;
const char *direction = "stop";
volatile bool moving = false;
volatile unsigned long lastMove = 0;
unsigned long lastSensor = 0, lastScreen = 0, lastRetry = 0;
float temperature = NAN, humidity = NAN;
const unsigned long WATCHDOG_MS = 800;

void stopMotors() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  direction = "stop"; moving = false;
}
bool moveRobot(const char *d) {
  lastMove = millis();
  if (!strcmp(d,"forward")) { digitalWrite(IN1,HIGH);digitalWrite(IN2,LOW); digitalWrite(IN3,HIGH);digitalWrite(IN4,LOW); }
  else if (!strcmp(d,"backward")) { digitalWrite(IN1,LOW);digitalWrite(IN2,HIGH); digitalWrite(IN3,LOW);digitalWrite(IN4,HIGH); }
  else if (!strcmp(d,"left")) { digitalWrite(IN1,LOW);digitalWrite(IN2,HIGH); digitalWrite(IN3,HIGH);digitalWrite(IN4,LOW); }
  else if (!strcmp(d,"right")) { digitalWrite(IN1,HIGH);digitalWrite(IN2,LOW); digitalWrite(IN3,LOW);digitalWrite(IN4,HIGH); }
  else if (!strcmp(d,"forward_left")) { digitalWrite(IN1,LOW);digitalWrite(IN2,LOW); digitalWrite(IN3,HIGH);digitalWrite(IN4,LOW); }
  else if (!strcmp(d,"forward_right")) { digitalWrite(IN1,HIGH);digitalWrite(IN2,LOW); digitalWrite(IN3,LOW);digitalWrite(IN4,LOW); }
  else if (!strcmp(d,"backward_left")) { digitalWrite(IN1,LOW);digitalWrite(IN2,LOW); digitalWrite(IN3,LOW);digitalWrite(IN4,HIGH); }
  else if (!strcmp(d,"backward_right")) { digitalWrite(IN1,LOW);digitalWrite(IN2,HIGH); digitalWrite(IN3,LOW);digitalWrite(IN4,LOW); }
  else if (!strcmp(d,"stop")) { stopMotors(); return true; }
  else { stopMotors(); return false; }
  direction = d; moving = true; return true;
}
void motorWatchdog(void *) { for (;;) { if (moving && millis()-lastMove>WATCHDOG_MS) stopMotors(); vTaskDelay(pdMS_TO_TICKS(100)); } }

static const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>Happy Robot</title><style>
:root{--bg:#080d1b;--card:#111c33;--line:#2a4168;--a:#4ee7df;--t:#f5f8ff;--m:#95a6c4}*{box-sizing:border-box}body{margin:0;background:radial-gradient(circle at 10% 0,#1d4161,transparent 40%),var(--bg);color:var(--t);font:15px system-ui}main{max-width:900px;margin:auto;padding:22px 16px}.top{display:flex;justify-content:space-between;margin-bottom:18px;font-size:22px;font-weight:800;letter-spacing:.08em}.top b,.ok{color:var(--a)}.ok{font-size:12px;padding-top:8px}.grid{display:grid;grid-template-columns:1.3fr .8fr;gap:16px}.card{background:linear-gradient(145deg,#14223d,#0d1527);border:1px solid var(--line);border-radius:18px;padding:14px;box-shadow:0 15px 36px #0008}.video{aspect-ratio:4/3;overflow:hidden;border-radius:12px;background:#000;position:relative}.video img{width:100%;height:100%;object-fit:cover}.live{position:absolute;top:9px;left:9px;background:#06101ddd;border-radius:18px;padding:4px 9px;font-size:11px}.live:before{content:'●';color:#ff6079;margin-right:5px}.stats{display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:14px}.stat{border:1px solid var(--line);border-radius:10px;padding:9px;color:var(--m);font-size:11px}.stat strong{display:block;color:var(--t);font-size:16px;margin-top:3px}.label{font-size:11px;letter-spacing:.1em;color:var(--m);text-transform:uppercase}.dir{font-size:25px;font-weight:700;text-transform:capitalize;margin:4px 0 12px}.joy{width:184px;height:184px;border:1px solid #3b5c91;border-radius:50%;margin:8px auto 15px;background:radial-gradient(circle,#24406c,#111c33 65%);position:relative;touch-action:none}.knob{position:absolute;width:62px;height:62px;left:61px;top:61px;border-radius:50%;background:linear-gradient(145deg,var(--a),#258bc4);box-shadow:0 5px 18px #0009}.stop{width:100%;padding:12px;border:0;border-radius:10px;background:#cf405b;color:white;font-weight:800;letter-spacing:.08em}.hint{text-align:center;color:var(--m);font-size:12px}@media(max-width:680px){.grid{grid-template-columns:1fr}.joy{width:170px;height:170px}.knob{left:54px;top:54px}}
</style></head><body><main><div class=top>HAPPY <b>ROBOT</b><span class=ok id=net>CONNECTING</span></div><div class=grid><section class=card><div class=video><img id=feed><span class=live>LIVE</span></div><div class=stats><div class=stat>TEMPERATURE<strong id=temp>--</strong></div><div class=stat>HUMIDITY<strong id=hum>--</strong></div><div class=stat>TIME<strong id=clock>--:--</strong></div></div></section><section class=card><div class=label>Drive direction</div><div class=dir id=dir>Stop</div><div class=joy id=joy><div class=knob id=knob></div></div><button class=stop id=stop>EMERGENCY STOP</button><p class=hint>Hold and drag. Releasing always stops the robot.</p></section></div></main><script>
const joy=document.querySelector('#joy'),knob=document.querySelector('#knob'),out=document.querySelector('#dir');document.querySelector('#feed').src='http://'+location.hostname+':81/stream';let down=false,dir='stop',keep;const send=d=>fetch('/move?dir='+d,{cache:'no-store'}).catch(()=>{});function pick(x,y){let a=Math.atan2(y,x)*180/Math.PI;if(Math.hypot(x,y)<20)return'stop';if(a>=-22.5&&a<22.5)return'right';if(a<67.5&&a>=22.5)return'backward_right';if(a<112.5&&a>=67.5)return'backward';if(a<157.5&&a>=112.5)return'backward_left';if(a>=157.5||a<-157.5)return'left';if(a<-112.5)return'forward_left';if(a<-67.5)return'forward';return'forward_right'}function drag(e){let r=joy.getBoundingClientRect(),x=e.clientX-r.left-r.width/2,y=e.clientY-r.top-r.height/2,m=Math.hypot(x,y),z=r.width*.31;if(m>z){x=x/m*z;y=y/m*z}knob.style.transform=`translate(${x}px,${y}px)`;let d=pick(x,y);if(d!=dir){dir=d;out.textContent=d.replace('_',' ');send(d)}}function stop(){down=false;dir='stop';out.textContent='Stop';knob.style.transform='';send(dir);clearInterval(keep)}joy.onpointerdown=e=>{down=true;joy.setPointerCapture(e.pointerId);drag(e);keep=setInterval(()=>send(dir),250)};joy.onpointermove=e=>{if(down)drag(e)};['pointerup','pointercancel','lostpointercapture'].forEach(x=>joy.addEventListener(x,stop));document.querySelector('#stop').onclick=stop;document.addEventListener('visibilitychange',()=>{if(document.hidden)stop()});async function stat(){try{let d=await (await fetch('/status',{cache:'no-store'})).json();temp.textContent=d.temp==null?'--':d.temp+' °C';hum.textContent=d.humidity==null?'--':d.humidity+' %';clock.textContent=d.time;net.textContent='● ONLINE'}catch(e){net.textContent='OFFLINE'}}stat();setInterval(stat,2000);
</script></body></html>)HTML";

esp_err_t home(httpd_req_t *r) { httpd_resp_set_type(r,"text/html"); return httpd_resp_send(r,PAGE,HTTPD_RESP_USE_STRLEN); }
esp_err_t statusPage(httpd_req_t *r) {
  char ts[10]="--:--",a[12],b[12],json[130]; struct tm t;
  if (WiFi.status()==WL_CONNECTED && getLocalTime(&t,10)) strftime(ts,sizeof(ts),"%H:%M",&t);
  if(isnan(temperature)) strcpy(a,"null"); else snprintf(a,sizeof(a),"%.1f",temperature);
  if(isnan(humidity)) strcpy(b,"null"); else snprintf(b,sizeof(b),"%.1f",humidity);
  snprintf(json,sizeof(json),"{\"temp\":%s,\"humidity\":%s,\"time\":\"%s\"}",a,b,ts);
  httpd_resp_set_type(r,"application/json"); return httpd_resp_sendstr(r,json);
}
esp_err_t movePage(httpd_req_t *r) {
  char q[64]={0},d[24]="stop"; if(httpd_req_get_url_query_len(r)>0 && httpd_req_get_url_query_str(r,q,sizeof(q))==ESP_OK) httpd_query_key_value(q,"dir",d,sizeof(d));
  bool good=moveRobot(d); httpd_resp_set_type(r,"application/json"); return httpd_resp_sendstr(r,good?"{\"ok\":true}":"{\"ok\":false}");
}
esp_err_t streamPage(httpd_req_t *r) {
  httpd_resp_set_type(r,"multipart/x-mixed-replace;boundary=frame");
  while(true){camera_fb_t *fb=esp_camera_fb_get();if(!fb)return ESP_FAIL;char h[64];int n=snprintf(h,sizeof(h),"\r\n--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",fb->len);esp_err_t e=httpd_resp_send_chunk(r,h,n);if(e==ESP_OK)e=httpd_resp_send_chunk(r,(const char*)fb->buf,fb->len);esp_camera_fb_return(fb);if(e!=ESP_OK)return e;}
}
void startServer() {
  httpd_config_t c=HTTPD_DEFAULT_CONFIG(); c.server_port=80;c.ctrl_port=32768;
  httpd_uri_t a={"/",HTTP_GET,home,NULL},b={"/status",HTTP_GET,statusPage,NULL},d={"/move",HTTP_GET,movePage,NULL};
  if(httpd_start(&webServer,&c)==ESP_OK){httpd_register_uri_handler(webServer,&a);httpd_register_uri_handler(webServer,&b);httpd_register_uri_handler(webServer,&d);}
  c.server_port=81;c.ctrl_port=32769;httpd_uri_t s={"/stream",HTTP_GET,streamPage,NULL};if(httpd_start(&streamServer,&c)==ESP_OK)httpd_register_uri_handler(streamServer,&s);
}
bool initCamera() {
  camera_config_t c={};c.ledc_channel=LEDC_CHANNEL_0;c.ledc_timer=LEDC_TIMER_0;c.pin_d0=Y2_GPIO_NUM;c.pin_d1=Y3_GPIO_NUM;c.pin_d2=Y4_GPIO_NUM;c.pin_d3=Y5_GPIO_NUM;c.pin_d4=Y6_GPIO_NUM;c.pin_d5=Y7_GPIO_NUM;c.pin_d6=Y8_GPIO_NUM;c.pin_d7=Y9_GPIO_NUM;c.pin_xclk=XCLK_GPIO_NUM;c.pin_pclk=PCLK_GPIO_NUM;c.pin_vsync=VSYNC_GPIO_NUM;c.pin_href=HREF_GPIO_NUM;
  c.pin_sccb_sda=SIOD_GPIO_NUM;c.pin_sccb_scl=SIOC_GPIO_NUM; // Fixed: current ESP32 camera API spelling
  c.pin_pwdn=PWDN_GPIO_NUM;c.pin_reset=RESET_GPIO_NUM;c.xclk_freq_hz=20000000;c.pixel_format=PIXFORMAT_JPEG;c.grab_mode=CAMERA_GRAB_WHEN_EMPTY;
  if(psramFound()){c.frame_size=FRAMESIZE_SVGA;c.jpeg_quality=12;c.fb_count=2;c.fb_location=CAMERA_FB_IN_PSRAM;}else{c.frame_size=FRAMESIZE_QVGA;c.jpeg_quality=15;c.fb_count=1;c.fb_location=CAMERA_FB_IN_DRAM;}
  return esp_camera_init(&c)==ESP_OK;
}
void dashboard() {
  oled.clearDisplay();oled.drawRoundRect(0,0,128,64,5,SH110X_WHITE);oled.fillRoundRect(4,4,120,15,3,SH110X_WHITE);oled.setTextColor(SH110X_BLACK);oled.setCursor(11,8);oled.print("HAPPY ROBOT");oled.setTextColor(SH110X_WHITE);oled.setCursor(88,8);oled.print(WiFi.status()==WL_CONNECTED?"ONLINE":"OFFLINE");
  oled.setCursor(7,26);oled.print("IP:");oled.setCursor(28,26);if(WiFi.status()==WL_CONNECTED)oled.print(WiFi.localIP());else oled.print("connecting...");oled.setCursor(7,37);oled.print("Drive: ");oled.print(direction);oled.setCursor(7,48);oled.print("Temp:");if(isnan(temperature))oled.print(" --");else{oled.print(temperature,1);oled.print(" C");}oled.setCursor(74,48);oled.print("Hum:");if(isnan(humidity))oled.print(" --");else{oled.print(humidity,0);oled.print("%");}oled.drawFastHLine(6,54,116,SH110X_WHITE);oled.setCursor(7,57);oled.print("Open browser at this IP");oled.display();
}
void connectWiFi() { WiFi.mode(WIFI_STA);WiFi.setSleep(false);WiFi.begin(ssid,password);unsigned long start=millis();while(WiFi.status()!=WL_CONNECTED&&millis()-start<15000){dashboard();delay(250);}if(WiFi.status()==WL_CONNECTED){configTime(3*3600,0,"pool.ntp.org");Serial.print("http://");Serial.println(WiFi.localIP());} }
void setup() { Serial.begin(115200);WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG,0);pinMode(IN1,OUTPUT);pinMode(IN2,OUTPUT);pinMode(IN3,OUTPUT);pinMode(IN4,OUTPUT);stopMotors();Wire.begin(SDA_PIN,SCL_PIN);if(!oled.begin(0x3C,true))while(true)delay(1000);dht.begin();dashboard();connectWiFi();if(initCamera())startServer();else Serial.println("Camera initialization failed");xTaskCreate(motorWatchdog,"motorWatchdog",2048,NULL,1,NULL); }
void loop() { unsigned long now=millis();if(now-lastSensor>=2000){lastSensor=now;temperature=dht.readTemperature();humidity=dht.readHumidity();}if(WiFi.status()!=WL_CONNECTED&&now-lastRetry>=30000){lastRetry=now;WiFi.reconnect();}if(now-lastScreen>=400){lastScreen=now;dashboard();}delay(10); }
