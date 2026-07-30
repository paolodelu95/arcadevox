// version.h — versione del firmware e sorgente degli aggiornamenti via internet.
#pragma once

// Da alzare ad ogni release: il confronto con il manifest si basa su questa.
// Formato "maggiore.minore.patch", tutti e tre i numeri 0..255.
#define FW_VERSION "1.1.0"

// Manifest degli aggiornamenti, un JSON di tre campi:
//
//   {"version":"1.2.0","url":"https://.../firmware.bin","notes":"cosa cambia"}
//
// Questo e' solo il default: dal portale se ne puo' indicare un altro, e la
// scelta finisce in NVS.
#define FW_MANIFEST_URL \
    "https://raw.githubusercontent.com/paolodelu95/arcadevox/main/firmware/manifest.json"
