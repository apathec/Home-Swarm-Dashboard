# Contributing to Home Swarm Dashboard

Thanks for considering a contribution! This is a hobby project, so don't expect blazing-fast turnaround, but PRs and issues are very welcome.

## Reporting bugs

Open an issue with:

1. **What you're seeing** — screenshot or description of the broken behavior
2. **Hardware** — your display board (HOSYOND 4.0", or something else?), miner types
3. **Library versions** — TFT_eSPI, ArduinoJson, WiFiManager, ESP32 board package
4. **Compile error?** Paste the **first 10 lines** of the error log (the actual cause is usually near the top)
5. **Runtime error?** Open Serial Monitor at 115200 baud, capture the output, paste it

## Suggesting features

Open an issue tagged `enhancement` and describe:

- What problem the feature solves
- Roughly how you'd see it working
- Whether you'd be willing to implement it (totally fine to say no)

## Pull requests

1. Fork the repo, make a branch with a descriptive name (`fix-avalon-temp-parser`, `add-antminer-support`, etc.)
2. Keep PRs focused — one feature or fix per PR
3. Test on real hardware if possible. If you can't, say so in the PR description so reviewers know what to verify
4. Follow the existing code style (4-space indent, K&R braces, descriptive variable names)
5. Update the README if you're adding user-facing functionality

## Adding support for a new miner type

The codebase is structured to make this approachable. Steps:

1. Add a new value to the `MinerType` enum in `SwarmDashboard/config.h`
2. Update `minerTypeName()` and `colorForType()` in the main sketch to handle the new value
3. Write a new poll function in `SwarmDashboard.ino` modeled after `pollAxeOS()` (HTTP) or `pollAvalon()` (TCP)
4. Wire up your new type in the `switch (e.type)` block inside `pollAllMiners()`
5. Add the type to the `<select>` dropdown in `WebUI.h`'s `handleForm()`
6. If your miner uses an unusual protocol or response format, consider adding a separate `*API.h` file for the parser

A debug pattern that's been useful for new miners: add a `Serial.println()` of the raw response inside the parser temporarily to see what the device actually returns. We did this for the Avalon Nano 3s and it saved a lot of guesswork.

## Code style notes

- Header guards: use `#pragma once` (not `#ifndef`)
- Use `extern` declarations in `WebUI.h` to access state defined in the main sketch
- Persistent state goes through `Preferences` (NVS), not SPIFFS or EEPROM
- All HTTP UI strings should be in `F()` macros where reasonable to keep them in flash
- Avoid blocking calls longer than a few seconds in `loop()` — the web UI needs cycles too. If you do block (the polling wave does), pump `webui.loop()` inside the wait

## Testing checklist

Before opening a PR, verify:

- [ ] Code compiles cleanly with no warnings on ESP32 core 2.0.17
- [ ] Sketch fits in flash (default is ~85% used; PRs that push it over 95% probably need optimization)
- [ ] Web UI still loads and is navigable
- [ ] Existing miners still poll correctly
- [ ] Reboot survives without losing miner config
- [ ] No secrets, API keys, or personal data in committed files

## Questions?

Open an issue or comment on an existing one. Happy to help.
