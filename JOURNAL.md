## Starting the journal - 4 hour (2026-08-07)

I thought I should probably get into some more Hack Club stuff, and add more documentation for what I'm doing so I don't forget. I'm in the process of revamping the circuit protection, for the amount of money these modules and ICs cost ~50$ CAD, I think it's wise to throw at least 5$ in circuit protection components at it. Of course the best circuit protection is just being careful and taking the proper precautions, but I'm a little silly.

I've already protected the RF frontends of the LORA and GNSS modules, with ESD diodes and current limiting, so I'm moving on to the power inputs. I'm looking at implementing the `TI TPS25947` family of eFuses, specifically the [TPS259470LRPWR](https://www.ti.com/product/TPS25947/part-details/TPS259470LRPWR), which Claude suggested. This eFuse has both an overvoltage protection feature and an overcurrent protection feature. In my GNSS module's datasheet, the NEO-M9N, it has a maximum power supply voltage of 3.6V. I can then set the eFuse's OVLO (overvoltage lockout) to cut off power when the input voltage exceeds 3.55v, just below the NEO's maximum. However, there is one issue with this. In order for the eFuse to recover and continue operation after a fault state, the 3v3 input power rail must fall below 3.25V, which would never happen in regular use. But that would just mean I have to power off the device, and turn it on, which isn't that bad considering it protected my expensive module from frying.

![KiCad Schematic TPS259470](/docs/img/Screenshot_1-2026_08_07_23:45.png)

Considering maybe adding a cheap 50-cent TVS diode to the TPS259470LRPWR input as the datasheet says may be helpful. I'll have to see.
