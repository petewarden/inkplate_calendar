/*
  Arduino daily calendar application for the Inkplate Tempera4 development board.
  Copyright 2025, Pete Warden, pete@petewarden.com.
  Released under the Apache2 open source license, see LICENSE for details.
*/

#include "Inkplate.h"      // Include Inkplate library to the sketch
#include "driver/rtc_io.h" // Include ESP32 library for RTC pin I/O (needed for rtc_gpio_isolate() function)
#include <rom/rtc.h>       // Include ESP32 library for RTC (needed for rtc_get_reset_reason() function)

#define STRINGIZE_DETAIL(x) #x
#define STRINGIZE(x) STRINGIZE_DETAIL(x)
#define CHECK_OK(x)                                                          \
  do                                                                         \
  {                                                                          \
    int result = (x);                                                        \
    if (result != ESP_OK)                                                    \
    {                                                                        \
      Serial.write("Error at " __FILE__ ":" STRINGIZE(__LINE___) ", code "); \
      Serial.write(result);                                                  \
      Serial.write("\n");                                                    \
    }                                                                        \
  } while (false)

#define testAssert(X)                                                          \
  do                                                                           \
  {                                                                            \
    if (!(X))                                                                  \
    {                                                                          \
      printTouchBuffer();                                                      \
      Serial.println("Assertion failed at " __FILE__ ":" STRINGIZE(__LINE__)); \
      while (true)                                                             \
        ;                                                                      \
    }                                                                          \
  } while (false)

