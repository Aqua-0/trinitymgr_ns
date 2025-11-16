# trinitymgr_ns
A mod loader for Pokemon Legends: Z-A on Nintendo Switch

Go to https://gamebanana.com/tools/21202 for use instructions

## Build
- Install devkitPro with libnx and switch ports of SDL2, SDL2_image, SDL2_ttf, libcurl.
- Ensure `DEVKITPRO` and `DEVKITARM` env vars are set.
- `make clean && make -j` to build `trinitymanager_ns.nro`.

## Notes
- Runs best as Application (title override/forwarder) to allow launching target title.
- Networking uses `socketInitializeDefault()` and `curl` to fetch thumbnails/downloads.
