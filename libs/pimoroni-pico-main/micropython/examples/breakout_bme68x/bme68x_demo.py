"""BME688 / BME680 demo

This demo will work for both the BME680 and BME688.
"""

import machine
import time
from breakout_bme68x import BreakoutBME68X, STATUS_HEATER_STABLE

bme = BreakoutBME68X(machine.I2C(), 0x76)
# If this gives an error, try the alternative address
# bme = BreakoutBME68X(machine.I2C(), 0x77)

while True:
    temperature, pressure, humidity, gas, status, _, _ = bme.read()
    heater = "Stable" if status & STATUS_HEATER_STABLE else "Unstable"
    print("{:0.2f}c, {:0.2f}Pa, {:0.2f}%, {:0.2f} Ohms, Heater: {}".format(
        temperature, pressure, humidity, gas, heater))
    time.sleep(1.0)