#define testEqual(X, Y)                                                        \
  do                                                                           \
  {                                                                            \
    if ((X) != (Y))                                                            \
    {                                                                          \
      printTouchBuffer();                                                      \
      Serial.print(#X " = ");                                                  \
      (X).print();                                                             \
      Serial.print(#Y " = ");                                                  \
      (Y).print();                                                             \
      Serial.println("Assertion failed at " __FILE__ ":" STRINGIZE(__LINE__)); \
      while (true)                                                             \
        ;                                                                      \
    }                                                                          \
  } while (false)

#define LOG_INT(x) do { \
  Serial.print(#x " = "); \
  Serial.println((x)); \
} while (false)

#define DEFAULT_SLEEP_TIME (1 * 60 * 1000 * 1000)

typedef enum
{
  UP_ARROW,
  DOWN_ARROW,
  OK_BUTTON,
} IconType;

class Button
{
public:
  Button(int left, int top, int width, int height, const char *name, IconType icon) : left(left), top(top), width(width), height(height), name(name), icon(icon), pressed(false), pressStartTime(-1) {}

  Button(const Button &other)
  {
    this->left = other.left;
    this->top = other.top;
    this->width = other.width;
    this->height = other.height;
    this->name = other.name;
    this->icon = other.icon;
    this->pressed = other.pressed;
    this->pressStartTime = other.pressStartTime;
  }

  bool contains(int x, int y, int margin = 0)
  {
    const int left = this->left - margin;
    const int right = left + this->width + (margin * 2);
    const int top = this->top - margin;
    const int bottom = top + this->height + (margin * 2);

    return (
      (x >= left) &&
      (y >= top) &&
      (x < right) &&
      (y < bottom)
    );
  }

  int left;
  int top;
  int width;
  int height;
  const char *name;
  IconType icon;
  bool pressed;
  int32_t pressStartTime;
};

typedef struct STouchData
{
  int x;
  int y;
  uint32_t time;
  bool lifted;
  bool operator==(const struct STouchData &other)
  {
    return (
        (this->x == other.x) &&
        (this->y == other.y) &&
        (this->time == other.time) &&
        (this->lifted == other.lifted));
  }
  bool operator!=(const struct STouchData &other)
  {
    return !(*this == other);
  }
  void print() const
  {
    Serial.print("x = ");
    Serial.print(this->x);
    Serial.print(", y = ");
    Serial.print(this->y);
    Serial.print(", time = ");
    Serial.print(this->time);
    Serial.print(", lifted = ");
    Serial.println(this->lifted);
  }
  void clear()
  {
    this->x = -1;
    this->y = -1;
    this->time = -1;
    this->lifted = false;
  }
} TouchData;

void displayToNDigits(int32_t d, int n, char pad = ' ', int base = DEC);
void printToNDigits(int32_t d, int n, char pad = ' ', int base = DEC);
void initializeTouchscreen();
void touchscreenWorker(void *pvParameters);
void pushHeadTouchBuffer(const TouchData *touch);
bool popTailTouchBuffer(TouchData *touch);
void testTouchBuffer();
void printTouchBuffer();
void handleTouchEvents(Button* buttons, int buttonsCount);
void onButtonClick(const Button& button, const TouchData& touch);
void etchASketch();
void drawSettings();
const char* weekday(int year, int month, int day);
int getMonthLength(int month, int year);

Inkplate display(INKPLATE_1BIT); // Create an object on Inkplate library and also set library into 1-bit mode (BW)

int g_previous_battery_percentage = 0;
int g_previous_month = 0;
int g_previous_day = 0;

Button g_settings_buttons[] = {
    {60, 20, 85, 40, "month_up", UP_ARROW},
    {60, 150, 85, 40, "month_down", DOWN_ARROW},
    {205, 20, 85, 40, "day_up", UP_ARROW},
    {205, 150, 85, 40, "day_down", DOWN_ARROW},
    {400, 20, 85, 40, "year_up", UP_ARROW},
    {400, 150, 85, 40, "year_down", DOWN_ARROW},
    {160, 250, 85, 40, "hour_up", UP_ARROW},
    {160, 380, 85, 40, "hour_down", DOWN_ARROW},
    {300, 250, 85, 40, "minute_up", UP_ARROW},
    {300, 380, 85, 40, "minute_down", DOWN_ARROW},
    {400, 250, 85, 40, "ampm_toggle", UP_ARROW},
    {400, 380, 85, 40, "ampm_toggle", DOWN_ARROW},
    {200, 450, 200, 100, "ok_button", OK_BUTTON},
};
const size_t g_settings_buttons_count = sizeof(g_settings_buttons) / sizeof(g_settings_buttons[0]);

TaskHandle_t g_touchscreen_task;

constexpr size_t g_touch_buffer_capacity = 256;
StaticSemaphore_t g_touch_buffer_mutex_buffer = {};
SemaphoreHandle_t g_touch_buffer_semaphore = NULL;
int g_touch_buffer_head_index = 0;
int g_touch_buffer_tail_index = 0;
TouchData g_touch_buffer[g_touch_buffer_capacity] = {};

TouchData g_previous_touch = {};
uint16_t g_ts_x_res = 0;
uint16_t g_ts_y_res = 0;

void setup()
{
  Serial.begin(115200);
  display.begin(); // Init Inkplate library (you should call this function ONLY ONCE)

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  Serial.print("wakeup_reason = ");
  Serial.println(wakeup_reason);
  const bool isButtonWakeup = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  if (isButtonWakeup)
  {
    display.frontlight(true);
  }

  // Make sure we can wake up with the side button.
  CHECK_OK(esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0));

  if (isButtonWakeup || !display.rtcIsSet())
  {
    initializeTouchscreen();
    g_previous_touch.lifted = true;
  }
  else
  {
    drawCurrentTime();
    const int hour = display.rtcGetHour();
    const int minute = display.rtcGetMinute();
    // Needs to be 64-bit because the sleep time is expressed in microseconds.
    uint64_t secondsToSleep;
    if (hour < 12)
    {
      const uint64_t hoursToSleep = 11 - hour;
      const uint64_t minutesToSleep = 60 - minute;
      secondsToSleep = (60 * 60 * hoursToSleep) + (60 * minutesToSleep) + 30;
    }
    else
    {
      const uint64_t hoursToSleep = 23 - hour;
      const uint64_t minutesToSleep = 60 - minute;
      secondsToSleep = (60 * 60 * hoursToSleep) + (60 * minutesToSleep) + 30;
    }
    LOG_INT(secondsToSleep);
    CHECK_OK(esp_sleep_enable_timer_wakeup(secondsToSleep * (1000 * 1000)));
    display.frontlight(false);
    esp_deep_sleep_start();
  }
}

void loop()
{
  drawSettings();
  // etchASketch();
}

void etchASketch()
{
  bool anyTouchFound = false;
  while (true)
  {
    TouchData current;
    const bool touchFound = popTailTouchBuffer(&current);
    if (!touchFound)
    {
      break;
    }
    anyTouchFound = true;
    if (!g_previous_touch.lifted && !current.lifted)
    {
      display.drawLine(g_previous_touch.x, g_previous_touch.y, current.x, current.y, BLACK);
      // display.fillCircle(current.x, current.y, 20, BLACK);
    }
    g_previous_touch = current;
  }

  if (anyTouchFound)
  {
    display.partialUpdate(false, false);
  }
}

void initializeTouchscreen()
{
  while (!display.tsInit(true))
  {
    delay(100);
  }
  
  Serial.println("Touchscreen init ok");

  g_touch_buffer_semaphore = xSemaphoreCreateMutexStatic(&g_touch_buffer_mutex_buffer);
  g_previous_touch.clear();
  // testTouchBuffer();
  xTaskCreate(
      touchscreenWorker,  /* Task function. */
      "touchscreen_task", /* name of task. */
      10000,              /* Stack size of task */
      NULL,               /* parameter of the task */
      1,                  /* priority of the task */
      &g_touchscreen_task /* Task handle to keep track of created task */
  );
}

void tsGetXY(uint8_t *_d, int *x, int *y)
{
  *x = *y = 0;
  *x = (_d[0] & 0xf0);
  *x <<= 4;
  *x |= _d[1];
  *y = (_d[0] & 0x0f);
  *y <<= 8;
  *y |= _d[2];
}

bool tsGetData(TouchData *outTouch)
{
  uint8_t _raw[8];
  uint8_t fingers = 0;
  display.tsGetRawData(_raw);
  // No data found
  if (_raw[0] != 0x5a)
  {
    return false;
  }

  for (int i = 0; i < 8; i++)
  {
    if (_raw[7] & (1 << i))
      fingers++;
  }

  outTouch->lifted = (fingers == 0);

  extern uint16_t _tsXResolution;
  extern uint16_t _tsYResolution;

  constexpr int fingerIndex = 0;
  int xRaw;
  int yRaw;
  tsGetXY((_raw + 1) + (fingerIndex * 3), &xRaw, &yRaw);

  outTouch->y = E_INK_HEIGHT - 1 - ((xRaw * E_INK_HEIGHT - 1) / _tsXResolution);
  outTouch->x = ((yRaw * E_INK_WIDTH - 1) / _tsYResolution);

  return true;
}

void touchscreenWorker(void *pvParameters)
{
  while (true)
  {
    vTaskDelay(10 * portTICK_PERIOD_MS);

    TouchData newTouch;
    const bool touchFound = tsGetData(&newTouch);
    if (touchFound)
    {
      newTouch.time = millis();
      pushHeadTouchBuffer(newTouch);
    }
  }
}

void pushHeadTouchBuffer(const TouchData &touch)
{
  xSemaphoreTake(g_touch_buffer_semaphore, portMAX_DELAY);

  const bool isEmpty = (g_touch_buffer_head_index == g_touch_buffer_tail_index);
  bool doAdd;
  if (isEmpty)
  {
    doAdd = true;
  }
  else
  {
    TouchData *oldHead = &g_touch_buffer[(g_touch_buffer_head_index - 1) % g_touch_buffer_capacity];
    const bool isSameLift = (oldHead->lifted == touch.lifted);
    const bool isSamePosition = ((oldHead->x == touch.x) && (oldHead->y == touch.y));
    if (isSameLift && isSamePosition)
    {
      doAdd = false;
      if (!touch.lifted)
      {
        oldHead->time = touch.time;
      }
    }
    else
    {
      doAdd = true;
    }
  }

  if (doAdd)
  {
    g_touch_buffer[g_touch_buffer_head_index % g_touch_buffer_capacity] = touch;
    g_touch_buffer_head_index += 1;

    int size = (g_touch_buffer_head_index - g_touch_buffer_tail_index);

    if (size > g_touch_buffer_capacity)
    {
      g_touch_buffer_tail_index += 1;
    }
  }

  xSemaphoreGive(g_touch_buffer_semaphore);
}

bool popTailTouchBuffer(TouchData *outTouch)
{
  bool anyFound;
  xSemaphoreTake(g_touch_buffer_semaphore, portMAX_DELAY);

  const int size = (g_touch_buffer_head_index - g_touch_buffer_tail_index);
  if (size == 0)
  {
    anyFound = false;
  }
  else
  {
    anyFound = true;
    *outTouch = g_touch_buffer[g_touch_buffer_tail_index % g_touch_buffer_capacity];
    g_touch_buffer[g_touch_buffer_tail_index % g_touch_buffer_capacity].clear();
    g_touch_buffer_tail_index += 1;
  }
  xSemaphoreGive(g_touch_buffer_semaphore);

  return anyFound;
}

void testTouchBuffer()
{
  TouchData a = {100, 100, 1000, false};
  TouchData b = {150, 150, 2000, false};
  TouchData c = {200, 200, 3000, false};

  TouchData result = {};
  bool anyFound = false;

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  pushHeadTouchBuffer(a);
  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testAssert(result == a);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  pushHeadTouchBuffer(a);
  pushHeadTouchBuffer(b);
  pushHeadTouchBuffer(c);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, a);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, b);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, c);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  for (int i = 0; i < g_touch_buffer_capacity; ++i)
  {
    TouchData aN = a;
    aN.time = a.time + i;
    aN.x = a.x + i;
    pushHeadTouchBuffer(aN);
  }
  pushHeadTouchBuffer(b);
  pushHeadTouchBuffer(c);

  for (int i = 0; i < (g_touch_buffer_capacity - 2); ++i)
  {
    TouchData aN = a;
    aN.time = a.time + (i + 2);
    aN.x = a.x + (i + 2);
    anyFound = popTailTouchBuffer(&result);
    testAssert(anyFound);
    testEqual(result, aN);
  }

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, b);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, c);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  TouchData liftedA = {100, 100, 1000, true};
  TouchData liftedALater = {100, 100, 1100, true};
  TouchData liftedB = {150, 150, 2000, true};
  TouchData liftedC = {200, 200, 3000, true};

  pushHeadTouchBuffer(liftedA);
  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, liftedA);

  pushHeadTouchBuffer(liftedB);
  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, liftedB);

  pushHeadTouchBuffer(liftedC);
  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, liftedC);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  pushHeadTouchBuffer(liftedA);
  pushHeadTouchBuffer(liftedA);
  pushHeadTouchBuffer(liftedA);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, liftedA);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  pushHeadTouchBuffer(a);
  pushHeadTouchBuffer(liftedA);
  pushHeadTouchBuffer(liftedALater);
  pushHeadTouchBuffer(liftedB);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, a);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, liftedA);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, liftedB);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  pushHeadTouchBuffer(liftedA);
  pushHeadTouchBuffer(liftedB);
  pushHeadTouchBuffer(c);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, liftedA);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, liftedB);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, c);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  TouchData samePosA = {100, 100, 1500, false};
  TouchData samePosB = {150, 150, 2500, false};
  TouchData samePosC = {200, 200, 3500, false};

  pushHeadTouchBuffer(samePosA);
  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, samePosA);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  pushHeadTouchBuffer(a);
  pushHeadTouchBuffer(samePosA);
  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, samePosA);

  anyFound = popTailTouchBuffer(&result);
  result.print();
  testAssert(!anyFound);

  pushHeadTouchBuffer(a);
  pushHeadTouchBuffer(samePosA);
  pushHeadTouchBuffer(b);
  pushHeadTouchBuffer(samePosB);
  pushHeadTouchBuffer(c);
  pushHeadTouchBuffer(samePosC);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, samePosA);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, samePosB);

  anyFound = popTailTouchBuffer(&result);
  testAssert(anyFound);
  testEqual(result, samePosC);

  anyFound = popTailTouchBuffer(&result);
  testAssert(!anyFound);

  Serial.println("testTouchBuffer() succeeded");
  while (true)
    ;
}

