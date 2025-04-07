### SNES RA Adapter PoC

## Proof of Concept (PoC)

This PoC, developed in a single day, demonstrates that a significant portion of the NES RA Adapter can be repurposed for another console.

The rcheevos integration, user interface, internet connectivity—everything can be reused. It will automatically fetch the game image and achievements regardless of the console.

However, the good news ends here. The Raspberry Pi Pico lacks the speed and sufficient GPIOs to properly intercept the SNES buses. My approach focused on the Address Bus A, capturing signals from A0 to A16. This should allow access to the memory-mapped WRAM for Bus A. Additionally, I used the /WD signal to detect writes, the /WRSEL signal to identify when the Working RAM is in use, and the PHI2 clock.

The cartridge identification process does not work because there are not enough pins available to read the ROM. As a result, game identification must be hardcoded into the ESP32 firmware.

snes-esp-firmware and snes-pico-firmware are forks from the respective repositories of NES at 2025-03-07

## Successful Tests:

- Captured memory writes related to menu changes [https://youtu.be/BznORG7JtVQ](https://youtu.be/BznORG7JtVQ)

- Successfully triggered an achievement in Street Fighter II [https://youtu.be/alV1YOO3Cyo](https://youtu.be/alV1YOO3Cyo)

## Unsuccessful Tests:

- Some achievements in Street Fighter II did not trigger correctly.

- Attempted to capture the Super Mario World "Jump to Yoshi" achievement. While I was able to detect the initial write to the memory address indicating that Mario is riding Yoshi at the start of the stage, I could not track it throughout the level.

## Conclusion

Adapting the NES RA Adapter for preliminary SNES testing was straightforward. With more time to study the SNES architecture, analyze signals with a logic analyzer, and utilize a more powerful microcontroller with additional GPIOs, it may be possible to develop an SNES version of the NES RA Adapter.

---

If you want to have retroachievements using your real Super Nintendo hardware, you can give a try with RA2SNES (a project from another developer)

- **RA2SNES**: [https://github.com/Factor-64/RA2Snes](https://github.com/Factor-64/RA2Snes) - RA2Snes is a program built using Qt 6.7.3 in C++ and C that bridges the QUsb2Snes webserver & rcheevos client to allow unlocking Achievements on real Super Nintendo Hardware through the SD2Snes USB port.
