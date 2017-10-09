#include "mbed.h"
#include "AZ3166WiFi.h"
#include "http_client.h"
#include "AudioClassV2.h"
#include "Websocket.h"
#include "RingBuffer.h"

void InitWiFi()
{
  Screen.print("WiFi \r\n \r\nConnecting...\r\n             \r\n");
  
  if(WiFi.begin() == WL_CONNECTED)
  {
    printf("connected \r\\n");
  }
}
AudioClass& Audio = AudioClass::getInstance();
const int RING_BUFFER_SIZE = 15000;
RingBuffer ringBuffer(RING_BUFFER_SIZE);
char readBuffer[2048];
char websocketBuffer[5000];
bool startPlay = false;
static char emptyAudio[256];
Websocket *websocket;
void play()
{
  printf("start play\r\n");
  Audio.attachRecord(NULL);
  Audio.attachPlay(playCallback);
  Audio.format(8000, 16);
  Audio.startPlay();
  startPlay = true;
}

void record()
{   
  ringBuffer.clear();
  Audio.format(8000, 16);
  Audio.attachPlay(NULL);
  Audio.attachRecord(recordCallback);
  Audio.startRecord();
}

void stop()
{
  Audio.stop();
  startPlay= false;
}

void playCallback(void)
{
  if (ringBuffer.use() < 512)
  {
    Audio.write(emptyAudio, 512);
    return;
  }
  ringBuffer.get((uint8_t*)readBuffer, 512);
  Audio.write(readBuffer, 512);
}

void recordCallback(void)
{
  Audio.read(readBuffer, 2048);
  ringBuffer.put((uint8_t*)readBuffer, 2048);
}

void setResponseBodyCallback(const char* data, size_t dataSize)
{
  while(ringBuffer.available() < dataSize) {
    printf("ringBuffer ava %d\r\n", ringBuffer.available());
    delay(10);
  }
  ringBuffer.put((uint8_t*)data, dataSize);
  if (ringBuffer.use() > RING_BUFFER_SIZE / 2 && startPlay == false) {
    play();
  }
}

char* getUrl()
{
  char *url;
  url = (char *)malloc(300);
  if (url == NULL)
  {
    return NULL;
  }
  HTTPClient guidRequest = HTTPClient(HTTP_GET, "http://www.fileformat.info/tool/guid.htm?count=1&format=text&hyphen=true");
  const Http_Response* _response = guidRequest.send();
  if (_response == NULL)
  {
    printf("Guid generator HTTP request failed.\r\n");
    return NULL;
  }
  snprintf(url, 300, "%s%s", "ws://yiribot.azurewebsites.net/chat?nickName=", _response->body);
  printf("url: %s\r\n", url);
  return url;
}

void setup() {
  pinMode(USER_BUTTON_A, INPUT);
  pinMode(USER_BUTTON_B, INPUT);
  if (WiFi.begin() == WL_CONNECTED)
  {
    printf("connected\r\n");
  }

  memset(emptyAudio, 0x0, 256);
  char *url = getUrl();
  websocket = new Websocket(url);
  int connect_state = (*websocket).connect();
  printf("connect_state %d\r\n", connect_state);
}

bool isClose = false;

void loop() {
  if (isClose) return;
  // put your main code here, to run repeatedly:
  printf("you can start a new question now\r\n");
  while(digitalRead(USER_BUTTON_A) == HIGH) {
    if (digitalRead(USER_BUTTON_B) == LOW) {
      (*websocket).close();
      isClose = true;
      return;
    } 
    (*websocket).heartBeat();
    delay(100);
  }
  (*websocket).send("pcmstart", 4, 0x02);
  record();
  while(digitalRead(USER_BUTTON_A) == LOW || ringBuffer.use() > 0)
  {
    if (digitalRead(USER_BUTTON_A) == HIGH) stop();
    int sz = ringBuffer.get((uint8_t*)websocketBuffer, 2048);
    if (sz > 0) {
      (*websocket).send(websocketBuffer, sz, 0x00);
    }
  }
  stop();
  (*websocket).send("pcmend", 4, 0x80);
  delay(100);
  printf("your question send\r\n");
  ringBuffer.clear();
  
  unsigned char opcode = 0;
  int len = 0;
  bool first = true;
  while ((opcode & 0x80) == 0x00) {
    int tmp = (*websocket).read(websocketBuffer, &len, &opcode, first);
    printf("tmp %d recv len %d opcode %d\r\n", tmp, len, opcode);
    printf("%s\r\n", websocketBuffer);
    first = false;
    if (tmp == 0) break;
    setResponseBodyCallback(websocketBuffer, len);
  }
  if (startPlay == false) play();
  while(ringBuffer.use() >= 512) delay(100);
  stop();
}