void printTouchBuffer()
{
  for (int i = 0; i < g_touch_buffer_capacity; ++i)
  {
    Serial.print("g_touch_buffer[");
    Serial.print(i);
    Serial.print("] = ");
    g_touch_buffer[i].print();
  }
  Serial.print("g_touch_buffer_head_index = ");
  Serial.println(g_touch_buffer_head_index);
  Serial.print("g_touch_buffer_tail_index = ");
  Serial.println(g_touch_buffer_tail_index);
}

void drawCurrentTime()
{
  display.setTextColor(WHITE, BLACK);
  display.setTextWrap(false);

  display.fillRect(0, 0, display.width(), display.height(), BLACK);

  display.rtcGetRtcData();

  Serial.print("display.battery.soc() = ");
  Serial.println(display.battery.soc());

  const int batteryPercentage = ((display.battery.soc() * 100) + 32768) / 65536;

  if ((display.rtcGetMonth() == g_previous_month) &&
      (display.rtcGetDay() == g_previous_day) &&
      (batteryPercentage == g_previous_battery_percentage))
  {
    return;
  }
  g_previous_month = display.rtcGetMonth();
  g_previous_day = display.rtcGetDay();
  g_previous_battery_percentage = batteryPercentage;

  display.setCursor(30, 20);
  display.setTextSize(8);
  display.print(weekday(display.rtcGetYear(), display.rtcGetMonth() + 1, display.rtcGetDay() + 1));

  display.setCursor(510, 70);
  display.setTextSize(4);
  if (display.rtcGetHour() < 12)
  {
    display.print("AM");
  }
  else
  {
    display.print("PM");
  }

  display.setCursor(30, 105);
  display.setTextSize(8);
  const char *const months[12] = {
      "January",
      "February",
      "March",
      "April",
      "May",
      "June",
      "July",
      "August",
      "September",
      "October",
      "November",
      "December",
  };
  display.print(months[display.rtcGetMonth()]);

  display.setCursor(30, 200);
  display.setTextSize(48);
  displayToNDigits(display.rtcGetDay() + 1, 2);

  display.setCursor(500, 30);
  display.setTextSize(3);

  displayToNDigits(batteryPercentage, 2);
  display.print("%");

  display.drawRect(498, 26, 80, 30, WHITE);
  display.drawRect(497, 25, 82, 32, WHITE);
  display.fillRect(490, 31, 8, 18, WHITE);

  display.display();
}

