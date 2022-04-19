# RetroArch 1.10.3 (dingux build) for miyoomini

Based on the dingux version of RetroArch, with video/audio/input driver customized for miyoomini
Specific differences from the dingux version are described below

## Video, Scaling 

Video driver is specialized for miyoomini, mainly uses hardware 2D graphics engine (MI_GFX)

Following 4 types of scaling are available depending on Settings > Video > Scaling:

Integer ON  + Aspect ON  ... Integer Scaling
Integer ON  + Aspect OFF ... Integer Scaling + Stretch either width or height to 4:3 aspect ratio, for CRT console emulators
Integer OFF + Aspect ON  ... Aspect Scaling
Integer OFF + Aspect OFF ... Fullscreen

Aspect/Fullscreen scaler method follows Settings > Video > Image Interpolation:

Bicubic		 : Software pre-upscaler + Hardware scaler (reduce blur)
Bilinear	 : Hardware scaler only (blur but slightly faster)
Nearest Neighbor : Software nearest neighbor scaler

NOTE: If Integer:ON and the frame is larger than 640x480, it is automatically turned OFF

Recommended settings:
- Integer ON	   : If you do not want to filter as much as possible even if the screen size is small
- Integer OFF	   : If you want to make the screen size as large as possible
- Aspect  ON	   : Recommended for LCD handheld emulators assuming perfect square pixels
- Aspect  OFF	   : Recommended for CRT console emulators assuming 4:3 screen
- Bicubic	   : Recommended in most cases
- Bilinear	   : If you want to increase the performance as much as possible even if the screen is blurry
- Nearest Neighbor : If you want to make pixels crisp anyway even if the size of each pixels are distorted

## Audio 

There are two drivers: oss and sdl

oss: Although the name oss is used, the MI_AO driver is actually used instead of oss
     Latency can be adjusted in 1ms increments, stabilizing FPS and reducing judder

sdl: Use custom SDL, latancy changes from a minimum of 21 ms in increments of about 10 ms
     Timing is a little rough, that makes fast forwarding is a little faster than oss

The latency default is 64 ms, a comfortable value with either driver

## Input, Rumble 

MENU is assigned to L3 and POWER(actually sleeps so normally unusable) to R3

Miyoomini cannot adjust the strength of the vibration,
so designed to adjust the time to vibrate after the rumble order

The vibration time from the rumble order can be adjusted with the following settings:
Settings > Input > Haptic Feedback/Vibration > Vibration Strength
 100% = 200ms, 90% = 180ms, ..., 10% = 20ms, 5% = 10ms, 0% = no vibration

Vibration strength varies greatly depending on the production lot of the miyoomini,
for devices that vibrate to the maximum, a setting of about 20% seems optimal
