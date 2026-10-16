# MCXC444 firmware (defuse_mcx)

`defuse_mcx/` is an MCUXpresso FreeRTOS project for the FRDM-MCXC444 (SDK_2.x_FRDM-MCXC444),
cloned from the team's Lab 3 `2271UART` project. Application code is in `defuse_mcx/source/`:

- `main.c`: board init, RGB LED (GPIO for now), FreeRTOS hooks, optional loopback self-test.
- `uart_link.c/.h`: interrupt-driven UART2 link (PTE22/PTE23, 115200 8N1) and the comm task.
- `app_config.h`: every pin, priority, stack size and queue length.
- `shared/` and `game/`: **synced copies** of the repo's `shared/` and `game/`. Never edit them
  here. Edit the originals and run `tools\sync_shared.ps1`; `tools\run_tests.ps1` fails if a
  copy is stale.

Pins and their sources: `docs/pin_table.md`.

## Import into your MCUXpresso IDE
1. File > Import > General > **Existing Projects into Workspace** > Next.
2. Select root directory: `<repo>\mcxc444\defuse_mcx`.
3. **Untick "Copy projects into workspace"**, so the IDE builds the files in this repo.
4. Finish. The project needs the FRDM-MCXC444 SDK (26.6.0) installed in the IDE.

## Build and flash
1. Select the project and click Build (hammer). Expect `0 errors, 0 warnings`.
2. Plug in the board's MCU-Link USB, then Debug (bug icon) > choose the LinkServer probe.
   The IDE creates its own launch configuration the first time.
3. Resume (F8) to run. The debug console is the MCU-Link VCOM port at 115200 baud.

Headless build (what the workhorse uses):

```bash
D:\D\MCUXpressoIDE_25.6.136\ide\mcuxpressoidec.exe -nosplash -consoleLog -application org.eclipse.cdt.managedbuilder.core.headlessbuild -data D:\D\Claude\cg2271-project\.mcux_ws -import D:\D\Claude\cg2271-project\mcxc444\defuse_mcx -build defuse_mcx/Debug
```

## Warning: do not regenerate FreeRTOS settings
`freertos/freertos-kernel/template/ARM_CM0/FreeRTOSConfig_Gen.h` is hand-edited: time slicing,
stack-overflow and malloc-failed hooks, unused features removed, heap size. Every change is
commented. **Do not open the project in Config Tools (Pins/Clocks/Peripherals) and click "Update
Code"**. That regenerates this file and the board files and silently undoes the changes.