void drawButton(const Button &button)
{
  const int left = button.left;
  const int width = button.width;
  const int right = left + width;
  const int center_x = (left + right) / 2;

  const int top = button.top;
  const int height = button.height;
  const int bottom = top + height;
  const int center_y = (top + bottom) / 2;

  switch (button.icon)
  {
  case UP_ARROW:
  {
    display.fillTriangle(left, bottom, center_x, top, right, bottom, WHITE);
  }
  break;

  case DOWN_ARROW:
  {
    display.fillTriangle(left, top, center_x, bottom, right, top, WHITE);
  }
  break;

  case OK_BUTTON:
  {
    display.fillRect(left, top, width, height, WHITE);
    display.setTextColor(BLACK, WHITE);
    display.setCursor(center_x - 40, center_y - 30);
    display.setTextSize(8);
    display.print("OK");
  }
  break;

  default:
  {
    // Should never get here.
    Serial.println("Should never get here.");
  }
  break;
  }
}

void handleTouchEvents(Button* buttons, int buttonsCount)
{
  while (true)
  {
    TouchData touch;
    const bool touchFound = popTailTouchBuffer(&touch);
    if (!touchFound)
    {
      break;
    }
    
    LOG_INT(touch.x);
    LOG_INT(touch.y);

    if (touch.lifted)
    {
      for (int i = 0; i < buttonsCount; ++i)
      {
        Button &button = buttons[i];
        button.pressed = false;
      }
    }
    else
    {
      bool anyHit = false;
      for (int i = 0; i < buttonsCount; ++i)
      {
        Button &button = buttons[i];
        if (button.contains(touch.x, touch.y, 10))
        {
          anyHit = true;
          if (!button.pressed)
          {
            Serial.println("Button not pressed");
            button.pressed = true;
            button.pressStartTime = touch.time;
            onButtonClick(button, touch);
          }
          else
          {
            Serial.println("Button pressed");
            int lastTouchTime = g_previous_touch.time - button.pressStartTime;
            int thisTouchTime = touch.time - button.pressStartTime;
            constexpr int startDelay = 1000;
            constexpr int repeatTime = 500;
            if (thisTouchTime > startDelay)
            {
              int lastRepeatIndex = (lastTouchTime - startDelay) / repeatTime;
              if (lastRepeatIndex < 0)
              {
                lastRepeatIndex = 0;
              }
              int thisRepeatIndex = (thisTouchTime - startDelay) / repeatTime;
              for (int j = lastRepeatIndex; j < thisRepeatIndex; ++j)
              {
                onButtonClick(button, touch);
              }
            }
          }
        }
      }
      if (!anyHit)
      {
        Serial.println("No button hit");
        display.fillCircle(touch.x, touch.y, 20, BLACK);
        display.fillCircle(touch.x, touch.y, 15, WHITE);
      }
    }
    g_previous_touch = touch;
  }
}

