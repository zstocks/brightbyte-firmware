#pragma once

// Credentials. Copy this file to secrets.h and fill it in. secrets.h is
// listed in .gitignore so it is never pushed to GitHub.

// The control app. DEVICE_TOKEN must match DEVICE_TOKEN in the
// server's .env file exactly.
#define SERVER_HOST "brightbyte.zacharystocks.com"
#define SIGN_ID "desk"
#define DEVICE_TOKEN "paste-the-device-token-here"

// Password for the sign's own "BrightByte-Setup" Wi-Fi network, which
// appears when it can't join a saved network. At least 8 characters.
#define SETUP_PASSWORD "choose-a-password"
