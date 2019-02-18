#!/bin/bash
sudo ./led-image-viewer ../img/1.png --led-multiplexing=1 --led-gpio-mapping=adafruit-hat-pwm --led-chain=24
--led-pixel-mapper="Snake" --led-brightness=10 --led-show-refresh --led-rows=288 --led-cols=32