void drawSettings()
{
  display.setTextColor(WHITE, BLACK);
  display.fillRect(0, 0, display.width(), display.height(), BLACK);

  handleTouchEvents(g_settings_buttons, g_settings_buttons_count);

  display.rtcGetRtcData();

  display.setCursor(60, 80);
  display.setTextSize(8);
  displayToNDigits(display.rtcGetMonth() + 1, 2, '0');
  display.print("/");
  displayToNDigits(display.rtcGetDay() + 1, 2, '0');
  display.print("/");

  displayToNDigits(display.rtcGetYear(), 4, '0');

  const int hour24 = display.rtcGetHour();
  int hour12;
  const char* timeSuffix;
  if (hour24 < 11)
  {
      if (hour24 == 0)
      {
        hour12 = 12;
      }
      else
      {
        hour12 = hour24;
      }
      timeSuffix = "AM";
  }
  else
  {
      if (hour24 == 12)
      {
        hour12 = 12;
      }
      else
      {
        hour12 = hour24 - 12;
      }
      timeSuffix = "PM";
  }

  display.setCursor(160, 310);
  display.setTextSize(8);
  displayToNDigits(hour12, 2, '0');
  display.print(":");
  displayToNDigits(display.rtcGetMinute(), 2, '0');
  display.print(timeSuffix);

  for (int i = 0; i < g_settings_buttons_count; ++i)
  {
    const Button &button = g_settings_buttons[i];
    drawButton(button);
  }

  display.partialUpdate(false, false);
}

