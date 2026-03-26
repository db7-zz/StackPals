import os, sys, io
import M5
from M5 import *
import time



line0 = None
line1 = None
line3 = None
line2 = None
line4 = None


import random

i = None


def setup():
  global line0, line1, line3, line2, line4, i

  M5.begin()
  Widgets.setRotation(1)
  Widgets.fillScreen(0x000000)
  line0 = Widgets.Line(29, 60, 78, 60, 0xffffff)
  line1 = Widgets.Line(138, 60, 186, 60, 0xffffff)
  line3 = Widgets.Line(221, 11, 221, 34, 0xffffff)
  line2 = Widgets.Line(103, 96, 124, 96, 0xffffff)
  line4 = Widgets.Line(213, 31, 213, 12, 0xffffff)

  Widgets.setRotation(1)


def loop():
  global line0, line1, line3, line2, line4, i
  M5.update()
  time.sleep((random.randint(1, 3)))
  line0.setVisible(False)
  line1.setVisible(False)
  time.sleep_ms(200)
  line0.setVisible(True)
  line1.setVisible(True)


if __name__ == '__main__':
  try:
    setup()
    while True:
      loop()
  except (Exception, KeyboardInterrupt) as e:
    try:
      from utility import print_error_msg
      print_error_msg(e)
    except ImportError:
      print("please update to latest firmware")
