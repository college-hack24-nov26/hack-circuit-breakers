
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>

#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define MAX_DEVICES 4
#define CS_PIN 10

MD_Parola display(
  HARDWARE_TYPE,
  CS_PIN,
  MAX_DEVICES
);

void setup()
{
  display.begin();

  // Display orientation
  display.setZoneEffect(0, true, PA_FLIP_UD);
  display.setZoneEffect(0, true, PA_FLIP_LR);

  // Minimum brightness
  display.setIntensity(0);

  display.displayClear();

  // Scroll RIGHT TO LEFT
  display.displayText(
    "AgriMind",
    PA_RIGHT,
    70,
    0,
    PA_SCROLL_LEFT,
    PA_SCROLL_LEFT
  );
}

void loop()
{
  if (display.displayAnimate())
  {
    display.displayReset();
  }
}