int mod(int x, int mod)
{
  return (x + mod) % mod;
}

void onButtonClick(const Button& button, const TouchData& touch)
{
  int weekday = display.rtcGetWeekday();
  int day = display.rtcGetDay();
  int month = display.rtcGetMonth();
  int year = display.rtcGetYear();
  int hour = display.rtcGetHour();
  int minute = display.rtcGetMinute();
  bool restart = false;

  if (strcmp(button.name, "month_up") == 0)
  {
    month += 1;
  } 
  else if (strcmp(button.name, "month_down") == 0)
  {
    month -= 1;
  }
  else if (strcmp(button.name, "day_up") == 0)
  {
    day += 1;
  }
  else if (strcmp(button.name, "day_down") == 0)
  {
    day -= 1;
  }
  else if (strcmp(button.name, "year_up") == 0)
  {
    year += 1;
  }
  else if (strcmp(button.name, "year_down") == 0)
  {
    year -= 1;
  } 
  else if (strcmp(button.name, "hour_up") == 0)
  {
    hour += 1;
  }
  else if (strcmp(button.name, "hour_down") == 0)
  {
    hour -= 1;
  } 
  else if (strcmp(button.name, "minute_up") == 0)
  {
    minute += 1;
  }
  else if (strcmp(button.name, "minute_down") == 0)
  {
    minute -= 1;
  } 
  else if (strcmp(button.name, "ampm_toggle") == 0)
  {
    hour += 12;
  }
  else if (strcmp(button.name, "ok_button") == 0)
  {
    restart = true;
  }
  else
  {
    Serial.print("Unknown button ");
    Serial.println(button.name);
  }

  month = mod(month, 12);
  day = mod(day, getMonthLength(month, year));
  hour = mod(hour, 24);
  minute = mod(minute, 60);

  display.rtcSetDate(weekday, day, month, year);
  display.rtcSetTime(hour, minute, 0);

  if (restart)
  {
    drawCurrentTime();
    ESP.restart();
  }
}

