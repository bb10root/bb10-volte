# BB10 VoLTE Addon

**Enable VoLTE on BlackBerry 10 devices.**

An addon designed to enable and improve Voice over LTE (VoLTE) functionality on supported BlackBerry 10 devices, helping extend their usability as mobile carriers phase out legacy 2G and 3G networks.

![Platform](https://img.shields.io/badge/Platform-BlackBerry%2010-000000?logo=blackberry)
![OS](https://img.shields.io/badge/OS-10.3.x-blue)
![Requirements](https://img.shields.io/badge/Requires-AutoRoot%20v4%2B-orange)
![Status](https://img.shields.io/badge/Status-Experimental-yellow)

## 📌 About

As mobile operators continue to retire older cellular networks, devices that rely on 2G or 3G for voice calls may lose their ability to make and receive calls, even when LTE data connectivity remains available.

**BB10 VoLTE Addon** aims to address this limitation by enabling VoLTE functionality on supported BlackBerry 10 hardware.

The project builds upon community research into the BlackBerry 10 VoLTE implementation and provides an installable addon distributed as a ZIP archive.

> **Note:** VoLTE availability depends on device hardware, firmware, carrier configuration, network compatibility, and operator provisioning. Installing this addon does not guarantee that VoLTE will work on every device or network.

## ✨ Features

- **VoLTE Enablement** — enables VoLTE-related functionality on supported BlackBerry 10 devices.
- **LTE Voice Calling** — aims to allow voice calls over LTE without relying on legacy 2G/3G networks.
- **Extended Device Usability** — helps keep compatible BB10 devices useful as legacy cellular networks are decommissioned.
- **Self-Provisioning** — builds the carrier list and the VoLTE profile on the phone from Android's public carrier data.
- **Easy Installation** — distributed as a ZIP package and installed through AutoRoot.
- **Community Research** — builds upon existing reverse engineering and proof-of-concept research from the BB10 community.

## 📱 Compatibility

### Requirements

- A supported BlackBerry 10 device.
- BlackBerry 10 OS **10.3.x**.
- **AutoRoot v4 or newer** installed on the device.
- An active SIM card and a mobile subscription with VoLTE enabled.
- A carrier and LTE network compatible with the device's VoLTE implementation.

### Important compatibility notes

VoLTE support varies between devices, firmware versions, and mobile operators.

Even if LTE data works correctly, voice over LTE may not work unless the carrier accepts the device's configuration and provisions the service for it.

Compatibility should be verified on each target device and network.

Outgoing calls use QoS preconditions on every network. Some networks require them: Viettel rejected outgoing calls without them. They have only been tested on Viettel, so if outgoing calls fail on your network while incoming calls work, please report it.

### Carrier provisioning

The addon keeps its own carrier list and builds the VoLTE profile for your SIM on the phone, so most carriers need no per-carrier file.

**Carrier list.** The stock list dates from 2017. On first start, and when its copy is over 30 days old, the addon downloads Android's carrier and APN lists from `android.googlesource.com`. It adds the missing carriers to a copy of the stock list and links that copy over `/base/etc/carrier/ids/carrierIds.xml`. The system partition is not modified, and the link is made again at every start. The files live in `data/`: `carrierIds.xml`, `carrierIds.stock.xml` and `carrier-apns.tsv`.

The download needs a working connection. If the phone has no working APN yet, connect to Wi-Fi or set the APN by hand once. If the download fails, the stock list stays in use and the addon tries again at the next start. To refresh the list by hand, run `ims_manager -a provision`.

**Profile.** The addon uses `data/carrier-<carrier_id>-volte.xml` if you wrote one. Otherwise it builds `data/carrier-auto-<carrier_id>-volte.xml` at start, from the stock config for that carrier, or from the stock default config plus Android's APNs. It adds an IMS service unless the stock config already has a real one.

Android's APNs replace the stock ones, which are around ten years old. A stock APN stays only if Android has no usable APN for that carrier, or doesn't give one clear answer across the networks that carrier spans. Android APNs that are IPv6-only or can't be used on LTE are skipped.

The profile is linked over `/base/etc/carrier/config/<carrier_id>.xml` and published as the carrier's normal config, so you can change the APN in Settings. A carrier with no stock config and no Android APN starts with it empty: enter it in Settings, because a guessed APN can break mobile data.

A wrong APN can drop the phone to 2G, because the same APN is used to attach to LTE. If that happens, correct it in Settings and select LTE again by hand. VoLTE then reconnects by itself, which can take a few minutes.

**SIMs with no carrier ID.** A SIM that is in neither list has no profile to link. It can only use an optional `data/carrier-default-volte.xml`, which also sets the APNs. The phone can't save APN changes while it uses this file. To add it, create the file and add this line to `pkg/data/perms`:

```text
carrier-default-volte.xml:0:0:644
```

### Forcing IMS voice (on by default)

Some networks register the device for IMS but report no IMS voice support. `ims_rrm` then refuses IMS calls and they fall back to 2G/3G, even on networks that would accept them.

To get past this, the package ships an empty `data/force-ims-voice` file. While IMS is registered on LTE, the daemon sets the `IMS_VOICE_SUPPORTED` bit that `ims_rrm` checks. When IMS is down it clears the bit, so calls can still fall back.

On the network this was tested on, calls in both directions depend on it. With it off, outgoing calls go straight to 2G/3G, and `ims_rrm` answers incoming calls with `488 Not Acceptable Here`, so they move to 2G/3G too.

This is still experimental:

- If your network refuses these calls, they may fail instead of falling back. If that happens, turn this off.
- Emergency call routing has not been checked with this bit set.

**To turn it off**, delete `force-ims-voice` from `data/` on the device and reboot, because the daemon only reads it at start. Installing the package again restores the file, so delete it again after each install, or remove the file and its line in `pkg/data/perms` before building.

## 🚀 Installation

### 1. Download the addon

Download or build the `volte.zip` archive from the project's release or distribution source.

### 2. Transfer the archive

Copy `volte.zip` to an accessible directory on your BlackBerry 10 device.

For example, you may place it in a temporary directory or another location accessible to the installer.

### 3. Open a terminal

Open a terminal directly on the device or connect remotely using SSH or Telnet, if available.

### 4. Run the installer

Execute the AutoRoot installer with the path to the ZIP archive:

```bash
/var/rootdata/installer /path/to/volte.zip
```

Replace `/path/to/volte.zip` with the actual path to the transferred archive.

Upgrading works the same way, with the daemon still running. The installer can't replace a running binary, so the package also carries it as `ims_manager.new`; `start.sh` stops the old daemon, moves the new one into place and logs `[BBE] UPDATED` in `/var/log/installer.log`. If it logs `[BBE] UPDATE_FAILED` instead, reboot and install again.

### 5. Verify functionality

After installation:

1. Follow any instructions displayed by the installer.
2. Reboot the device if required by the installation process.
3. Verify that LTE connectivity is available.
4. Check whether VoLTE is registered or enabled, where the device exposes this information.
5. Test incoming and outgoing calls while the device is connected to LTE.

**Before installing:** Back up important data and ensure you have a way to recover the device if the addon causes unexpected behavior.

## 📡 Carrier files

`ims_manager` looks up the SIM's carrier ID (MCC/MNC in `/base/etc/carrier/ids/carrierIds.xml`) and uses these files from `/var/addons/volte/data`:

- `carrier-<carrierID>-volte.xml` — the CDS carrier profile with the `ims` service. If no file matches the carrier ID, the first `carrier-*.xml` is used.
- `ir92-<carrierID>` (optional) — the IR.92 DCA object written to `/var/dca/sources/ims/services/carrier_config/ims/ir92`, replacing the default. It must start with `@ir92`. Networks that answer the INVITE with `Require: precondition` need `UsePreconditions:b:true` here; without it outgoing calls never alert the callee and end with `Ringback timer expired`. `ims_rrm` ignores `UsePreconditions:n:1`.

Add both files to `pkg/data/perms`.

## 🧪 Tests

`make test` builds and runs the host-side unit tests for the carrier helpers with the native compiler and libxml2.

## ⚠️ Disclaimer

This project is provided for research, experimentation, and preservation of BlackBerry 10 devices.

- VoLTE functionality is not guaranteed on all devices or carriers.
- Carrier-side provisioning and network restrictions may prevent calls over LTE.
- Installation may modify system files or configuration.
- Incorrect modifications may affect device stability or cellular connectivity.
- Use this addon at your own risk.

Always maintain a backup of important data and a recovery option before modifying system components.

## 🙏 Credits & Acknowledgments

This project would not be possible without the BlackBerry 10 community and its ongoing research into the platform's cellular capabilities.

Special thanks to:

- **[Qin Yu (imqinyu)](https://github.com/imqinyu)** — for the core VoLTE research and proof-of-concept work that inspired this project.
- **[Hazematman](https://github.com/Hazematman)** — for the initial findings, research, and technical insights that helped make further investigation possible.

### Research references

This project is based on and inspired by the following research:

- [BB10DeviceVoLTEResearch — Qin Yu](https://github.com/imqinyu/BB10DeviceVoLTEResearch)
- [Hazematman's BlackBerry 10 research](https://github.com/Hazematman)

Please refer to the original research for technical background and attribution.

## 📄 License

This project is licensed under the **GNU General Public License v3.0 (GPL-3.0)**.

You are free to use, study, modify, and redistribute this software under the terms of the license.

See the [LICENSE](LICENSE) file for the full license text.

---

<p align="center">
  <b>Keeping BlackBerry 10 connected.</b><br>
  Built on community research, reverse engineering, and a passion for BB10.
</p>
