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

#define DEFAULT_SLEEP_TIME (5 * 60 * 1000 * 1000)

typedef enum
{
  UP_ARROW,
  DOWN_ARROW
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

  bool contains(int x, int y)
  {
    return (
      (x >= this->left) &&
      (y >= this->top) &&
      (x < (this->left + this->width)) &&
      (x < (this->top + this->height))
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

void displayToNDigits(uint8_t _d, int n, char pad = ' ', int base = DEC);
void printToNDigits(uint8_t _d, int n, char pad = ' ', int base = DEC);
void touchscreenWorker(void *pvParameters);
void pushHeadTouchBuffer(const TouchData *touch);
bool popTailTouchBuffer(TouchData *touch);
void testTouchBuffer();
void printTouchBuffer();
void etchASketch();
void onButtonClick(const Button& button, const TouchData& touch);

Inkplate display(INKPLATE_1BIT); // Create an object on Inkplate library and also set library into 1-bit mode (BW)

int g_previous_battery_percentage = 0;
int g_previous_month = 0;
int g_previous_day = 0;

bool g_button_wake_up = false;

Button g_settings_buttons[] = {
    {60, 240, 100, 40, "month_up", UP_ARROW},
    {60, 370, 100, 40, "month_down", DOWN_ARROW},
    {200, 240, 100, 40, "day_up", UP_ARROW},
    {200, 370, 100, 40, "day_down", DOWN_ARROW},
    {400, 240, 100, 40, "year_up", UP_ARROW},
    {400, 370, 100, 40, "year_down", DOWN_ARROW},
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

  delay(1000);

  const int retryCount = 10;
  for (int i = 0; i < retryCount; ++i)
  {
    if (display.tsInit(true))
    {
      Serial.println("Touchscreen init ok");
      break;
    }
    else
    {
      Serial.println("Touchscreen init fail");
      if (i < (retryCount - 1))
      {
        delay(200);
      }
      else
      {
        Serial.println("All touchscreen inits failed, restarting");
        ESP.restart();
      }
    }
  }

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

  // Make sure we can wake up with the side button.
  CHECK_OK(esp_sleep_enable_ext0_wakeup(GPIO_NUM_36, 0));

  if (!display.rtcIsSet()) // Check if RTC is already is set. If ts not, set time and date
  {
    //  display.setTime(hour, minute, sec);
    display.rtcSetTime(12, 05, 00); // 24H mode, ex. 13:30:00
    //  display.setDate(weekday, day, month, yr);
    display.rtcSetDate(5, 15, 2, 2025); // 0 for Monday, ex. Monday, 17.7.2023.

    // display.rtcSetEpoch(1589610300); // Or use epoch for setting the time and date
  }

  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();

  g_button_wake_up = (wakeup_reason == ESP_SLEEP_WAKEUP_EXT0);

  if (!g_button_wake_up)
  {
    display.frontlight(false);
    drawCurrentTime(); // Display current time and date
    CHECK_OK(esp_sleep_enable_timer_wakeup(DEFAULT_SLEEP_TIME));
    esp_deep_sleep_start();
  }

  display.frontlight(true);
  display.fillRect(0, 0, display.width(), display.height(), WHITE);
  display.partialUpdate(false, false);
  g_previous_touch.lifted = true;
}

void loop()
{
  // Serial.println("loop() start");
  drawSetDate();
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

  display.fillRect(0, 0, display.width(), display.height(), BLACK);

  display.rtcGetRtcData();

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

  display.setCursor(20, 20);
  display.setTextSize(8);
  const char *const days[7] = {
      "Monday",
      "Tuesday",
      "Wednesday",
      "Thursday",
      "Friday",
      "Saturday",
      "Sunday",
  };
  display.print(days[display.rtcGetWeekday()]);

  display.setCursor(20, 105);
  display.setTextSize(8);
  const char *const months[13] = {
      "",
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

  display.setCursor(10, 200);
  display.setTextSize(48);
  displayToNDigits(display.rtcGetDay(), 2);

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

  default:
  {
    // Should never get here.
    Serial.println("Should never get here.");
  }
  break;
  }
}

void drawSetDate()
{
  display.setTextColor(WHITE, BLACK);
  display.fillRect(0, 0, display.width(), display.height(), BLACK);

  display.rtcGetRtcData();

  display.setCursor(110, 60);
  display.setTextSize(8);
  display.print("Set Date");

  display.rtcGetRtcData();

  while (true)
  {
    TouchData touch;
    const bool touchFound = popTailTouchBuffer(&touch);
    if (!touchFound)
    {
      break;
    }
    
    if (!touch.lifted)
    {
      for (int i = 0; i < g_settings_buttons_count; ++i)
      {
        Button &button = g_settings_buttons[i];
        if (button.contains(touch.x, touch.y))
        {
          if (!button.pressed)
          {
            button.pressed = true;
            button.pressStartTime = touch.time;
            onButtonClick(button, touch);
          }
          else
          {
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
    }
    g_previous_touch = touch;
  }

  display.setCursor(60, 300);
  display.setTextSize(8);
  displayToNDigits(display.rtcGetMonth(), 2, '0');
  display.print("/");
  displayToNDigits(display.rtcGetDay(), 2, '0');
  display.print("/");
  displayToNDigits(display.rtcGetYear(), 4, '0');

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
  Serial.print("Start year = ");
  Serial.println(year);

  Serial.print("onButtonClick: ");
  Serial.println(button.name);

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
  else
  {
    Serial.print("Unknown button ");
    Serial.println(button.name);
  }

  month = mod(month, 12);
  day = mod(day, 31);

  Serial.print("End year = ");
  Serial.println(year);

  display.rtcSetDate(weekday, day, month, year - 2000);
}

void displayToNDigits(uint8_t _d, int n, char pad, int base)
{
  int limit = base;
  for (int i = 0; i < (n - 2); ++i)
  {
    limit = (limit * base);
  }

  for (int i = (n - 1); i > 0; --i)
  {
    if (_d < limit)
      display.print(pad);
    limit = (limit / base);
  }
  display.print(_d, base);
}

void printToNDigits(uint8_t _d, int n, char pad, int base)
{
  int limit = base;
  for (int i = 0; i < (n - 2); ++i)
  {
    limit = (limit * base);
  }

  for (int i = (n - 1); i > 0; --i)
  {
    if (_d < limit)
      Serial.print(pad);
    limit = (limit / base);
  }
  Serial.print(_d, base);
}