void displayToNDigits(int32_t d, int n, char pad, int base)
{
  int limit = base;
  for (int i = 0; i < (n - 2); ++i)
  {
    limit = (limit * base);
  }

  for (int i = (n - 1); i > 0; --i)
  {
    if (d < limit)
      display.print(pad);
    limit = (limit / base);
  }
  display.print(d, base);
}

void printToNDigits(int32_t d, int n, char pad, int base)
{
  int limit = base;
  for (int i = 0; i < (n - 2); ++i)
  {
    limit = (limit * base);
  }

  for (int i = (n - 1); i > 0; --i)
  {
    if (d < limit)
      Serial.print(pad);
    limit = (limit / base);
  }
  Serial.print(d, base);
}

// See https://stackoverflow.com/questions/6054016/c-program-to-find-day-of-week-given-date
const char *weekday(int year, int month, int day) {
  /* using C99 compound literals in a single line: notice the splicing */
  return ((const char *[])                                         \
          {"Monday", "Tuesday", "Wednesday",                       \
           "Thursday", "Friday", "Saturday", "Sunday"})[           \
      (                                                            \
          day                                                      \
        + ((153 * (month + 12 * ((14 - month) / 12) - 3) + 2) / 5) \
        + (365 * (year + 4800 - ((14 - month) / 12)))              \
        + ((year + 4800 - ((14 - month) / 12)) / 4)                \
        - ((year + 4800 - ((14 - month) / 12)) / 100)              \
        + ((year + 4800 - ((14 - month) / 12)) / 400)              \
        - 32045                                                    \
      ) % 7];
}

bool isLeapYear(int year)
{
  // If a year is multiple of 400, 
  // then it is a leap year 
  if (year % 400 == 0) 
      return true; 

  // Else If a year is multiple of 100, 
  // then it is not a leap year 
  else if (year % 100 == 0) 
      return false; 

  // Else If a year is multiple of 4, 
  // then it is a leap year 
  else if (year % 4 == 0) 
      return true; 
  // if no above condition is satisfied, then it is not 
  // a leap year 
  return false; 
}

// Month is zero-based: January is 0, February is 1, etc.
int getMonthLength(int month, int year)
{
  if (month > 11)
  {
    Serial.println("Month too high");
    return 31;
  }
  const int monthLengths[] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
  };
  int result = monthLengths[month];
  if ((month == 1) && isLeapYear(year))
  {
    result += 1;
  }
  return result;
}