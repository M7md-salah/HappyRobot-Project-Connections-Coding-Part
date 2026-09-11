# 74HC595 motor-controller wiring

This sketch drives the L298N through a **74HC595 powered at 3.3 V**. Remove the existing direct L298N-to-ESP32 wires from GPIO 2, 4, 12, and 16 before connecting the shift register.

| 74HC595 pin | Connect to |
|---|---|
| 16 (VCC) | ESP32 3.3 V |
| 8 (GND) | ESP32 GND; also connect this ground to L298N GND |
| 14 (SER / DS) | ESP32 GPIO 2 |
| 11 (SRCLK / SHCP) | ESP32 GPIO 12 |
| 12 (RCLK / STCP) | ESP32 **U0T / GPIO 1** |
| 10 (/MR) | 3.3 V |
| 13 (/OE) | 100 kΩ resistor to GND **and** 10 µF capacitor to 3.3 V (see safety note) |
| 15 (QA / Q0) | L298N IN1 |
| 1 (QB / Q1) | L298N IN2 |
| 2 (QC / Q2) | L298N IN3 |
| 3 (QD / Q3) | L298N IN4 |
| 9 (Q7S) | Leave unconnected |

Keep the L298N ENA and ENB jumpers fitted (or tie ENA and ENB to 5 V) for full-speed operation. Use a suitable separate motor supply for the L298N; do not power motors from the ESP32's 3.3 V rail. The L298N and ESP32-CAM must share GND.

## Required changes to existing wiring

- Leave the DHT data wire on **GPIO 15**; no DHT rewiring is needed.
- Add a **10 kΩ pull-down resistor** from each of GPIO 2 and GPIO 12 to GND. These ESP32-CAM pins are boot-strapping pins and must start low.
- Put a 0.1 µF ceramic capacitor between the 74HC595 VCC and GND, close to the IC.

`U0T` is the ESP32-CAM header label for GPIO 1. It is normally used for serial-upload messages, so serial text can briefly toggle the 74HC595 latch; this does not change the motor state because no shift-clock pulse accompanies it. Do not use `U0R`/GPIO 3.

## Safety

The webpage sends a motor-stop command when the joystick is released and the ESP32 also stops the motors if it does not receive another motion command within 0.7 seconds. Still, because ENA/ENB are permanently enabled, disconnect the motor battery or keep the wheels raised while first programming and testing: shift-register outputs can be indeterminate briefly during a reset. The `/OE` resistor-and-capacitor connection in the table delays output enable at cold power-up while the sketch writes its initial STOP state. For an ESP32 reset while power remains connected, it is still safest to disconnect the motor battery first.

If forward/backward or left/right is reversed, swap that motor's two wires at the L298N output, or swap that motor's IN pair in the sketch.
