# MCXC444 project

Create the FRDM-MCXC444 FreeRTOS project **in this folder** from an SDK FreeRTOS example
(MCUXpresso IDE: New project wizard, pick the FreeRTOS example from the MCUXpresso SDK).

After creating it:
1. Add `../shared` to the C include paths (Project Properties > C/C++ Build > Settings > Includes).
2. Add `shared/protocol.c` to the build (link it as a source folder/file).
3. Build with zero warnings.

`shared/protocol.h` and `shared/protocol.c` are canonical and are compiled unchanged on both boards.
