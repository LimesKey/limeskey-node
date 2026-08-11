## Starting the journal - 4 hour (2026-08-07)

I thought I should probably get into some more Hack Club stuff, and add more documentation for what I'm doing so I don't forget. I'm in the process of revamping the circuit protection, for the amount of money these modules and ICs cost ~50$ CAD, I think it's wise to throw at least 5$ in circuit protection components at it. Of course the best circuit protection is just being careful and taking the proper precautions, but I'm a little silly.

I've already protected the RF frontends of the LORA and GNSS modules, with ESD diodes and current limiting, so I'm moving on to the power inputs. I'm looking at implementing the `TI TPS25947` family of eFuses, specifically the [TPS259470LRPWR](https://www.ti.com/product/TPS25947/part-details/TPS259470LRPWR), which Claude suggested. This eFuse has both an overvoltage protection feature and an overcurrent protection feature. In my GNSS module's datasheet, the NEO-M9N, it has a maximum power supply voltage of 3.6V. I can then set the eFuse's OVLO (overvoltage lockout) to cut off power when the input voltage exceeds 3.55v, just below the NEO's maximum. However, there is one issue with this. In order for the eFuse to recover and continue operation after a fault state, the 3v3 input power rail must fall below 3.25V, which would never happen in regular use. But that would just mean I have to power off the device, and turn it on, which isn't that bad considering it protected my expensive module from frying.

![KiCad Schematic TPS259470](/docs/img/Screenshot_1-2026_08_07_23:45.png)

Considering maybe adding a cheap 50-cent TVS diode to the TPS259470LRPWR input as the datasheet says may be helpful. I'll have to see.

## Finalizing circuit protection - 2 hours (2026-08-08)
* Switched from the `TPS259470LRPWR` to the `TPS259474LRPWR` for slightly better OVLO tolerances (5.5% to 1.7%), allowing the trip voltage to be more precise. 
* Added rechargable battery to V_BCKP on U2.
* Decided on using the `TPS259474ARPWR` for future buck converter output

![KiCad Schematic GNSS](/docs/img/Screenshot_1-2026_08_08_03:13.png)


## Research BMS and Charge controller choices - 2 hours (2026-08-09)
I think I've settled on using the `MAX17320Gxx`, I was thinking of initally going with the `BQ40Z50` but with that one you are forced to use some of TI's software and buy their testing equipment in order to program it. The MAX17320 is only a little more and has a whole host of nice to have features like cycle count and intelligent algorithms to calculate percentage. It is however quite expensive at 8$ CAD per IC, but I think it's worth the safety and ease of use aspects. I watched [this youtuber](https://www.youtube.com/watch?v=UUr-CJudg38) on how he designed his BMS, it was quite insightful. 

![MAX17320 schematic image](/docs/img/MAX17320.png)

### Charge Controller Selection
As for the charge controller, I went with the [BQ25798](https://www.ti.com/product/BQ25798), it's relatively cheap at $3 CAD each, it's modern and a recent design, highly efficient. It supports a wide range of input voltages (USB-C PD fast-charging!) and even can communicate with the microcontroller to set custom charge limits to preserve battery health. However, it's a little noisy with it's four switching mosfets, so it's going to take some considerations in layout as to not interrupt any data lines and RF components/traces.

My goal was to get a slightly more premium BMS that had some of the nice features you'd see in mobile phones.

## Wiring up the BMS - 3 hours (2026-08-10)

| my design | reference design |
| :---: | :---: |
| <img src="docs/img/bms-wiring.png" width="800" alt="Custom Schematic"> | <img src="docs/img/max17320-schematic-examp.png" width="800" alt="Reference Schematic"> |

I'm sure there are still some issues or things to double check, but it's a good start. I studed the example block diagram schematic from the datasheet, and a few other schematics I could find when googling "MAX17320G22+ kicad schematic". Claude said the `AON7534` N-channel mosfet would be best, so I trusted it. I'm trying to think how I'll have the thermistor placed, because I'm using two 21700 battery holders next to eachother. Perhaps I could expose some copper and use one of those SMD NTC thermistors? The batteries would then be sitting very close to the thermistor, and perhaps I could add some type of thermally conductive but not electrically conductive type of material or foam. I don't like the idea of taping the thermistor to the batteries...
